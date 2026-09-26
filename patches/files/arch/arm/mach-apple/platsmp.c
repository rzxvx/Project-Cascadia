// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple S5L8942X (A5) SMP -- the second core started the way iOS starts it.
 *
 * Read out of iOS 6.1's kernelcache (docs/research/p105-smp-bringup.md):
 *
 *  - The ADT's function-enable_core is the 'Core' function of
 *    AppleS5L8940XPerformanceController on the pmgr node, called with a core
 *    mask: cpu0 1, cpu1 2.  To start a core it writes the mask to PMGR+0x1214
 *    and then PMGR+0x1220 (and 0 to +0x1204, which is not needed here and
 *    whose other bits are unknown).
 *  - The core then comes out of reset at physical 0, an alias of the first
 *    page of DRAM, where XNU's cpu_start() has put its exception vectors.
 *
 * Here that page is reserved (dts: cpu-reset@80000000, no-map, the cpus
 * node's apple,cpu-reset-page) and holds a trampoline: ldr pc, [pc, #-4]
 * and the physical address to go to.
 *
 * The PMGR also powers CPU1 off as soon as it sits in WFI, and powers it
 * back on -- through reset, so through the trampoline -- when an interrupt
 * comes for it: XNU's deep idle.  CPU0 is left alone.  (Seen with
 * tools/cpu1probe and tools/cpudbg: on WFI CPU1's debug block goes dark and
 * its SMP bit drops out of the SCU; an IPI then restarts it from the reset
 * vector.  WFE keeps it on.)  Linux idling in a plain WFI therefore lost
 * CPU1's L1, dirty lines and all, and the next interrupt sent it through
 * secondary_startup a second time -- the whole system died within
 * milliseconds.  So CPU1 idles the XNU way: its state saved by
 * cpu_suspend(), L1 cleaned and out of coherency, WFI, and the trampoline
 * points at apple_s5l_cpu_resume, which brings it back.
 */

#include <linux/bits.h>
#include <linux/cpu_pm.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/smp.h>

#include <asm/barrier.h>
#include <asm/cacheflush.h>
#include <asm/cp15.h>
#include <asm/cputype.h>
#include <asm/proc-fns.h>
#include <asm/smp_plat.h>
#include <asm/suspend.h>
#include <asm/system_misc.h>

#define PMGR_CORE_STOP		0x1210
#define PMGR_CORE_START		0x1214
#define PMGR_CORE_RUN		0x1220

/* The reset page: trampoline at 0, the address it jumps to at 4, and a
 * WFI loop for a core that is offline, should anything wake it. */
#define TRAMP_TARGET		0x04
#define TRAMP_PARK		0x40
#define ARM_LDR_PC_PC_M4	0xe51ff004	/* ldr pc, [pc, #-4] */
#define ARM_WFI			0xe320f003
#define ARM_B_MINUS_4		0xeafffffd	/* b . - 4 */

#undef MODULE_PARAM_PREFIX
#define MODULE_PARAM_PREFIX "apple_smp."

/*
 * How a core that the PMGR powers off in WFI idles:
 *   0  power down: cpu_suspend, L1 cleaned, WFI; back through reset
 *   1  poll: never WFI (for comparison -- burns the core while idle)
 *   2  plain WFI, as if the core stayed on (breaks it; for experiments)
 */
static int idle_mode;
module_param_named(idle, idle_mode, int, 0644);

/* Power-down idles that did power the core off, and those whose WFI fell
 * through (an interrupt already pending). */
static unsigned int powerdowns, wfi_returns;
module_param(powerdowns, uint, 0444);
module_param(wfi_returns, uint, 0444);

static void __iomem *pmgr_base;
static void __iomem *tramp;
static phys_addr_t reset_page;

void apple_aic1_secondary_init(unsigned int cpu);
void apple_s5l_secondary_startup(void);
void apple_s5l_cpu_resume(void);

static u32 core_mask(unsigned int cpu)
{
	return BIT(MPIDR_AFFINITY_LEVEL(cpu_logical_map(cpu), 0));
}

static void tramp_target(phys_addr_t pa)
{
	writel_relaxed(pa, tramp + TRAMP_TARGET);
	dsb(st);
}

/* On the core going down: clean L1, leave coherency, WFI.  Returns only if
 * the core was not powered off after all (an interrupt already pending). */
static int apple_s5l_powerdown(unsigned long arg)
{
	v7_exit_coherency_flush(louis);
	wfi();
	asm volatile(
	"	mrc	p15, 0, r0, c1, c0, 1\n"
	"	orr	r0, r0, #(1 << 6)\n"		/* ACTLR.SMP */
	"	mcr	p15, 0, r0, c1, c0, 1\n"
	"	isb\n"
	"	mrc	p15, 0, r0, c1, c0, 0\n"
	"	orr	r0, r0, #(1 << 2)\n"		/* SCTLR.C */
	"	mcr	p15, 0, r0, c1, c0, 0\n"
	"	isb\n"
	: : : "r0", "memory");
	return 1;
}

static void apple_s5l_idle(void)
{
	/* CPU0 stays on in WFI. */
	if (core_mask(smp_processor_id()) == BIT(0) || idle_mode == 2) {
		cpu_do_idle();
		return;
	}
	if (idle_mode == 1 || cpu_pm_enter())
		return;
	tramp_target(__pa_symbol(apple_s5l_cpu_resume));
	if (cpu_suspend(0, apple_s5l_powerdown))
		wfi_returns++;
	else
		powerdowns++;
	cpu_pm_exit();
}

static void __init apple_smp_prepare_cpus(unsigned int max_cpus)
{
	struct device_node *cpus, *page, *pmgr;
	struct resource res;

	cpus = of_find_node_by_path("/cpus");
	page = cpus ? of_parse_phandle(cpus, "apple,cpu-reset-page", 0) : NULL;
	if (page && !of_address_to_resource(page, 0, &res))
		reset_page = res.start;
	of_node_put(page);
	of_node_put(cpus);

	pmgr = of_find_compatible_node(NULL, NULL, "apple,s5l8940x-pmgr");
	if (pmgr)
		pmgr_base = of_iomap(pmgr, 0);
	of_node_put(pmgr);

	if (reset_page)
		tramp = ioremap(reset_page, 0x100);
	if (!tramp || !pmgr_base) {
		pr_err("apple-smp: no %s -- CPU1 stays off\n",
		       tramp ? "PMGR" : "apple,cpu-reset-page");
		return;
	}
	writel_relaxed(ARM_LDR_PC_PC_M4, tramp);
	writel_relaxed(ARM_WFI, tramp + TRAMP_PARK);
	writel_relaxed(ARM_B_MINUS_4, tramp + TRAMP_PARK + 4);
	tramp_target(reset_page + TRAMP_PARK);

	/* Before CPU1 can ever run: its WFI must not be Linux's plain one. */
	arm_pm_idle = apple_s5l_idle;
}

static int apple_boot_secondary(unsigned int cpu, struct task_struct *idle)
{
	u32 mask = core_mask(cpu);

	if (!tramp || !pmgr_base)
		return -ENODEV;
	/* A mask of 1 is CPU0, the one running this. */
	if (mask == BIT(0))
		return -EINVAL;

	tramp_target(__pa_symbol(apple_s5l_secondary_startup));
	writel(mask, pmgr_base + PMGR_CORE_START);
	writel(mask, pmgr_base + PMGR_CORE_RUN);
	return 0;
}

/* On the new CPU, before it takes interrupts: its AIC window made quiet. */
static void apple_secondary_init(unsigned int cpu)
{
	apple_aic1_secondary_init(cpu);
}

#ifdef CONFIG_HOTPLUG_CPU
/*
 * The CPU going down: its L1 was flushed by arch_cpu_idle_dead(), and WFI
 * powers it off.  A stray interrupt would power it on again, so the
 * trampoline sends it to the WFI loop rather than anywhere in the kernel.
 */
static void apple_cpu_die(unsigned int cpu)
{
	tramp_target(reset_page + TRAMP_PARK);
	for (;;)
		wfi();
}

/* On a surviving CPU: power the dead one off, as iOS's EnableCore(off) does. */
static int apple_cpu_kill(unsigned int cpu)
{
	u32 mask = core_mask(cpu);

	if (!pmgr_base || mask == BIT(0))
		return 0;
	writel(mask, pmgr_base + PMGR_CORE_STOP);
	return 1;
}
#endif

static const struct smp_operations apple_s5l_smp_ops __initconst = {
	.smp_prepare_cpus	= apple_smp_prepare_cpus,
	.smp_boot_secondary	= apple_boot_secondary,
	.smp_secondary_init	= apple_secondary_init,
#ifdef CONFIG_HOTPLUG_CPU
	.cpu_die		= apple_cpu_die,
	.cpu_kill		= apple_cpu_kill,
#endif
};
CPU_METHOD_OF_DECLARE(apple_pmgr_core, "apple,pmgr-core", &apple_s5l_smp_ops);
