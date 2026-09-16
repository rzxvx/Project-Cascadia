// SPDX-License-Identifier: GPL-2.0-only
/*
 * System tick from the USB Start-of-Frame interrupt (P105AP / A5).
 *
 * This is a bring-up expedient, and it is deliberate.  Every proper timer on
 * this SoC has been chased down and ruled out from the running system:
 *
 *   - Apple watchdog @0x3F103020: the counter runs at 24 MHz and the comparator
 *     provably works (armed with RESET_EN it reset the machine on schedule),
 *     but the interrupt half is not wired.  No combination of {compare at +0x04
 *     or +0x08} x {CTRL 0x1, 0x8, 0x9, 0xc} ever latched IRQ_STATUS or raised
 *     AIC hwirq 4, and the PMGR gate for its clock-id reads on.
 *   - The rest of PMGR: a scan of all 7168 words found that watchdog counter to
 *     be the only free-running register in the entire 28 KB window.
 *   - Cortex-A9 private timer @PERIPHBASE+0x600 and global timer @+0x200: both
 *     read as zero and refuse writes.  The SCU @+0x000 and the GIC CPU
 *     interface @+0x100 in the same window answer with sane, self-consistent
 *     values (SCU enabled, config 0x511 = two CPUs, CPU0 in SMP), so the window
 *     itself is alive -- PERIPHCLK simply is not supplied to the timer block.
 *   - PMU event counter overflow: PMOVSR never set, so counter 0 never even
 *     counted.  PMCCNTR works, so the cycle counter half is fine.
 *
 * What is left that demonstrably fires is the USB controller.  At high speed
 * the host sends a Start-of-Frame every 125 us, dwc2 can raise GINTSTS_SOF on
 * each one, and AIC hwirq 11 has been delivering interrupts reliably since the
 * AIC re-arm fix.  So: count SOFs, and hand the kernel a clockevent at 8 kHz.
 *
 * The honest limitations:
 *   - No tick until USB is enumerated.  That is exactly how the system boots
 *     today, so nothing regresses; jiffies simply start when the cable does.
 *   - If the host suspends the bus, SOFs stop and so does the tick.
 *   - 8000 interrupts/s is real overhead on a 1 GHz A9, a few percent.
 *
 * For a board that lives permanently tethered to a laptop -- which is how this
 * one is developed and how its console works at all -- that is a fair trade for
 * working sleep(), timeouts, preemption and, in turn, USB host and SMP.
 */

#define pr_fmt(fmt) "apple-sof-timer: " fmt

#include <linux/clockchips.h>
#include <linux/cpumask.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/processor.h>

/* One SOF per 125 us microframe at high speed. */
#define SOF_TICK_HZ		8000

static struct clock_event_device sof_clkevt;
static unsigned int sof_countdown;	/* SOFs left until the next event; 0 = idle */
static unsigned int sof_reload;		/* non-zero while periodic */
static unsigned int sof_seen;
static bool sof_live;

static inline u32 pmccntr(void)
{
	u32 v;

	asm volatile("mrc p15, 0, %0, c9, c13, 0" : "=r"(v));
	return v;
}

/**
 * apple_s5l_usb_sof_tick - called by dwc2 for every Start-of-Frame
 *
 * Runs in hard interrupt context, and deliberately OUTSIDE dwc2's own
 * hsotg->lock: the clockevent handler walks into the timer and scheduler code,
 * and holding a USB driver lock across that is asking for a lock-order
 * surprise.  Everything touched here is private to this file.
 */
void apple_s5l_usb_sof_tick(void)
{
	unsigned int n;

	sof_seen++;

	n = READ_ONCE(sof_countdown);
	if (!n)
		return;

	if (--n) {
		WRITE_ONCE(sof_countdown, n);
		return;
	}

	/* Re-arm before calling the handler: in periodic mode the handler does
	 * not call back into set_next_event. */
	WRITE_ONCE(sof_countdown, READ_ONCE(sof_reload));

	if (sof_clkevt.event_handler)
		sof_clkevt.event_handler(&sof_clkevt);
}

static int sof_set_next_event(unsigned long delta, struct clock_event_device *e)
{
	WRITE_ONCE(sof_reload, 0);
	WRITE_ONCE(sof_countdown, delta ? (unsigned int)delta : 1);
	return 0;
}

static int sof_state_shutdown(struct clock_event_device *e)
{
	WRITE_ONCE(sof_reload, 0);
	WRITE_ONCE(sof_countdown, 0);
	return 0;
}

static int sof_state_oneshot(struct clock_event_device *e)
{
	WRITE_ONCE(sof_reload, 0);
	WRITE_ONCE(sof_countdown, 0);
	return 0;
}

static int sof_state_periodic(struct clock_event_device *e)
{
	WRITE_ONCE(sof_reload, SOF_TICK_HZ / HZ);
	WRITE_ONCE(sof_countdown, SOF_TICK_HZ / HZ);
	return 0;
}

static struct clock_event_device sof_clkevt = {
	.name			= "apple-usb-sof",
	.features		= CLOCK_EVT_FEAT_ONESHOT | CLOCK_EVT_FEAT_PERIODIC,
	/* Below the PMU and watchdog attempts on purpose: if a real timer is
	 * ever found, it should win without anyone editing this file. */
	.rating			= 250,
	.set_next_event		= sof_set_next_event,
	.set_state_shutdown	= sof_state_shutdown,
	.set_state_oneshot	= sof_state_oneshot,
	.set_state_periodic	= sof_state_periodic,
};

/*
 * Register only once SOFs are demonstrably arriving.  A kernel with no
 * clockevent boots -- that is the status quo -- but a kernel whose tick never
 * fires does not, so this never registers on hope.
 *
 * By late_initcall dwc2 has probed and the host has normally reset the bus,
 * which is what starts SOFs; they do not wait for enumeration to finish.
 * jiffies are frozen here by definition, so the wait spins on the cycle
 * counter rather than using any timeout API.
 */
static int __init apple_sof_clkevt_register(void)
{
	unsigned int before = READ_ONCE(sof_seen);
	u32 start = pmccntr();
	unsigned long cpu_hz = 1000000000UL;	/* only used to size the spin */

	while ((u32)(pmccntr() - start) < cpu_hz * 2) {	/* up to ~2 s */
		if (READ_ONCE(sof_seen) != before)
			break;
		cpu_relax();
	}

	if (READ_ONCE(sof_seen) == before) {
		pr_err("SOF-TIMER: FAIL -- no Start-of-Frame seen in ~2 s. Is the cable in and dwc2 enumerated? Staying tickless.\n");
		return 0;
	}

	sof_live = true;
	sof_clkevt.cpumask = cpumask_of(0);
	clockevents_config_and_register(&sof_clkevt, SOF_TICK_HZ, 1, 0xffff);

	pr_err("SOF-TIMER: PASS -- %u SOFs counted, clockevent registered at %d Hz (%d SOFs per jiffy). jiffies are live.\n",
	       sof_seen, SOF_TICK_HZ, SOF_TICK_HZ / HZ);
	return 0;
}
late_initcall(apple_sof_clkevt_register);
