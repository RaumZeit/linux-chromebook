
GCOV_PROFILE := y
ccflags-y += -Idrivers/video/tegra/host
ccflags-y += -Idrivers/devfreq
ccflags-y += -Wno-multichar

nvhost-gk20a-y = \
	gk20a.o \
	ctrl_gk20a.o \
	fifo_gk20a.o \
	channel_gk20a.o \
	cdma_gk20a.o \
	debug_gk20a.o \
	gr_gk20a.o \
	kind_gk20a.o \
	mm_gk20a.o \
	pmu_gk20a.o \
	priv_ring_gk20a.o \
	clk_pllg.o \
	clk_gk20a.o \
	therm_gk20a.o \
	gr_ctx_gk20a_sim.o \
	gr_ctx_gk20a.o \
	gk20a_gating_reglist.o \
	gk20a_scale.o \
	gk20a_dvfs.o \
	gk20a_power.o \
	gk20a_sysfs.o

nvhost-gk20a-$(CONFIG_TEGRA_GK20A_DEBUG_SESSION) += \
	dbg_gpu_gk20a.o \
	regops_gk20a.o

obj-$(CONFIG_TEGRA_GRHOST) += nvhost-gk20a.o
