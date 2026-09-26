// SPDX-License-Identifier: GPL-2.0-only
/*
 * Clockevent from the Cortex-A9 PMU overflow interrupt (P105AP / A5).
 *
 * Why this and not a timer block
 * ------------------------------
 * There is no usable timer peripheral on this SoC as far as we can reach it.
 * The A9 private timer lives in the PERIPHBASE window, and any MMIO mapping of
 * it hangs the machine.  The watchdog at 0x3F103020 counts and its comparator
 * provably works (it resets the machine on schedule), but its interrupt half is
 * not wired -- no CTRL bit combination ever latched IRQ_STATUS or raised AIC
 * hwirq 4.  A scan of all 28 KB of the PMGR window found exactly one
 * free-running register in it, that same watchdog counter.
 *
 * The PMU is the way out because it needs no MMIO at all: everything here is
 * CP15, the same coprocessor space PMCCNTR already runs in.
 *
 * Why hwirq 135
 * -------------
 * The ADT decodes cleanly.  cpu0 has interrupts = <192, 135, 193> and cpu1 has
 * <194, 139, 195>; function-ipi_dispatch points at 192 and ipi_dispatch_other
 * at 193, so the outer two are the IPIs and the middle one is the per-CPU
 * interrupt.  aic target-destinations = <135 cpu0, 139 cpu1> pins each to its
 * own core.  On a Cortex-A9 with no architected timer, the per-CPU interrupt
 * that is left is nPMUIRQ.
 *
 * Idle
 * ----
 * The PMU counts CPU cycles, and WFI stops the core clock, so the tick would
 * die the moment the system went idle.  cpu_idle_poll_ctrl(true) keeps the idle
 * loop spinning instead.  That is a bring-up trade -- it costs power and heat --
 * and it is only switched on if the clockevent actually registers.
 *
 * CONFIG_ARM_PMU is built in, but there is no arm,cortex-a9-pmu node in the DT,
 * so the perf driver never probes and the counters are ours.
 */

#define pr_fmt(fmt) "apple-pmu-timer: " fmt

#include <linux/bits.h>
#include <linux/clockchips.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/printk.h>
#include <linux/processor.h>

#include <asm/barrier.h>

/* ARMv7 PMU, CP15 c9.  Counter 0 is used as the tick; PMCCNTR stays the
 * clocksource and is never reprogrammed here. */
#define ARMV7_EVT_CPU_CYCLES	0x11
#define PMU_TICK_CTR		0
#define PMU_TICK_BIT		BIT(PMU_TICK_CTR)

#define PMCR_E			BIT(0)	/* enable all counters */

static inline u32 pmu_rd_pmcr(void)
{ u32 v; asm volatile("mrc p15, 0, %0, c9, c12, 0" : "=r"(v)); return v; }
static inline void pmu_wr_pmcr(u32 v)
{ asm volatile("mcr p15, 0, %0, c9, c12, 0" :: "r"(v)); }
static inline void pmu_cntenset(u32 v)
{ asm volatile("mcr p15, 0, %0, c9, c12, 1" :: "r"(v)); }
static inline void pmu_cntenclr(u32 v)
{ asm volatile("mcr p15, 0, %0, c9, c12, 2" :: "r"(v)); }
static inline u32 pmu_rd_ovsr(void)
{ u32 v; asm volatile("mrc p15, 0, %0, c9, c12, 3" : "=r"(v)); return v; }
static inline void pmu_wr_ovsr(u32 v)
{ asm volatile("mcr p15, 0, %0, c9, c12, 3" :: "r"(v)); }
static inline void pmu_selr(u32 v)
{ asm volatile("mcr p15, 0, %0, c9, c12, 5" :: "r"(v)); }
static inline void pmu_evtyper(u32 v)
{ asm volatile("mcr p15, 0, %0, c9, c13, 1" :: "r"(v)); }
static inline void pmu_evcntr(u32 v)
{ asm volatile("mcr p15, 0, %0, c9, c13, 2" :: "r"(v)); }
static inline void pmu_intenset(u32 v)
{ asm volatile("mcr p15, 0, %0, c9, c14, 1" :: "r"(v)); }
static inline void pmu_intenclr(u32 v)
{ asm volatile("mcr p15, 0, %0, c9, c14, 2" :: "r"(v)); }
static inline u32 pmccntr(void)
{ u32 v; asm volatile("mrc p15, 0, %0, c9, c13, 0" : "=r"(v)); return v; }

static unsigned long pmu_rate;
static unsigned long pmu_periodic_delta;
static unsigned int pmu_irq;
static unsigned int pmu_irq_count;

static void pmu_tick_arm(unsigned long delta)
{
	pmu_cntenclr(PMU_TICK_BIT);
	pmu_selr(PMU_TICK_CTR);
	isb();
	pmu_evtyper(ARMV7_EVT_CPU_CYCLES);
	/* Count up to wrap: preload so that exactly `delta` cycles remain. */
	pmu_evcntr((u32)-(u32)delta);
	pmu_wr_ovsr(PMU_TICK_BIT);	/* drop any stale overflow */
	pmu_intenset(PMU_TICK_BIT);
	pmu_cntenset(PMU_TICK_BIT);
	pmu_wr_pmcr(pmu_rd_pmcr() | PMCR_E);
	isb();
}

static void pmu_tick_disarm(void)
{
	pmu_intenclr(PMU_TICK_BIT);
	pmu_cntenclr(PMU_TICK_BIT);
	pmu_wr_ovsr(PMU_TICK_BIT);
	isb();
}

static int pmu_set_next_event(unsigned long delta, struct clock_event_device *e)
{
	pmu_tick_arm(delta);
	return 0;
}

static int pmu_state_shutdown(struct clock_event_device *e)
{
	pmu_tick_disarm();
	return 0;
}

static int pmu_state_oneshot(struct clock_event_device *e)
{
	pmu_tick_disarm();
	return 0;
}

static int pmu_state_periodic(struct clock_event_device *e)
{
	pmu_tick_arm(pmu_periodic_delta);
	return 0;
}

static irqreturn_t pmu_tick_isr(int irq, void *dev_id)
{
	struct clock_event_device *evt = dev_id;

	if (!(pmu_rd_ovsr() & PMU_TICK_BIT))
		return IRQ_NONE;

	pmu_wr_ovsr(PMU_TICK_BIT);
	pmu_cntenclr(PMU_TICK_BIT);
	pmu_irq_count++;

	if (clockevent_state_periodic(evt))
		pmu_tick_arm(pmu_periodic_delta);

	if (evt->event_handler)
		evt->event_handler(evt);

	return IRQ_HANDLED;
}

static struct clock_event_device pmu_clkevt = {
	.name			= "apple-pmu-timer",
	.features		= CLOCK_EVT_FEAT_ONESHOT | CLOCK_EVT_FEAT_PERIODIC,
	.rating			= 400,
	.set_next_event		= pmu_set_next_event,
	.set_state_shutdown	= pmu_state_shutdown,
	.set_state_oneshot	= pmu_state_oneshot,
	.set_state_periodic	= pmu_state_periodic,
};

void __init apple_s5l_pmu_clkevt_init(unsigned long cpu_hz)
{
	struct device_node *np;

	if (!cpu_hz) {
		pr_err("no CPU rate, refusing to build a tick on a guess\n");
		return;
	}

	np = of_find_compatible_node(NULL, NULL, "apple,s5l8940x-pmu-timer");
	if (!np || !of_device_is_available(np)) {
		of_node_put(np);
		pr_info("no pmu-timer node, staying tickless\n");
		return;
	}

	pmu_irq = irq_of_parse_and_map(np, 0);
	of_node_put(np);
	if (!pmu_irq) {
		pr_err("failed to map IRQ\n");
		return;
	}

	pmu_rate = cpu_hz;
	pmu_periodic_delta = pmu_rate / HZ;

	pmu_tick_disarm();

	if (request_irq(pmu_irq, pmu_tick_isr, IRQF_TIMER | IRQF_NOBALANCING,
			"apple-pmu-timer", &pmu_clkevt)) {
		pr_err("request_irq %u failed\n", pmu_irq);
		return;
	}

	{
		u32 pmcr = pmu_rd_pmcr();
		u32 ceid0;

		asm volatile("mrc p15, 0, %0, c9, c12, 6" : "=r"(ceid0));
		/* PMCR[15:11] is N, the number of programmable event counters.  The
		 * 2026-09-15 attempt failed with PMOVSR=0, i.e. counter 0 never
		 * overflowed -- if N is 0 the counter does not exist and that is the
		 * whole story, independent of any interrupt routing. */
		pr_err("PMU-TIMER: PMCR=%#x N=%u programmable counters, PMCEID0=%#x (CPU_CYCLES %s)\n",
		       pmcr, (pmcr >> 11) & 0x1f, ceid0,
		       (ceid0 & BIT(ARMV7_EVT_CPU_CYCLES)) ? "implemented" : "MISSING");
	}

	pr_err("PMU-TIMER: irq=%u cpu=%lu Hz HZ=%d delta=%lu\n",
	       pmu_irq, pmu_rate, HZ, pmu_periodic_delta);
}

/*
 * Registration waits for interrupts to be live, exactly like the AIC
 * LATE-SMOKE test: time_init() runs from start_kernel() long before
 * local_irq_enable(), and on this port IRQs only come up in kernel_init()
 * after the AIC re-arm, so an ISR-witnessed self-test cannot pass any earlier.
 *
 * Nothing is registered unless the interrupt is observed: a kernel with no
 * clockevent boots (that is today's status quo), a kernel whose tick never
 * fires does not.
 */
static int __init apple_pmu_clkevt_register(void)
{
	unsigned int before;
	u32 start;

	if (!pmu_irq || !pmu_rate)
		return 0;

	before = pmu_irq_count;
	pmu_tick_arm(pmu_rate / 200);		/* overflow in ~5 ms */

	/* jiffies are frozen by definition here, so spin on the cycle counter. */
	start = pmccntr();
	while ((u32)(pmccntr() - start) < pmu_rate / 10) {	/* ~100 ms */
		if (READ_ONCE(pmu_irq_count) != before)
			break;
		cpu_relax();
	}

	if (READ_ONCE(pmu_irq_count) == before) {
		pmu_tick_disarm();
		pr_err("PMU-TIMER: FAIL -- no overflow interrupt on irq %u (PMOVSR=%#x). hwirq 135 is not nPMUIRQ, or the PMU IRQ is not routed to the AIC. Staying tickless.\n",
		       pmu_irq, pmu_rd_ovsr());
		return 0;
	}

	pmu_tick_disarm();

	/* WFI stops the cycle counter, which would stop the tick.  Only pay this
	 * cost once we know the tick is real. */
	cpu_idle_poll_ctrl(true);

	pmu_clkevt.cpumask = cpumask_of(0);
	pmu_clkevt.irq = pmu_irq;
	clockevents_config_and_register(&pmu_clkevt, pmu_rate, 0x1000, 0x7fffffff);

	pr_err("PMU-TIMER: PASS -- %u overflow IRQ(s) delivered. Clockevent registered at %lu Hz, idle forced to poll. jiffies are live.\n",
	       pmu_irq_count, pmu_rate);
	return 0;
}
late_initcall(apple_pmu_clkevt_register);
