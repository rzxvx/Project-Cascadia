// SPDX-License-Identifier: GPL-2.0-only
/*
 * Cortex-A9 PMCCNTR clocksource for Apple S5L894x (A5).
 *
 * A9 global/private timers never tick in Recovery (PERIPHCLK dead; CTRL
 * writes do not stick). XNU does not program them either. The cycle
 * counter does advance — use it as clocksource + sched_clock + delay.
 *
 * Rate is a fixed 1 GHz guess (A5 class). Wall time may be skewed until
 * calibrated; udelay/timekeeping still need a free-running counter.
 */

#include <linux/clocksource.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/sched_clock.h>

#include <asm/delay.h>

/* Fallback only.  apple_s5l_pmccntr_init() is handed the rate measured against
 * the watchdog's 24 MHz counter; this constant is what gets used if that
 * calibration could not run.  It was the hardcoded guess that made wall-clock
 * time wrong on this platform for the whole bring-up. */
#define PMCCNTR_RATE_FALLBACK	1000000000UL

static unsigned long apple_pmccntr_rate = PMCCNTR_RATE_FALLBACK;

static u64 notrace apple_pmccntr_read(void)
{
	u32 cyc;

	asm volatile("mrc p15, 0, %0, c9, c13, 0" : "=r"(cyc));
	return cyc;
}

static u64 apple_pmccntr_cs_read(struct clocksource *cs)
{
	return apple_pmccntr_read();
}

static struct clocksource apple_pmccntr_cs = {
	.name	= "pmccntr",
	/* Below the 24 MHz watchdog counter (350) on purpose: this counts CPU
	 * cycles and stops dead in WFI, so it is only a fallback for the case
	 * where the watchdog node is missing. */
	.rating	= 250,
	.read	= apple_pmccntr_cs_read,
	.mask	= CLOCKSOURCE_MASK(32),
	.flags	= CLOCK_SOURCE_IS_CONTINUOUS,
};

static unsigned long apple_pmccntr_read_long(void)
{
	return (unsigned long)apple_pmccntr_read();
}

static struct delay_timer apple_pmccntr_delay = {
	.read_current_timer = apple_pmccntr_read_long,
	.freq = PMCCNTR_RATE_FALLBACK,
};

static void __init apple_pmccntr_enable(void)
{
	u32 v;

	/* PMUSERENR: allow user + privileged access to PMU */
	v = 1u | (1u << 2);
	asm volatile("mcr p15, 0, %0, c9, c14, 0" :: "r"(v));

	/* PMCNTENCLR: clear enable bits before programming */
	v = 0x80000000u;
	asm volatile("mcr p15, 0, %0, c9, c12, 2" :: "r"(v));

	/* PMCR: E | P | C then leave E set */
	v = (1u << 0) | (1u << 1) | (1u << 2);
	asm volatile("mcr p15, 0, %0, c9, c12, 0" :: "r"(v));

	/* PMCNTENSET: enable cycle counter (bit 31) */
	v = 0x80000000u;
	asm volatile("mcr p15, 0, %0, c9, c12, 1" :: "r"(v));

	v = 1u;
	asm volatile("mcr p15, 0, %0, c9, c12, 0" :: "r"(v));

	isb();
}

/* Split in two so the caller can enable the counter, measure the real CPU
 * frequency against the watchdog's 24 MHz reference, and only then register
 * everything with a rate that is not a guess. */
void __init apple_s5l_pmccntr_enable_counter(void)
{
	apple_pmccntr_enable();
}

void __init apple_s5l_pmccntr_init(unsigned long rate)
{
	if (rate)
		apple_pmccntr_rate = rate;
	apple_pmccntr_delay.freq = apple_pmccntr_rate;

	pr_info("pmccntr: clocksource at %lu Hz (%s)\n", apple_pmccntr_rate,
		rate ? "measured against the 24 MHz watchdog counter" : "fallback guess");

	/* sched_clock is registered by the watchdog clocksource instead.
	 * sched_clock_register() keeps whichever rate is HIGHER, so registering
	 * PMCCNTR's 1 GHz here would permanently shut out the 24 MHz counter --
	 * and PMCCNTR freezes in idle, which is exactly what we are fixing. */
	clocksource_register_hz(&apple_pmccntr_cs, apple_pmccntr_rate);
	register_current_timer_delay(&apple_pmccntr_delay);
}
