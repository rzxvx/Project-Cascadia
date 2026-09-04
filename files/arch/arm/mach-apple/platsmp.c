// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple S5L894x (A5) SMP — PMGR Core(n) + start-addr + AIC IPI.
 *
 * enable-method: "apple,pmgr-core"
 *
 * aic1-lab v31 glass facts:
 *   - SCU already enabled (0x2D) by iBoot — do NOT ioremap CBAR (hangs).
 *   - IPI_SEND → cpu1 EVENT 0x00040001 (type=IPI, OTHER) works.
 *   - PMGR+0x6000 100A0C07→100A0C0F sticks; bit1 already set before poke.
 *   - Missing piece for second penguin: cpu1 entry PC (start-addr + reset).
 *
 * See docs/p105-smp-bringup.md.
 */

#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/smp.h>

#include <asm/cacheflush.h>
#include <asm/smp_plat.h>

#include "p105_fb_dbg.h"

#define P105_PMGR_PHYS		0x3F100000ul
#define P105_PMGR_SIZE		0x7000
#define P105_PMGR_CPU_REG	0x6000
#define P105_PMGR_APPLY_1180	0x1180
#define P105_PMGR_APPLY_1200	0x1200
#define P105_PMGR_APPLY_1204	0x1204

/* Candidate cpu1 start-address offsets (PA plant only — NOT 0x2100/2104).
 * iBSS: 0x2100/2104 are CTRL/STATUS (store 2/0x1f + poll); 0x6004 is config.
 */
static const u32 apple_pmgr_start_offs[] = {
	0x6008, 0x600c, 0x6010, 0x6004, /* 6004 unlikely; kept for rb probe */
};

static void __iomem *pmgr_base;

void apple_aic1_ipi_wake(unsigned int cpu);

static void apple_pmgr_write_start_addr(u32 pa)
{
	unsigned int i;

	if (!pmgr_base)
		return;

	for (i = 0; i < ARRAY_SIZE(apple_pmgr_start_offs); i++) {
		void __iomem *r = pmgr_base + apple_pmgr_start_offs[i];

		writel_relaxed(pa, r);
		if (readl_relaxed(r) != pa)
			writel_relaxed(pa | 1u, r);
	}
	dsb(sy);
}

/*
 * Core(n) — ADT arg is 1-based. Live v31: 0x100A0C07 → 0x100A0C0F.
 * Pulse bit(n-1) low then restore full low nibble so a parked core may reset.
 */
static int apple_pmgr_core_enable(unsigned int core_arg)
{
	u32 v, bit;

	if (!pmgr_base || core_arg < 1 || core_arg > 2)
		return -EINVAL;

	bit = BIT(core_arg - 1);

	writel_relaxed(readl_relaxed(pmgr_base + P105_PMGR_APPLY_1180) |
		       0x80000000u,
		       pmgr_base + P105_PMGR_APPLY_1180);
	writel_relaxed(0x7FFE, pmgr_base + P105_PMGR_APPLY_1200);
	writel_relaxed(0x3fff8001, pmgr_base + P105_PMGR_APPLY_1204);

	v = readl_relaxed(pmgr_base + P105_PMGR_CPU_REG);
	/* Brief clear of this core's bit (reset pulse). */
	writel_relaxed((v & ~0xFu) | ((v & 0xFu) & ~bit),
		       pmgr_base + P105_PMGR_CPU_REG);
	dsb(sy);
	udelay(50);

	v = readl_relaxed(pmgr_base + P105_PMGR_CPU_REG);
	writel_relaxed(v | 0xFu | bit, pmgr_base + P105_PMGR_CPU_REG);
	dsb(sy);
	udelay(100);

	v = readl_relaxed(pmgr_base + P105_PMGR_CPU_REG);
	p105_fb_dbg_hex("pmgr6", v);
	return 0;
}

static void __init apple_smp_prepare_cpus(unsigned int max_cpus)
{
	p105_fb_dbg("smp_prep");

	pmgr_base = ioremap(P105_PMGR_PHYS, P105_PMGR_SIZE);
	if (!pmgr_base) {
		p105_fb_dbg("pmgr_mapf");
		return;
	}

	/*
	 * SCU is already enabled by iBoot (lab SCUb=SCUa=0x2D).  Mapping any
	 * CBAR window under Linux has hung this port — skip scu_enable().
	 */
	p105_fb_dbg("scu_ibrt");

	apple_pmgr_core_enable(1);
}

static int apple_boot_secondary(unsigned int cpu, struct task_struct *idle)
{
	unsigned int core_arg = cpu_logical_map(cpu) + 1;
	u32 entry = (u32)__pa_symbol(secondary_startup);

	p105_fb_dbg_hex("boot_c", cpu);
	p105_fb_dbg_hex("entry", entry);

	/* Plant entry before Core pulse so a reset fetch can see it. */
	apple_pmgr_write_start_addr(entry);
	dsb(sy);
	isb();

	if (apple_pmgr_core_enable(core_arg))
		return -ENODEV;

	/* AIC IPI OTHER path proven in aic1-lab v31 (EV1=0x00040001). */
	apple_aic1_ipi_wake(cpu);
	arch_send_wakeup_ipi_mask(cpumask_of(cpu));
	dsb(sy);
	sev();

	return 0;
}

static const struct smp_operations apple_s5l_smp_ops __initconst = {
	.smp_prepare_cpus	= apple_smp_prepare_cpus,
	.smp_boot_secondary	= apple_boot_secondary,
};
CPU_METHOD_OF_DECLARE(apple_pmgr_core, "apple,pmgr-core", &apple_s5l_smp_ops);
