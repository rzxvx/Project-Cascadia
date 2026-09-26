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
 *  - The core then comes out of reset at the first page of DRAM, where XNU's
 *    cpu_start() has put its exception vectors.
 *
 * Here that page is reserved (dts: cpu-reset@80000000, no-map, the cpus
 * node's apple,cpu-reset-page) and its reset vector jumps to
 * secondary_startup.  Seen working with tools/cpu1probe before this was
 * written: 2 to +0x1214 and +0x1220, and CPU1 ran from that reset vector.
 */

#include <linux/bits.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/smp.h>

#include <asm/barrier.h>
#include <asm/cputype.h>
#include <asm/smp_plat.h>

#define PMGR_CORE_STOP		0x1210
#define PMGR_CORE_START		0x1214
#define PMGR_CORE_RUN		0x1220

#define ARM_LDR_PC_PC_M4	0xe51ff004	/* ldr pc, [pc, #-4] */

static void __iomem *pmgr_base;
static phys_addr_t reset_page;

void apple_aic1_secondary_init(unsigned int cpu);

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

	if (!reset_page || !pmgr_base)
		pr_err("apple-smp: no %s -- CPU1 stays off\n",
		       reset_page ? "PMGR" : "apple,cpu-reset-page");
}

static int apple_boot_secondary(unsigned int cpu, struct task_struct *idle)
{
	u32 mask = BIT(MPIDR_AFFINITY_LEVEL(cpu_logical_map(cpu), 0));
	void __iomem *vec;

	if (!reset_page || !pmgr_base)
		return -ENODEV;
	/* A mask of 1 is CPU0, the one running this. */
	if (mask == BIT(0))
		return -EINVAL;

	vec = ioremap(reset_page, 8);
	if (!vec)
		return -ENOMEM;
	writel_relaxed(ARM_LDR_PC_PC_M4, vec);
	writel_relaxed(__pa_symbol(secondary_startup), vec + 4);
	iounmap(vec);
	wmb();

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
/* The CPU going down parks itself; the next start is a reset anyway. */
static void apple_cpu_die(unsigned int cpu)
{
	for (;;)
		wfi();
}

/* On a surviving CPU: power the dead one off, as iOS's EnableCore(off) does. */
static int apple_cpu_kill(unsigned int cpu)
{
	u32 mask = BIT(MPIDR_AFFINITY_LEVEL(cpu_logical_map(cpu), 0));

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
