#include <linux/kernel.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/smp.h>
#include <linux/cpu_pm.h>
#include <linux/clockchips.h>
#include <linux/hrtimer.h>
#include <linux/debugfs.h>
#include <linux/module.h>
#include <linux/tegra-dvfs.h>
#include <linux/tegra-soc.h>
#include <linux/syscore_ops.h>
#include <linux/clk/tegra124-dfll.h>

#include <asm/smp_plat.h>

#include "sleep.h"
#include "pm.h"
#include "flowctrl.h"
#include "irq.h"

static ktime_t last_g2lp;
static unsigned int lp_cpu_max_rate;
static struct clk *cclk_g, *cclk_lp, *dfll_clk, *pll_x;
static u64 g_time, lp_time, last_update;
static DEFINE_SPINLOCK(cluster_stats_lock);

static void cluster_stats_update(void)
{
	u64 cur_time, diff;

	spin_lock(&cluster_stats_lock);
	cur_time = get_jiffies_64();
	diff = cur_time - last_update;
	if (is_lp_cluster())
		lp_time += diff;
	else
		g_time += diff;
	last_update = cur_time;
	spin_unlock(&cluster_stats_lock);
}

static inline void enable_pllx_cluster_port(void)
{
	if (is_lp_cluster())
		clk_prepare_enable(cclk_g);
	else
		clk_prepare_enable(cclk_lp);
}

static inline void disable_pllx_cluster_port(void)
{
	if (is_lp_cluster())
		clk_disable_unprepare(cclk_g);
	else
		clk_disable_unprepare(cclk_lp);
}

static void _setup_flowctrl(unsigned int flags)
{
	unsigned int target_cluster = flags & TEGRA_POWER_CLUSTER_MASK;
	unsigned int current_cluster, cpu;
	u32 reg;

	current_cluster = is_lp_cluster() ? TEGRA_POWER_CLUSTER_LP :
					TEGRA_POWER_CLUSTER_G;

	cpu = cpu_logical_map(smp_processor_id());

	/*
	 * Read the flow controler CSR register and clear the CPU switch
	 * and immediate flags. If an actual CPU switch is to be performed,
	 * re-write the CSR register with the desired values.
	 */
	reg = flowctrl_read_cpu_csr(cpu);
	reg &= ~(FLOW_CTRL_CSR_IMMEDIATE_WAKE |
		FLOW_CTRL_CSR_SWITCH_CLUSTER);

	if (flags & TEGRA_POWER_CLUSTER_IMMEDIATE)
		reg |= FLOW_CTRL_CSR_IMMEDIATE_WAKE;

	if (!target_cluster)
		goto done;

	reg &= ~FLOW_CTRL_CSR_ENABLE_EXT_MASK;
	if (flags & TEGRA_POWER_CLUSTER_PART_CRAIL) {
		if ((flags & TEGRA_POWER_CLUSTER_PART_NONCPU) == 0 &&
			(current_cluster == TEGRA_POWER_CLUSTER_LP))
			reg |= FLOW_CTRL_CSR_ENABLE_EXT_NCPU;
		else
			reg |= FLOW_CTRL_CSR_ENABLE_EXT_CRAIL;
	}

	if (flags & TEGRA_POWER_CLUSTER_PART_NONCPU)
		reg |= FLOW_CTRL_CSR_ENABLE_EXT_NCPU;

	if ((current_cluster != target_cluster) ||
		(flags & TEGRA_POWER_CLUSTER_FORCE)) {
		reg |= FLOW_CTRL_CSR_SWITCH_CLUSTER;
	}

done:
	flowctrl_write_cpu_csr(cpu, reg);
}

static void _restore_flowctrl(void)
{
	unsigned int cpu;
	u32 reg;

	cpu = cpu_logical_map(smp_processor_id());

	/*
	 * Make sure the switch and immediate flags are cleared in
	 * the flow controller to prevent undesirable side-effects
	 * for future users of the flow controller.
	 */
	reg = flowctrl_read_cpu_csr(cpu);
	reg &= ~(FLOW_CTRL_CSR_IMMEDIATE_WAKE |
		FLOW_CTRL_CSR_SWITCH_CLUSTER);
	reg &= ~FLOW_CTRL_CSR_ENABLE_EXT_MASK;

	flowctrl_write_cpu_csr(cpu, reg);
}

static int tegra_idle_power_down_last(unsigned int sleep_time,
					unsigned int flags)
{
	tegra_pmc_pm_set(TEGRA_CLUSTER_SWITCH);

	if (flags & TEGRA_POWER_CLUSTER_G) {
		if (is_lp_cluster())
			flowctrl_cpu_rail_enable();
	}

	_setup_flowctrl(flags);

	tegra_idle_last();

	_restore_flowctrl();

	return 0;
}

static int tegra_cluster_control(unsigned int flags)
{
	unsigned int target_cluster = flags & TEGRA_POWER_CLUSTER_MASK;
	unsigned int current_cluster;
	unsigned long irq_flags;
	int cpu, err = 0;

	current_cluster = is_lp_cluster() ? TEGRA_POWER_CLUSTER_LP :
					TEGRA_POWER_CLUSTER_G;

	if ((target_cluster == TEGRA_POWER_CLUSTER_MASK) || !target_cluster)
		return -EINVAL;

	if ((current_cluster == target_cluster) &&
		!(flags & TEGRA_POWER_CLUSTER_FORCE))
		return -EEXIST;

	enable_pllx_cluster_port();
	local_irq_save(irq_flags);

	if (num_online_cpus() > 1) {
		err = -EBUSY;
		goto out;
	}

	if (current_cluster != target_cluster && !timekeeping_suspended) {
		ktime_t now = ktime_get();

		if (target_cluster == TEGRA_POWER_CLUSTER_G) {
			s64 t = ktime_to_us(ktime_sub(now, last_g2lp));
			s64 t_off = 300; /* need to obtain this from DT? */

			if (t_off > t)
				udelay((unsigned int)(t_off - t));

		} else
			last_g2lp = now;
	}

	cpu = cpu_logical_map(smp_processor_id());

	tegra_set_cpu_in_lp2();
	cpu_pm_enter();

	if (!timekeeping_suspended)
		clockevents_notify(CLOCK_EVT_NOTIFY_BROADCAST_ENTER, &cpu);

	tegra_idle_power_down_last(0, flags);

	if (!is_lp_cluster())
		tegra_cluster_switch_restore_gic();

	if (!timekeeping_suspended)
		clockevents_notify(CLOCK_EVT_NOTIFY_BROADCAST_EXIT, &cpu);

	cpu_pm_exit();
	tegra_clear_cpu_in_lp2();

out:
	local_irq_restore(irq_flags);
	disable_pllx_cluster_port();

	return err;
}

int tegra_switch_cluster(int new_cluster)
{
	int err = 0;
	unsigned int flags;
	unsigned long rate;
	struct clk *new_clk;
	bool on_dfll = (clk_get_parent(cclk_g) == dfll_clk);

	flags = TEGRA_POWER_CLUSTER_IMMEDIATE;
	flags |= TEGRA_POWER_CLUSTER_PART_DEFAULT;

	if (new_cluster != TEGRA_CLUSTER_G && new_cluster != TEGRA_CLUSTER_LP)
		return -EINVAL;

	if (new_cluster == is_lp_cluster())
		return 0;

	if (new_cluster == TEGRA_CLUSTER_LP) {
		rate = clk_get_rate(cclk_g);
		if (rate > lp_cpu_max_rate) {
			pr_warn("%s: No mode switch to LP at rate %lu\n",
				__func__, rate);
			return -EINVAL;
		}
		flags |= TEGRA_POWER_CLUSTER_LP;
		new_clk = cclk_lp;
	} else {
		rate = clk_get_rate(cclk_lp);
		flags |= TEGRA_POWER_CLUSTER_G;
		new_clk = cclk_g;
	}

	if (on_dfll && new_cluster == TEGRA_CLUSTER_LP)
		tegra124_dfll_unlock_loop();

	err = clk_set_rate(new_clk, rate);
	if (err) {
		pr_err("%s: failed to set cluster clock rate: %d\n", __func__,
		       err);
		goto abort;
	}

	cluster_stats_update();

	err = tegra_cluster_control(flags);
	if (err) {
		pr_err("%s: failed to switch to %s cluster\n", __func__,
			new_cluster == TEGRA_CLUSTER_LP ? "LP" : "G");
		goto abort;
	}

	if (on_dfll && new_cluster == TEGRA_CLUSTER_G)
		tegra124_dfll_lock_loop();

	return 0;

abort:
	if (on_dfll && new_cluster == TEGRA_CLUSTER_LP)
		tegra124_dfll_lock_loop();

	pr_err("%s: aborted switch to %s cluster\n", __func__,
			new_cluster == TEGRA_CLUSTER_LP ? "LP" : "G");

	return err;
}

#ifdef CONFIG_DEBUG_FS
static int cluster_get(void *data, u64 *val)
{
	*val = (u64) is_lp_cluster();

	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(cluster_ops, cluster_get, NULL, "%llu\n");

static int cluster_stats_show(struct seq_file *s, void *data)
{
	u64 g, lp;

	cluster_stats_update();

	spin_lock(&cluster_stats_lock);
	g = g_time;
	lp = lp_time;
	spin_unlock(&cluster_stats_lock);

	seq_printf(s, "G %-10llu\n", g);
	seq_printf(s, "LP %-10llu\n", lp);

	return 0;
}

static int cluster_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, cluster_stats_show, inode->i_private);
}

static const struct file_operations cluster_stats_ops = {
	.open		= cluster_stats_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};
#endif

int tegra_cluster_control_init(void)
{
	int err = 0, num_lp_freqs;
	unsigned long *freqs_lp;
	struct clk *tmp_parent;
#ifdef CONFIG_DEBUG_FS
	struct dentry *rootdir;
#endif

	cclk_g = clk_get(NULL, "cclk_g");
	cclk_lp = clk_get(NULL, "cclk_lp");
	dfll_clk = clk_get(NULL, "dfllCPU_out");
	pll_x = clk_get(NULL, "pll_x");
	tmp_parent = clk_get(NULL, "pll_p_out4");

	if (IS_ERR(cclk_g) || IS_ERR(cclk_lp) || IS_ERR(dfll_clk) ||
	    IS_ERR(pll_x) || IS_ERR(tmp_parent)) {
		pr_err("%s: Failed to get CPU clocks\n", __func__);
		err = -EPROBE_DEFER;
		goto err;
	}

	err = tegra_dvfs_get_freqs(cclk_lp, &freqs_lp, &num_lp_freqs);
	if (err || !num_lp_freqs) {
		pr_err("%s: Failed to get LP CPU freq-table\n", __func__);
		goto err;
	}
	lp_cpu_max_rate = freqs_lp[num_lp_freqs - 1];

	/*
	 * cclk_lp may initially be parented by pll_x_out0 (i.e. pll_x/2).
	 * To bypass the divider, switch to a non-pll_x clock source and
	 * then back to pll_x.
	 */
	if (clk_get_parent(cclk_lp) != pll_x) {
		err = clk_set_parent(cclk_lp, tmp_parent);
		if (err) {
			pr_err("%s: Failed to LP CPU parent: %d\n", __func__,
			       err);
			goto err;
		}
		err = clk_set_parent(cclk_lp, pll_x);
		if (err) {
			pr_err("%s: Failed to LP CPU parent: %d\n", __func__,
			       err);
			goto err;
		}
	}
	clk_put(tmp_parent);

#ifdef CONFIG_DEBUG_FS
	rootdir = debugfs_create_dir("tegra_cluster", NULL);
	if (rootdir) {
		debugfs_create_file("current_cluster", S_IRUGO, rootdir,
					NULL, &cluster_ops);
		debugfs_create_file("time_in_state", S_IRUGO, rootdir,
					NULL, &cluster_stats_ops);
	}
#endif

	last_update = get_jiffies_64();

	return 0;

err:
	if (!IS_ERR(cclk_g))
		clk_put(cclk_g);
	if (!IS_ERR(cclk_lp))
		clk_put(cclk_lp);
	if (!IS_ERR(dfll_clk))
		clk_put(dfll_clk);
	if (!IS_ERR(pll_x))
		clk_put(pll_x);
	if (!IS_ERR(tmp_parent))
		clk_put(tmp_parent);

	return err;
}
