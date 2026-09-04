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
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqdomain.h>
#include <linux/of.h>
#include <linux/of_address.h>
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

struct apple_aic1 {
	void __iomem		*base;
	unsigned int		nr_irq;
	struct irq_domain	*domain;
};

static struct apple_aic1 *apple_aic1;

void apple_a9_gic_drain(void);
extern bool apple_aic1_early_irq_escape;

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

	pr_info("aic,1: %u IRQs, WHOAMI=%u, CONFIG=%#x (EVENT@0x5004)\n",
		aic->nr_irq, aic1_read(aic, AIC1_WHOAMI),
		aic1_read(aic, AIC1_CONFIG));

	return 0;
}

IRQCHIP_DECLARE(apple_aic1, "aic,1", aic1_of_init);
IRQCHIP_DECLARE(apple_aic1_vendor, "apple,aic1", aic1_of_init);
