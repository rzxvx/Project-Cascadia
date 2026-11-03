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

#define PMCCNTR_RATE_HZ		1000000000UL

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
	.rating	= 300,
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
	.freq = PMCCNTR_RATE_HZ,
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

void __init apple_s5l_pmccntr_init(void)
{
	apple_pmccntr_enable();

	sched_clock_register(apple_pmccntr_read, 32, PMCCNTR_RATE_HZ);
	clocksource_register_hz(&apple_pmccntr_cs, PMCCNTR_RATE_HZ);
	register_current_timer_delay(&apple_pmccntr_delay);
}
