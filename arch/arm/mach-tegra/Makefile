asflags-y				+= -march=armv7-a

obj-y                                   += io.o
obj-y                                   += irq.o
obj-y					+= pmc.o
obj-y					+= flowctrl.o
obj-y					+= powergate.o
obj-y					+= apbio.o
obj-y					+= pm.o
obj-y					+= reset.o
obj-y					+= reset-handler.o
obj-y					+= sleep.o
obj-y					+= tegra.o
obj-$(CONFIG_CPU_IDLE)			+= cpuidle.o
obj-$(CONFIG_ARCH_TEGRA_2x_SOC)		+= tegra2_emc.o
obj-$(CONFIG_ARCH_TEGRA_2x_SOC)		+= sleep-tegra20.o
obj-$(CONFIG_ARCH_TEGRA_2x_SOC)		+= pm-tegra20.o
obj-$(CONFIG_ARCH_TEGRA_2x_SOC)		+= powergate-tegra20.o
ifeq ($(CONFIG_CPU_IDLE),y)
obj-$(CONFIG_ARCH_TEGRA_2x_SOC)		+= cpuidle-tegra20.o
endif
obj-$(CONFIG_ARCH_TEGRA_3x_SOC)		+= sleep-tegra30.o
obj-$(CONFIG_ARCH_TEGRA_3x_SOC)		+= pm-tegra30.o
obj-$(CONFIG_ARCH_TEGRA_3x_SOC)		+= powergate-tegra30.o
ifeq ($(CONFIG_CPU_IDLE),y)
obj-$(CONFIG_ARCH_TEGRA_3x_SOC)		+= cpuidle-tegra30.o
endif
obj-$(CONFIG_SMP)			+= platsmp.o headsmp.o
obj-$(CONFIG_HOTPLUG_CPU)               += hotplug.o
obj-$(CONFIG_TEGRA_PCI)			+= pcie.o

obj-$(CONFIG_ARCH_TEGRA_114_SOC)	+= sleep-tegra30.o
obj-$(CONFIG_ARCH_TEGRA_114_SOC)	+= pm-tegra30.o
obj-$(CONFIG_ARCH_TEGRA_114_SOC)	+= powergate-tegra114.o
ifeq ($(CONFIG_CPU_IDLE),y)
obj-$(CONFIG_ARCH_TEGRA_114_SOC)	+= cpuidle-tegra114.o
endif
obj-$(CONFIG_ARCH_TEGRA_124_SOC)	+= sleep-tegra30.o
obj-$(CONFIG_ARCH_TEGRA_124_SOC)	+= pm-tegra30.o
obj-$(CONFIG_ARCH_TEGRA_124_SOC)	+= powergate-tegra124.o
obj-$(CONFIG_ARCH_TEGRA_124_SOC)	+= cluster-control.o
ifeq ($(CONFIG_CPU_IDLE),y)
obj-$(CONFIG_ARCH_TEGRA_124_SOC)	+= cpuidle-tegra114.o
endif

obj-$(CONFIG_ARCH_TEGRA_2x_SOC)		+= board-harmony-pcie.o

obj-$(CONFIG_ARCH_TEGRA_2x_SOC)		+= board-paz00.o
obj-$(CONFIG_TEGRA_DVFS)		+= tegra-dvfs.o
obj-$(CONFIG_TEGRA_124_DVFS)		+= tegra124-dvfs.o

obj-$(CONFIG_TEGRA_CPU_EDP_LIMITS)	+= tegra_cpu_edp.o
ifeq ($(CONFIG_TEGRA_CPU_EDP_LIMITS),y)
obj-$(CONFIG_ARCH_TEGRA_124_SOC)	+= tegra124_cpu_edp.o
endif

obj-y					+= board-venice-panel.o
