// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple Interrupt Controller revision 1 ("aic,1") — 32-bit S5L89xx (A4–A6).
 *
 * This is a custom driver for the AIC found on Apple A5/A5X and related 32-bit
 * SoCs.  It is NOT interchangeable with drivers/irqchip/irq-apple-aic.c, which
 * targets arm64 Apple Silicon (A7+) and uses a different device-tree binding
 * and interrupt specifier.
 *
 * aic1-lab (P105AP) hard-won facts:
 *   - Per-CPU EVENT is at 0x5004 + (cpu << 7); classic 0x2004 stays 0
 *   - EVENT word is type<<16 | num (HW type=1), same packing as later AIC
 *   - Read acks and auto-masks; EOI path must MASK_CLR to re-enable
 *   - SW_SET @ 0x4000 works; arm: TARGET=CPU0, MASK_CLR, SW_CLR/SET
 *   - PMGR gate 0x4D clears HWM sticky lines but kills AIC — never use
 *     while AIC must stay live
 *   - Real I=0 hold works after draining 0x5004 (no SPSR.I force needed)
 */

#define pr_fmt(fmt) "apple-aic1: " fmt

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/clockchips.h>
#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqdomain.h>
#include <linux/math64.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/processor.h>
#include <linux/slab.h>
#include <linux/smp.h>

#include <asm/exception.h>
#include <asm/ptrace.h>
#ifdef CONFIG_SMP
#include <asm/smp.h>
#endif

#define AIC1_INFO		0x0004
#define AIC1_INFO_NR_IRQ	GENMASK(15, 0)

#define AIC1_CONFIG		0x0010
#define AIC1_CONFIG_ENABLE	BIT(0)
#define AIC1_CONFIG_IMPL	0xE0000000	/* pongoOS interrupt_init() */

#define AIC1_WHOAMI		0x2000
#define AIC1_EVENT		0x2004		/* idle on aic,1; keep for drain */
#define AIC1_IPI_SEND		0x2008
#define AIC1_IPI_ACK		0x200c
#define AIC1_IPI_MASK_SET	0x2024
#define AIC1_IPI_MASK_CLR	0x2028
#define AIC1_IPI_SELF		BIT(31)
#define AIC1_IPI_OTHER		BIT(0)
#define AIC1_IPI_SEND_CPU(cpu)	BIT(cpu)

#define AIC1_TARGET_CPU		0x3000
#define AIC1_SW_SET		0x4000
#define AIC1_SW_CLR		0x4080
#define AIC1_MASK_SET		0x4100
#define AIC1_MASK_CLR		0x4180

/* Per-CPU window (lab): EVENT @ 0x5004, IPI @ 0x5008/0x500c, stride 0x80 */
#define AIC1_CPU_EVENT(cpu)	(0x5004 + ((cpu) << 7))
#define AIC1_CPU_IPI_SET(cpu)	(0x5008 + ((cpu) << 7))
#define AIC1_CPU_IPI_ACK(cpu)	(0x500c + ((cpu) << 7))
#define AIC1_CPU_IPI_MASK_SET(cpu) (0x5024 + ((cpu) << 7))
#define AIC1_CPU_IPI_MASK_CLR(cpu) (0x5028 + ((cpu) << 7))

#define AIC1_EVENT_TYPE		GENMASK(31, 16)
#define AIC1_EVENT_NUM		GENMASK(15, 0)
#define AIC1_EVENT_TYPE_HW	1
#define AIC1_EVENT_TYPE_IPI	4	/* same packing as later AIC */

#define AIC1_MAX_IRQ		1024
#define AIC1_FALLBACK_NR_IRQ	192	/* S5L894x ADT / INFO = 0xc0 */
#define AIC1_NR_SWIPI		32
#define AIC1_NR_CPUS		2

/*
 * The AIC's own timer (2026-09-18), read out of the AIC driver inside iBEC
 * 12H321 -- iBoot uses literal addresses, so unlike XNU it can simply be read.
 * The timebase was then confirmed live from Linux with peek: it runs at the
 * same 24 MHz as the watchdog counter, and its low word matches it.
 *
 *   AIC +0x0020 / +0x0028  TIME_LO / TIME_HI, 64 bits, 24 MHz.  iBoot reads it
 *                          hi, lo, hi and retries if hi moved.
 * In a local window (the current CPU's alias at 0x2000, or the explicit copy
 * at 0x5000 + (cpu << 7), which is where this driver already reads EVENT):
 *   +0x10  config: bit 0 enables the timer; iBoot's init writes 0xe
 *   +0x14  countdown: iBoot parks it at ~0, enables, clears status, then
 *          writes deadline - now in timebase ticks
 *   +0x18  status: write 1 to clear
 *   +0x1c / +0x20  local event mask set / clear; the timer is bit 1
 * It fires as EVENT 0x00070001 -- type 7, number 1 -- which the dispatcher
 * below knew nothing about before this.
 */
#define AIC1_TIME_LO		0x0020
#define AIC1_TIME_HI		0x0028
#define AIC1_TMR_CFG		0x10
#define AIC1_TMR_CFG_ENABLE	BIT(0)
#define AIC1_TMR_CFG_IBOOT	0xe
#define AIC1_TMR_CNT		0x14
#define AIC1_TMR_STAT		0x18
#define AIC1_LOCAL_MASK_SET	0x1c
#define AIC1_LOCAL_MASK_CLR	0x20
#define AIC1_LOCAL_TIMER	BIT(1)
#define AIC1_EVENT_TYPE_LOCAL	7
#define AIC1_EVENT_NUM_TIMER	1
#define AIC1_TIMER_HZ		24000000
#define AIC1_ALIAS_WINDOW	0x2000
#define AIC1_CPU_WINDOW(cpu)	(0x5000 + ((cpu) << 7))

struct apple_aic1 {
	void __iomem		*base;
	unsigned int		nr_irq;
	struct irq_domain	*domain;
};

static struct apple_aic1 *apple_aic1;

/* DIAG (2026-09-13): count every entry into aic1_handle_irq to prove
 * whether the CPU ever takes an IRQ exception from AIC1's output line.
 * Read by the late_initcall smoke-test at the bottom of the file. */
static unsigned int aic1_handler_entries;

void apple_a9_gic_drain(void);
extern bool apple_aic1_early_irq_escape;

static void aic1_tmr_event(struct apple_aic1 *aic);

static inline u32 aic1_read(struct apple_aic1 *aic, u32 reg)
{
	return readl_relaxed(aic->base + reg);
}

static inline void aic1_write(struct apple_aic1 *aic, u32 reg, u32 val)
{
	writel_relaxed(val, aic->base + reg);
}

static u32 aic1_cpu_event_off(void)
{
	return AIC1_CPU_EVENT(smp_processor_id());
}

static void aic1_mask_all(struct apple_aic1 *aic)
{
	unsigned int i, nr_words = DIV_ROUND_UP(aic->nr_irq, 32);

	for (i = 0; i < nr_words; i++) {
		aic1_write(aic, AIC1_MASK_SET + i * 4, ~0U);
		aic1_write(aic, AIC1_SW_CLR + i * 4, ~0U);
	}
}

static void aic1_target_all(struct apple_aic1 *aic, u32 dest)
{
	unsigned int i;

	for (i = 0; i < aic->nr_irq; i++)
		aic1_write(aic, AIC1_TARGET_CPU + i * 4, dest);
}

static void aic1_ipi_quiesce(struct apple_aic1 *aic)
{
	unsigned int cpu;

	aic1_write(aic, AIC1_IPI_MASK_SET, ~0U);
	aic1_write(aic, AIC1_IPI_ACK, AIC1_IPI_SELF | AIC1_IPI_OTHER);
	aic1_write(aic, AIC1_IPI_MASK_SET, ~0U);
	for (cpu = 0; cpu < AIC1_NR_CPUS; cpu++) {
		aic1_write(aic, AIC1_CPU_IPI_ACK(cpu),
			   AIC1_IPI_SELF | AIC1_IPI_OTHER);
		aic1_write(aic, AIC1_CPU_IPI_MASK_SET(cpu), ~0U);
	}
}

static u32 aic1_read_event(struct apple_aic1 *aic)
{
	u32 e5 = aic1_read(aic, aic1_cpu_event_off());

	if (e5)
		return e5;
	/* Fallback: some firmware paths may still use the shared view */
	return aic1_read(aic, AIC1_EVENT);
}

static unsigned int aic1_drain_events(struct apple_aic1 *aic)
{
	unsigned int limit = aic->nr_irq * 2 + 16;
	unsigned int n = 0;
	unsigned int cpu;

	while (n < limit) {
		u32 e = aic1_read(aic, AIC1_EVENT);
		u32 any = e;

		for (cpu = 0; cpu < AIC1_NR_CPUS; cpu++)
			any |= aic1_read(aic, AIC1_CPU_EVENT(cpu));
		if (!any)
			break;
		n++;
	}

	return n;
}

/*
 * Lab P0 / I0 HOLD: mask, TARGET=0, IPI quiesce, drain per-CPU EVENT @0x5004,
 * CFG.enable=0.  Do NOT poke PMGR gate 0x4D (kills AIC clocks).
 */
static void aic1_clear_sticky_nirq(struct apple_aic1 *aic)
{
	u32 cfg;

	aic1_mask_all(aic);
	aic1_target_all(aic, 0);
	aic1_ipi_quiesce(aic);
	aic1_drain_events(aic);
	aic1_mask_all(aic);

	cfg = aic1_read(aic, AIC1_CONFIG);
	cfg |= AIC1_CONFIG_IMPL;
	cfg &= ~AIC1_CONFIG_ENABLE;
	aic1_write(aic, AIC1_CONFIG, cfg);
}

static void aic1_enable_hw(struct apple_aic1 *aic)
{
	u32 cfg;

	cfg = aic1_read(aic, AIC1_CONFIG);
	cfg |= AIC1_CONFIG_IMPL;
	aic1_write(aic, AIC1_CONFIG, cfg);
	cfg = aic1_read(aic, AIC1_CONFIG);
	cfg |= AIC1_CONFIG_ENABLE;
	aic1_write(aic, AIC1_CONFIG, cfg);
}

/*
 * Lab: SW_SET irq0 → EVENT 0x00010000 (type=HW, num=0).
 * Accept type=HW, or a raw index < nr_irq for safety.
 */
static unsigned int aic1_event_hwirq(struct apple_aic1 *aic, u32 event)
{
	u32 type = FIELD_GET(AIC1_EVENT_TYPE, event);
	u32 num = FIELD_GET(AIC1_EVENT_NUM, event);

	if (!event)
		return aic->nr_irq;

	if (type == AIC1_EVENT_TYPE_HW) {
		if (num < aic->nr_irq)
			return num;
		return aic->nr_irq;
	}

	if (event < aic->nr_irq)
		return event;

	return aic->nr_irq;
}

void apple_aic1_quiesce(void)
{
	struct apple_aic1 *aic = apple_aic1;

	if (!aic)
		return;

	aic1_clear_sticky_nirq(aic);
}

void apple_aic1_enable(void)
{
	struct apple_aic1 *aic = apple_aic1;

	if (!aic)
		return;

	aic1_enable_hw(aic);
}

/**
 * apple_aic1_rearm - bring AIC1 back to a live-but-quiet state
 *
 * kernel_init() quiesces AIC1 (mask all, TARGET=0, CFG.ENABLE=0) right before
 * do_initcalls() and never turns it back on, so every driver that requests an
 * IRQ afterwards gets a dead controller.  Call this at the end of that quiesce:
 * every hardware line stays MASKED (drivers unmask through request_irq, so no
 * sticky-line storm), but routing is restored to CPU0, IPIs are live again and
 * CFG.ENABLE is set.
 */
void apple_aic1_rearm(void)
{
	struct apple_aic1 *aic = apple_aic1;
	unsigned int i, nr_words;

	if (!aic) {
		pr_err("AIC1-REARM: no aic instance, skipping\n");
		return;
	}

	nr_words = DIV_ROUND_UP(aic->nr_irq, 32);

	/* Keep every HW line masked and free of stale SW triggers. */
	for (i = 0; i < nr_words; i++) {
		aic1_write(aic, AIC1_MASK_SET + i * 4, ~0U);
		aic1_write(aic, AIC1_SW_CLR + i * 4, ~0U);
	}

	/* Route every line at CPU0 (quiesce zeroed all of them). */
	for (i = 0; i < aic->nr_irq; i++)
		aic1_write(aic, AIC1_TARGET_CPU + i * 4, BIT(0));

	aic1_drain_events(aic);

	/* IPIs back on for this CPU. */
	aic1_write(aic, AIC1_IPI_MASK_CLR, AIC1_IPI_OTHER | AIC1_IPI_SELF);
	aic1_write(aic, AIC1_CPU_IPI_MASK_CLR(0), ~0U);

	aic1_enable_hw(aic);
	dsb(sy);

	pr_err("AIC1-REARM: CONFIG=%#x (ENABLE=%u) all lines masked, TARGET=CPU0, IPI live\n",
	       aic1_read(aic, AIC1_CONFIG),
	       !!(aic1_read(aic, AIC1_CONFIG) & AIC1_CONFIG_ENABLE));
}

#ifdef CONFIG_SMP
static void aic1_handle_ipi(struct pt_regs *regs)
{
	struct apple_aic1 *aic = apple_aic1;

	if (!aic)
		return;

	aic1_write(aic, AIC1_IPI_ACK, AIC1_IPI_OTHER);
	ipi_mux_process();
	aic1_write(aic, AIC1_IPI_MASK_CLR, AIC1_IPI_OTHER);
}

static void aic1_ipi_send_single(unsigned int cpu)
{
	struct apple_aic1 *aic = apple_aic1;

	if (!aic || cpu >= AIC1_NR_CPUS)
		return;

	aic1_write(aic, AIC1_IPI_SEND, AIC1_IPI_SEND_CPU(cpu));
}

/** Wake a parked core via AIC IPI_SEND (lab: EV1 becomes 0x00040001). */
void apple_aic1_ipi_wake(unsigned int cpu)
{
	struct apple_aic1 *aic = apple_aic1;

	if (!aic || cpu >= AIC1_NR_CPUS)
		return;

	aic1_write(aic, AIC1_IPI_MASK_CLR, AIC1_IPI_OTHER | AIC1_IPI_SELF);
	aic1_write(aic, AIC1_CPU_IPI_MASK_CLR(cpu), ~0U);
	aic1_write(aic, AIC1_IPI_SEND, AIC1_IPI_SEND_CPU(cpu));
	dsb(sy);
}

static int __init aic1_init_smp(struct apple_aic1 *aic)
{
	int base_ipi;
	unsigned int cpu;

	base_ipi = ipi_mux_create(AIC1_NR_SWIPI, aic1_ipi_send_single);
	if (WARN_ON(base_ipi <= 0))
		return -ENODEV;

	set_smp_ipi_range(base_ipi, AIC1_NR_SWIPI);

	/* Unmask OTHER (and per-CPU windows) so cross-calls can arrive. */
	aic1_write(aic, AIC1_IPI_MASK_CLR, AIC1_IPI_OTHER | AIC1_IPI_SELF);
	for (cpu = 0; cpu < AIC1_NR_CPUS; cpu++)
		aic1_write(aic, AIC1_CPU_IPI_MASK_CLR(cpu), ~0U);

	pr_info("aic,1: SMP IPIs via IPI_SEND @0x2008 (%u vIPIs)\n",
		AIC1_NR_SWIPI);
	return 0;
}
#else
static void aic1_handle_ipi(struct pt_regs *regs) { }
static int __init aic1_init_smp(struct apple_aic1 *aic) { return 0; }
void apple_aic1_ipi_wake(unsigned int cpu) { }
#endif

/**
 * apple_aic1_release_escape - drop SPSR.I force after lab-proven drain
 *
 * Call only after aic1_clear_sticky_nirq() / hw_quiesce and a successful
 * local_irq_enable window (see irq_thaw / arm_cpu0).
 */
void apple_aic1_release_escape(void)
{
	apple_aic1_early_irq_escape = false;
}

/** Software-trigger one hwirq (lab SW_SET path). */
void apple_aic1_sw_trigger(unsigned int hwirq)
{
	struct apple_aic1 *aic = apple_aic1;

	if (!aic || hwirq >= aic->nr_irq)
		return;

	aic1_write(aic, AIC1_TARGET_CPU + hwirq * 4, BIT(0));
	aic1_write(aic, AIC1_SW_CLR + (hwirq >> 5) * 4, BIT(hwirq & 31));
	aic1_write(aic, AIC1_SW_SET + (hwirq >> 5) * 4, BIT(hwirq & 31));
	aic1_write(aic, AIC1_MASK_CLR + (hwirq >> 5) * 4, BIT(hwirq & 31));
}

static void aic1_irq_mask(struct irq_data *d)
{
	struct apple_aic1 *aic = irq_data_get_irq_chip_data(d);
	irq_hw_number_t hw = irqd_to_hwirq(d);

	aic1_write(aic, AIC1_MASK_SET + (hw >> 5) * 4, BIT(hw & 31));
}

static void aic1_irq_unmask(struct irq_data *d)
{
	struct apple_aic1 *aic = irq_data_get_irq_chip_data(d);
	irq_hw_number_t hw = irqd_to_hwirq(d);

	/* FIX (2026-09-14): the kernel_init() quiesce zeroes TARGET_CPU for every
	 * line, and nothing else restores it per line.  Unmasking without a target
	 * leaves the IRQ routed to no CPU at all, so re-assert CPU0 here. */
	aic1_write(aic, AIC1_TARGET_CPU + hw * 4, BIT(0));
	aic1_write(aic, AIC1_MASK_CLR + (hw >> 5) * 4, BIT(hw & 31));
}

static void aic1_irq_eoi(struct irq_data *d)
{
	/* EVENT read auto-masked; unmask if still wanted (lab MASK_CLR). */
	if (!irqd_irq_masked(d) && !irqd_irq_disabled(d))
		aic1_irq_unmask(d);
}

static struct irq_chip aic1_chip = {
	.name		= "APPLE-AIC1",
	.irq_mask	= aic1_irq_mask,
	.irq_unmask	= aic1_irq_unmask,
	.irq_eoi	= aic1_irq_eoi,
};

static void __exception_irq_entry aic1_handle_irq(struct pt_regs *regs)
{
	struct apple_aic1 *aic = apple_aic1;
	unsigned int limit, n = 0;
	u32 event;

	aic1_handler_entries++;

	if (apple_aic1_early_irq_escape)
		regs->ARM_cpsr |= PSR_I_BIT;

	if (!aic) {
		apple_a9_gic_drain();
		return;
	}

	apple_a9_gic_drain();
	event = aic1_read_event(aic);
	if (!event) {
		aic1_clear_sticky_nirq(aic);
		return;
	}

	limit = aic->nr_irq * 2 + 16;
	do {
		u32 type = FIELD_GET(AIC1_EVENT_TYPE, event);
		u32 num = FIELD_GET(AIC1_EVENT_NUM, event);
		unsigned int hw;

		n++;
		if (type == AIC1_EVENT_TYPE_IPI) {
			aic1_handle_ipi(regs);
			continue;
		}

		if (type == AIC1_EVENT_TYPE_LOCAL) {
			if (num == AIC1_EVENT_NUM_TIMER)
				aic1_tmr_event(aic);
			else
				pr_err_ratelimited("AIC-TIMER: unexpected local event %#x\n",
						   event);
			continue;
		}

		hw = aic1_event_hwirq(aic, event);
		if (hw < aic->nr_irq)
			generic_handle_domain_irq(aic->domain, hw);
		else if (event)
			pr_err_ratelimited("unknown EVENT %#x (type=%u num=%u)\n",
					   event, type, num);
	} while (n < limit && (event = aic1_read_event(aic)));

	if (n >= limit) {
		aic1_mask_all(aic);
		aic1_drain_events(aic);
	}
}

static int aic1_irq_domain_map(struct irq_domain *d, unsigned int virq,
			       irq_hw_number_t hw)
{
	struct apple_aic1 *aic = d->host_data;

	irq_domain_set_info(d, virq, hw, &aic1_chip, aic, handle_fasteoi_irq,
			    NULL, NULL);
	irq_set_probe(virq);

	return 0;
}

static const struct irq_domain_ops aic1_irq_domain_ops = {
	.map	= aic1_irq_domain_map,
	.xlate	= irq_domain_xlate_onecell,
};

static int __init aic1_of_init(struct device_node *node,
			       struct device_node *parent)
{
	struct apple_aic1 *aic;
	void __iomem *regs;
	u32 info;

	if (apple_aic1) {
		pr_err("only one aic,1 instance supported\n");
		return -EEXIST;
	}

	regs = of_iomap(node, 0);
	if (!regs) {
		pr_err("failed to map MMIO\n");
		return -ENOMEM;
	}

	aic = kzalloc(sizeof(*aic), GFP_KERNEL);
	if (!aic) {
		iounmap(regs);
		return -ENOMEM;
	}
	aic->base = regs;

	info = aic1_read(aic, AIC1_INFO);
	aic->nr_irq = FIELD_GET(AIC1_INFO_NR_IRQ, info);
	if (!aic->nr_irq || aic->nr_irq > AIC1_MAX_IRQ) {
		pr_warn("INFO %#010x out of range, using %u lines\n",
			info, AIC1_FALLBACK_NR_IRQ);
		aic->nr_irq = AIC1_FALLBACK_NR_IRQ;
	}

	aic1_clear_sticky_nirq(aic);

	aic->domain = irq_domain_add_linear(node, aic->nr_irq,
					    &aic1_irq_domain_ops, aic);
	if (!aic->domain) {
		pr_err("failed to create IRQ domain\n");
		kfree(aic);
		iounmap(regs);
		return -ENOMEM;
	}

	apple_aic1 = aic;
	set_handle_irq(aic1_handle_irq);

	aic1_init_smp(aic);

	/* FIX (2026-09-13): aic1_clear_sticky_nirq() above cleared CONFIG.ENABLE,
	 * so with no other caller AIC1 stays disabled and never delivers IRQs.
	 * Turn it back on now that IPI/domain setup is done. */
	aic1_enable_hw(aic);

	/* FIX (2026-09-13): drop the early-IRQ escape (forces PSR.I on handler
	 * return). Was intended to be cleared after "first enables" but the
	 * caller was never wired up. Without this, CPU stays interrupt-blind. */
	apple_aic1_early_irq_escape = false;

	pr_info("aic,1: %u IRQs, WHOAMI=%u, CONFIG=%#x (EVENT@0x5004), escape=%d\n",
		aic->nr_irq, aic1_read(aic, AIC1_WHOAMI),
		aic1_read(aic, AIC1_CONFIG), apple_aic1_early_irq_escape);

	/* DIAG-2 (2026-09-13): bulk unmask ALL IRQs — MASK register semantics
	 * are set/clr write-only (reading returns 0xffffffff on this HW),
	 * so we can't tell if MASK_CLR actually worked from readback.
	 * Just carpet-bomb: write ~0 to MASK_CLR for all banks. */
	{
		unsigned i, nr_words = DIV_ROUND_UP(aic->nr_irq, 32);
		pr_err("AIC1-DIAG: bulk unmask ALL IRQs (%u banks)\n", nr_words);
		for (i = 0; i < nr_words; i++)
			aic1_write(aic, AIC1_MASK_CLR + i * 4, ~0U);
		/* also target every IRQ at CPU 0 */
		for (i = 0; i < aic->nr_irq; i++)
			aic1_write(aic, AIC1_TARGET_CPU + i * 4, BIT(0));
	}

	/* DIAG (2026-09-13): dump CPU CPSR to check if IRQs are unmasked
	 * at CPU level. If PSR.I bit (bit 7) is set, no IRQ exception
	 * ever fires regardless of AIC1 state. */
	{
		u32 cpsr;
		asm volatile("mrs %0, cpsr" : "=r"(cpsr));
		pr_err("AIC1-DIAG: CPU CPSR=%#x  (I=%u  F=%u)\n",
		       cpsr, !!(cpsr & 0x80), !!(cpsr & 0x40));
	}

	/* DIAG (2026-09-13): fire SW_SET on hwirq 0 for pipeline test
	 * (nothing else uses IRQ 0). EVENT reg MUST show 0x10000 (type=HW num=0). */
	pr_err("AIC1-DIAG: SW-firing hwirq 0 to test delivery pipeline\n");
	{
		u32 e_pre, e_post, cfg_pre, cfg_post;

		cfg_pre = aic1_read(aic, AIC1_CONFIG);
		e_pre = aic1_read(aic, aic1_cpu_event_off());

		aic1_write(aic, AIC1_TARGET_CPU + 0 * 4, BIT(0));
		aic1_write(aic, AIC1_MASK_CLR + 0 * 4, BIT(0));
		aic1_write(aic, AIC1_SW_SET + 0 * 4, BIT(0));
		udelay(100);

		e_post = aic1_read(aic, aic1_cpu_event_off());
		cfg_post = aic1_read(aic, AIC1_CONFIG);

		pr_err("AIC1-DIAG: SW0 cfg pre=%#x post=%#x EVENT pre=%#x post=%#x\n",
		       cfg_pre, cfg_post, e_pre, e_post);
	}

	return 0;
}

/*
 * DIAG (2026-09-13): late_initcall smoke test — runs long after
 * start_kernel()'s local_irq_enable(). Reads CPSR (I bit should be
 * clear now), fires SW_SET on hwirq 0, waits, and reads EVENT +
 * handler counter to prove whether the CPU ever took the exception.
 *
 * Interpretation:
 *   - CPSR.I=1 late  -> local_irq_enable() didn't actually clear it
 *                       (patched raw_local_irq_enable somewhere?)
 *   - CPSR.I=0 late, handler_entries=0, EVENT stuck at 0x10000
 *                    -> AIC1 output line never asserts CPU nIRQ pin
 *                       (HW wiring / missing enable gate)
 *   - CPSR.I=0 late, handler_entries>0 -> pipeline works;
 *                    root cause of dead peripheral IRQs is elsewhere
 *                    (MASK/TARGET readback lies; per-line enable missing)
 */
static int __init aic1_late_smoke(void)
{
	struct apple_aic1 *aic = apple_aic1;
	u32 cpsr_before, cpsr_after, ev_before, ev_after;
	unsigned int handler_before, handler_after;

	if (!aic) {
		pr_err("LATE-SMOKE: apple_aic1 not initialized\n");
		return 0;
	}

	asm volatile("mrs %0, cpsr" : "=r"(cpsr_before));
	handler_before = aic1_handler_entries;
	ev_before = aic1_read(aic, aic1_cpu_event_off());

	pr_err("LATE-SMOKE: CPSR=%#x (I=%u F=%u) CONFIG=%#x (ENABLE=%u) EVENT=%#x handler_entries=%u\n",
	       cpsr_before, !!(cpsr_before & 0x80), !!(cpsr_before & 0x40),
	       aic1_read(aic, AIC1_CONFIG),
	       !!(aic1_read(aic, AIC1_CONFIG) & AIC1_CONFIG_ENABLE),
	       ev_before, handler_before);

	/* Make absolutely sure hwirq 0 is unmasked and targeted at CPU0 */
	aic1_write(aic, AIC1_TARGET_CPU + 0 * 4, BIT(0));
	aic1_write(aic, AIC1_MASK_CLR + 0 * 4, BIT(0));

	pr_err("LATE-SMOKE: firing SW_SET hwirq 0\n");
	aic1_write(aic, AIC1_SW_SET + 0 * 4, BIT(0));

	/* Give plenty of time for an IRQ exception to fire + handler to run.
	 * PMCCNTR clocksource means jiffies don't advance without timer IRQs,
	 * so mdelay uses PMCCNTR loops and stays honest. */
	mdelay(50);

	asm volatile("mrs %0, cpsr" : "=r"(cpsr_after));
	handler_after = aic1_handler_entries;
	ev_after = aic1_read(aic, aic1_cpu_event_off());

	pr_err("LATE-SMOKE: after SW_SET+50ms: CPSR=%#x (I=%u F=%u) EVENT=%#x handler_entries=%u (delta=%u)\n",
	       cpsr_after, !!(cpsr_after & 0x80), !!(cpsr_after & 0x40),
	       ev_after, handler_after, handler_after - handler_before);

	if (handler_after > handler_before) {
		pr_err("LATE-SMOKE: PASS -- CPU takes AIC1 IRQ exception. Look for per-line enable / MASK readback lying.\n");
	} else if (cpsr_after & 0x80) {
		pr_err("LATE-SMOKE: FAIL -- CPSR.I=1 late in boot; local_irq_enable() didn't take effect.\n");
	} else if (ev_after == 0x10000 || ev_after == 0x10001) {
		pr_err("LATE-SMOKE: FAIL -- CPSR.I=0 but AIC1 output line never reaches CPU nIRQ pin (HW gating).\n");
	} else {
		pr_err("LATE-SMOKE: WEIRD -- EVENT=%#x, needs deeper look.\n", ev_after);
	}

	/*
	 * No draining here any more.  It used to read EVENT once more "to clean
	 * up", but an EVENT read acknowledges whatever is pending and masks it --
	 * with the AIC timer live, that could swallow a tick, and a swallowed
	 * timer event stays masked, so the tick would simply stop.  The SW_SET
	 * event above has long since been taken by the handler.
	 */
	return 0;
}
late_initcall(aic1_late_smoke);

/* ------------------------------------------------------------ AIC timer -- */

/* Never 0: offset 0x10 from the base is the AIC's own CONFIG, and a stray
 * timer write there would switch the whole controller off. */
static u32 aic1_tmr_win = AIC1_ALIAS_WINDOW;
static unsigned int aic1_tmr_events;
static u64 aic1_tmr_last;
static bool aic1_tmr_registered;
static struct clock_event_device aic1_clkevt;

static inline u32 aic1_tmr_read(struct apple_aic1 *aic, u32 reg)
{
	return aic1_read(aic, aic1_tmr_win + reg);
}

static inline void aic1_tmr_write(struct apple_aic1 *aic, u32 reg, u32 val)
{
	aic1_write(aic, aic1_tmr_win + reg, val);
}

static u64 aic1_time(struct apple_aic1 *aic)
{
	u32 hi, lo, hi2;

	do {
		hi = aic1_read(aic, AIC1_TIME_HI);
		lo = aic1_read(aic, AIC1_TIME_LO);
		hi2 = aic1_read(aic, AIC1_TIME_HI);
	} while (hi != hi2);

	return ((u64)hi << 32) | lo;
}

static void aic1_tmr_disarm(struct apple_aic1 *aic)
{
	aic1_tmr_write(aic, AIC1_LOCAL_MASK_SET, AIC1_LOCAL_TIMER);
	aic1_tmr_write(aic, AIC1_TMR_CFG,
		       aic1_tmr_read(aic, AIC1_TMR_CFG) & ~AIC1_TMR_CFG_ENABLE);
	aic1_tmr_write(aic, AIC1_TMR_STAT, 1);
}

static void aic1_tmr_arm(struct apple_aic1 *aic, u32 delta)
{
	/* iBoot's order: park the countdown, enable, clear status, load. */
	aic1_tmr_write(aic, AIC1_TMR_CNT, ~0U);
	aic1_tmr_write(aic, AIC1_TMR_CFG,
		       aic1_tmr_read(aic, AIC1_TMR_CFG) | AIC1_TMR_CFG_ENABLE);
	aic1_tmr_write(aic, AIC1_TMR_STAT, 1);
	aic1_tmr_write(aic, AIC1_TMR_CNT, delta);
	aic1_tmr_write(aic, AIC1_LOCAL_MASK_CLR, AIC1_LOCAL_TIMER);
}

/*
 * Hard IRQ context, from aic1_handle_irq.  The event is masked before anything
 * else and only set_next_event unmasks it again, so a timer that refuses to
 * clear stays quiet instead of becoming an interrupt storm.
 */
static void aic1_tmr_event(struct apple_aic1 *aic)
{
	aic1_tmr_events++;
	aic1_tmr_last = aic1_time(aic);
	aic1_tmr_disarm(aic);

	if (aic1_tmr_registered && aic1_clkevt.event_handler)
		aic1_clkevt.event_handler(&aic1_clkevt);
}

static int aic1_ce_set_next_event(unsigned long delta,
				  struct clock_event_device *ce)
{
	aic1_tmr_arm(apple_aic1, delta);
	return 0;
}

static int aic1_ce_shutdown(struct clock_event_device *ce)
{
	aic1_tmr_disarm(apple_aic1);
	return 0;
}

static struct clock_event_device aic1_clkevt = {
	.name			= "apple-aic1-timer",
	.features		= CLOCK_EVT_FEAT_ONESHOT,
	/* Above the USB SOF tick (250), which stays registered as a spare. */
	.rating			= 400,
	.set_next_event		= aic1_ce_set_next_event,
	.set_state_shutdown	= aic1_ce_shutdown,
	.set_state_oneshot	= aic1_ce_shutdown,
	.tick_resume		= aic1_ce_shutdown,
};

/*
 * Arm a 10 ms shot in one window and wait up to 200 ms of timebase for the
 * event to come back through the dispatcher.  IRQs are on before any initcall;
 * the wait spins on the AIC's own counter, so it needs no working tick.
 */
static bool __init aic1_tmr_try(struct apple_aic1 *aic, u32 win,
				const char *what)
{
	unsigned int before = READ_ONCE(aic1_tmr_events);
	u32 cfg0, cnt_a, cnt_b;
	u64 t0, t;

	aic1_tmr_win = win;
	cfg0 = aic1_tmr_read(aic, AIC1_TMR_CFG);
	aic1_tmr_write(aic, AIC1_TMR_CFG, AIC1_TMR_CFG_IBOOT);

	t0 = aic1_time(aic);
	aic1_tmr_arm(aic, AIC1_TIMER_HZ / 100);
	cnt_a = aic1_tmr_read(aic, AIC1_TMR_CNT);
	do {
		if (READ_ONCE(aic1_tmr_events) != before)
			break;
		cpu_relax();
		t = aic1_time(aic);
	} while (t - t0 < AIC1_TIMER_HZ / 5);
	cnt_b = aic1_tmr_read(aic, AIC1_TMR_CNT);

	if (READ_ONCE(aic1_tmr_events) != before) {
		pr_err("AIC-TIMER: %s window %#x: a 10 ms shot fired after %llu us (CFG was %#x)\n",
		       what, win, div_u64((aic1_tmr_last - t0) * 1000000ULL, AIC1_TIMER_HZ),
		       cfg0);
		return true;
	}

	aic1_tmr_disarm(aic);
	aic1_tmr_write(aic, AIC1_TMR_CFG, cfg0);
	pr_err("AIC-TIMER: %s window %#x: nothing in 200 ms. CFG was %#x, CNT %#x -> %#x, STAT %#x\n",
	       what, win, cfg0, cnt_a, cnt_b, aic1_tmr_read(aic, AIC1_TMR_STAT));
	return false;
}

/*
 * Register only on a timer seen to fire.  Failing leaves the system exactly as
 * it was -- the SOF tick carries on -- so this cannot cost a boot.
 *
 * arch_initcall, not late: dwc2 decides whether to take Start-of-Frame
 * interrupts when it first initialises the core, at device_initcall, and it
 * asks whether this timer is live.  The AIC is re-armed and interrupts are on
 * before any initcall runs (init/main.c), so this is as early as it can be
 * and still see the timer fire.
 */
static int __init aic1_timer_init(void)
{
	struct apple_aic1 *aic = apple_aic1;

	if (!aic)
		return 0;

	pr_err("AIC-TIMER: timebase at %#llx; trying the AIC's own timer (recipe from iBEC 12H321)\n",
	       aic1_time(aic));

	if (!aic1_tmr_try(aic, AIC1_ALIAS_WINDOW, "alias") &&
	    !aic1_tmr_try(aic, AIC1_CPU_WINDOW(0), "cpu0")) {
		aic1_tmr_win = AIC1_ALIAS_WINDOW;
		pr_err("AIC-TIMER: FAIL -- no timer event in either window; the tick stays on USB SOF.\n");
		return 0;
	}

	aic1_tmr_registered = true;
	aic1_clkevt.cpumask = cpumask_of(0);
	clockevents_config_and_register(&aic1_clkevt, AIC1_TIMER_HZ, 0xf, 0x7fffffff);
#ifdef CONFIG_ARCH_APPLE_S5L
	{
		/* Tell the USB SOF fallback it is not needed, before dwc2 builds
		 * its interrupt mask -- see apple_sof_clkevt.c. */
		void apple_s5l_real_tick(void);

		apple_s5l_real_tick();
	}
#endif
	pr_err("AIC-TIMER: PASS -- clockevent registered at 24 MHz, rating %d. The tick no longer needs a USB host.\n",
	       aic1_clkevt.rating);
	return 0;
}
arch_initcall(aic1_timer_init);

IRQCHIP_DECLARE(apple_aic1, "aic,1", aic1_of_init);
IRQCHIP_DECLARE(apple_aic1_vendor, "apple,aic1", aic1_of_init);
