// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple S5L894x watchdog counter -- CPU frequency calibration.
 *
 * This file used to try to build a clockevent out of the watchdog's bark.  That
 * is a dead end on this silicon, and the negative result is worth keeping:
 *
 *   0x3F103020 is the WHOLE watchdog.  WD0/WD1 at +0x000/+0x010, which later
 *   revisions of this IP have, do not exist here: writes are dropped and reads
 *   return 0.  The ADT's reg = 0x0F103020 size 0x10 was literally the device.
 *
 *   +0x00 CUR_TIME   free-running at 24 MHz, writable
 *   +0x04 BITE_TIME  works: armed with CTRL bit 2 and a 30 s compare, the
 *                    machine reset on schedule -- so the COMPARATOR IS FINE
 *   +0x08 BARK_TIME  second compare
 *   +0x0c CTRL       implemented-bit mask 0x0d; bit 1 is write-1-clear status,
 *                    bit 2 is RESET_EN (proven), bits 0 and 3 stick but do
 *                    nothing observable
 *
 *   Every combination of {compare at +0x04 or +0x08} x {CTRL 0x1, 0x8, 0x9,
 *   0xc} was tried: IRQ_STATUS never latched and AIC hwirq 4 never fired.  The
 *   PMGR gate for the wdt's clock-id 4 reads 0x2ff (on), so it is not gating,
 *   and a scan of all 7168 words of the PMGR window found this counter to be
 *   the only free-running register in the entire 28 KB.  The compare and reset
 *   halves work; the interrupt half simply is not wired on wdt-version 1.
 *
 * What the counter is still good for: it is an exact, known 24 MHz reference,
 * which is precisely what this platform lacked.  PMCCNTR's rate was a hardcoded
 * "fixed 1 GHz guess" that made all wall-clock time wrong.  Measuring PMCCNTR
 * against this counter gives the real CPU frequency, which then feeds both the
 * clocksource and the PMU clockevent.
 *
 * The 24 MHz figure is not a datasheet number: BITE_TIME = 0x2aea5400
 * (719,479,296 ticks) reset the machine in exactly 30 s => 23.98 MHz.
 */

#define pr_fmt(fmt) "apple-wdt: " fmt

#include <linux/clocksource.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/printk.h>
#include <linux/sched_clock.h>
#include <linux/processor.h>

#define WDT_CUR_TIME		0x00
#define APPLE_WDT_REF_HZ	24000000UL

static inline u32 apple_pmccntr(void)
{
	u32 v;

	asm volatile("mrc p15, 0, %0, c9, c13, 0" : "=r"(v));
	return v;
}

/*
 * The mapping is kept for the lifetime of the system: this counter is the
 * clocksource.  NOTHING writes CUR_TIME any more -- the clockevent attempt that
 * used to zero it on every arm is gone -- so it is a pure free-running counter.
 * Do not poke 0x3F103020 from userspace unless you want to break wall time.
 */
static void __iomem *wdt_base;

static u64 notrace apple_wdt_sched_read(void)
{
	return readl_relaxed(wdt_base + WDT_CUR_TIME);
}

static u64 apple_wdt_cs_read(struct clocksource *cs)
{
	return readl_relaxed(wdt_base + WDT_CUR_TIME);
}

static struct clocksource apple_wdt_cs = {
	.name	= "apple-wdt-24m",
	/* Above pmccntr (250).  PMCCNTR counts CPU cycles and therefore STOPS in
	 * WFI: with it as the clocksource, ktime only advanced while the system
	 * was busy, so sleep(1) took 13 real seconds of mostly-idle time before
	 * the kernel agreed a second had passed.  This counter runs off the 24 MHz
	 * reference and does not care what the core is doing. */
	.rating	= 350,
	.read	= apple_wdt_cs_read,
	.mask	= CLOCKSOURCE_MASK(32),
	.flags	= CLOCK_SOURCE_IS_CONTINUOUS,
};

/**
 * apple_s5l_wdt_clocksource_init - wall time that survives idle
 *
 * Also takes over sched_clock for the same reason.  sched_clock_register()
 * refuses a lower rate than one already registered, so pmccntr.c must not
 * register it first -- see the comment there.
 */
void __init apple_s5l_wdt_clocksource_init(void)
{
	if (!wdt_base) {
		pr_err("CS: counter not mapped, wall time stays on PMCCNTR (wrong in idle)\n");
		return;
	}

	clocksource_register_hz(&apple_wdt_cs, APPLE_WDT_REF_HZ);
	sched_clock_register(apple_wdt_sched_read, 32, APPLE_WDT_REF_HZ);
	pr_err("CS: 24 MHz watchdog counter is now the clocksource and sched_clock\n");
}

/**
 * apple_s5l_calibrate_cpu_hz - measure PMCCNTR against the 24 MHz watchdog counter
 *
 * Must run with the cycle counter already enabled.  Returns 0 if the watchdog
 * node is missing, in which case the caller should fall back to its guess.
 */
unsigned long __init apple_s5l_calibrate_cpu_hz(void)
{
	struct device_node *np;
	u32 w0, w1, c0, c1, wd, cd;
	u64 hz;

	np = of_find_compatible_node(NULL, NULL, "apple,s5l8940x-wdt-timer");
	if (!np) {
		pr_err("CALIB: no wdt node; CPU rate stays a guess\n");
		return 0;
	}

	wdt_base = of_iomap(np, 0);
	of_node_put(np);
	if (!wdt_base) {
		pr_err("CALIB: cannot map wdt counter\n");
		return 0;
	}

	/* ~50 ms of reference ticks is plenty and costs nothing at boot. */
	w0 = readl_relaxed(wdt_base + WDT_CUR_TIME);
	c0 = apple_pmccntr();
	while ((u32)(readl_relaxed(wdt_base + WDT_CUR_TIME) - w0) < APPLE_WDT_REF_HZ / 20)
		cpu_relax();
	w1 = readl_relaxed(wdt_base + WDT_CUR_TIME);
	c1 = apple_pmccntr();
	/* deliberately not unmapped: the clocksource lives on this counter */

	wd = w1 - w0;
	cd = c1 - c0;
	if (!wd) {
		pr_err("CALIB: watchdog counter is not running\n");
		return 0;
	}

	hz = mul_u32_u32(cd, APPLE_WDT_REF_HZ);
	do_div(hz, wd);

	pr_err("CALIB: %u CPU cycles per %u ticks of 24 MHz => CPU = %llu Hz\n",
	       cd, wd, hz);

	return (unsigned long)hz;
}
