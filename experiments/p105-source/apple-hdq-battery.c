// SPDX-License-Identifier: GPL-2.0
/*
 * The iPad mini 1's battery gauge: a TI bq27540 on HDQ, its one-wire bus,
 * driven through UART5 the way iOS (AppleHDQGasGauge) and iBoot do it.
 *
 * UART5 is set up by iBoot for exactly this -- 57600 baud, 8 data bits, two
 * stop bits, pin 196 muxed to it -- and this driver writes the same values
 * again rather than trusting them.  One UART frame is then 11 bits, 191 us,
 * one HDQ bit cycle, and a bit is the length of the low pulse: the host
 * sends 0xFE for a 1 (start bit + one low data bit, ~35 us) and 0xE0 for a 0
 * (start + five, ~104 us).  The line is shared, so the receiver sees the host's
 * own 8 bits and then the gauge's 8, each again a low pulse whose length
 * leaves bit 2 of the received byte set for a 1 and clear for a 0.
 * A transaction starts with an explicit 200-us break and 40-us recovery.
 *
 * Only reads (command bit 7 clear) are ever sent: register numbers are the
 * bq27541 family's standard commands, 16 bits as two 8-bit reads.
 * README ("Battery gauge") has the summary.
 */
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/ktime.h>
#include <linux/spinlock.h>
#include <linux/pm.h>
#include <linux/slab.h>
#include "apple-hdq-snapshot.h"

#define ULCON 0x00
#define UCON 0x04
#define UCON_SBREAK BIT(4)
#define UFCON 0x08
#define UTRSTAT 0x10
#define UTRSTAT_RXRDY BIT(0) /* Apple's UFSTAT layout differs from Samsung's */
#define UTXH 0x20
#define URXH 0x24
#define UBRDIV 0x28
#define UFRACVAL 0x2c

/* iBoot's setup: 8N2, RX/TX polled, FIFO on, 24 MHz / 16 / 26 = 57692 baud */
#define HDQ_ULCON 0x07
#define HDQ_UCON 0x405
#define HDQ_UFCON 0x01
#define HDQ_UBRDIV 25

/* bq27541-family standard commands */
#define BQ_TEMP 0x06 /* 0.1 K */
#define BQ_VOLT 0x08 /* mV */
#define BQ_FLAGS 0x0a
#define BQ_FLAG_DSG BIT(0)
#define BQ_FLAG_FC BIT(9)
#define BQ_RM 0x10 /* mAh */
#define BQ_FCC 0x12 /* mAh */
#define BQ_AI 0x14 /* mA, signed; negative discharging */
#define BQ_CYCT 0x2a
#define BQ_SOC 0x2c /* % */
#define BQ_DCAP 0x3c /* mAh */

struct hdq_battery {
	struct device *dev;
	void __iomem *base;
	struct mutex lock;
	spinlock_t sample_lock;
	struct power_supply *psy;
	unsigned long stamp; /* jiffies of the last refresh */
	int soc, volt, ai, temp, rm, fcc, flags, cyct, dcap;
	bool valid, suspended;
	u64 generation, sample_begin_ns, sample_end_ns;
};

static void hdq_drain(struct hdq_battery *b)
{
	int i;

	for (i = 0; i < 512 && (readl(b->base + UTRSTAT) & UTRSTAT_RXRDY); i++)
		readl(b->base + URXH);
}

/* one 8-bit register; -EIO if the gauge did not answer */
static int hdq_read8(struct hdq_battery *b, u8 reg)
{
	u8 rx[40];
	u32 ucon;
	int ret;
	int i, v = 0;
	unsigned int n = 0;

	if (reg > 0x7f)
		return -EINVAL;
	hdq_drain(b);
	writel(readl(b->base + UCON) | UCON_SBREAK, b->base + UCON);
	/* iBEC/iBoot drive UART5's break explicitly: 200 us low, 40 us idle. */
	udelay(200);
	writel(readl(b->base + UCON) & ~UCON_SBREAK, b->base + UCON);
	ret = readl_poll_timeout(b->base + UCON, ucon,
				!(ucon & UCON_SBREAK), 10, 5000);
	if (ret)
		return ret;
	udelay(40);			/* break recovery */
	hdq_drain(b);

	for (i = 0; i < 8; i++)		/* LSB first; bit 7 clear: read */
		writel(((reg >> i) & 1) ? 0xFE : 0xE0, b->base + UTXH);

	/* 8 echoed bits (1.5 ms), a turnaround, the gauge's 8 (1.7 ms) */
	usleep_range(4500, 5000);
	while (n < ARRAY_SIZE(rx) && (readl(b->base + UTRSTAT) & UTRSTAT_RXRDY))
		rx[n++] = readl(b->base + URXH) & 0xff;
	if (n < 16)
		return -EIO;
	/* Reject a damaged command echo before accepting the gauge's reply. */
	for (i = 0; i < 8; i++)
		if (rx[i] != (((reg >> i) & 1) ? 0xFE : 0xE0))
			return -EIO;
	for (i = 0; i < 8; i++)		/* the last 8 are the gauge's */
		v |= ((rx[n - 8 + i] & 0x04) ? 1 : 0) << i;
	return v;
}

static int hdq_read16(struct hdq_battery *b, u8 reg)
{
	int before, after, low, tries;

	/* No HDQ write command can be emitted, including reg+1. */
	if (reg > 0x7e)
		return -EINVAL;
	for (tries = 0; tries < 3; tries++) {
		before = hdq_read8(b, reg + 1);
		if (before < 0)
			return before;
		low = hdq_read8(b, reg);
		if (low < 0)
			return low;
		after = hdq_read8(b, reg + 1);
		if (after < 0)
			return after;
		if (before == after)
			return low | after << 8;
	}
	return -EIO;
}

/* Caller holds the transport mutex. Readers validate without sleeping. */
static void hdq_invalidate_locked(struct hdq_battery *b)
{
	unsigned long flags;
	spin_lock_irqsave(&b->sample_lock, flags);
	b->valid = false;
	b->generation++;
	spin_unlock_irqrestore(&b->sample_lock, flags);
}

/* Atomic publication of a sequential burst; display cache is at most5s.
 * Coherent high-low-high words use at least three byte transactions each. */
static int hdq_refresh_locked(struct hdq_battery *b, bool force)
{
	struct {
		u8 reg;
		int *dst;
	} r[] = {
		{ BQ_SOC, &b->soc },	 { BQ_VOLT, &b->volt },
		{ BQ_AI, &b->ai },	 { BQ_TEMP, &b->temp },
		{ BQ_RM, &b->rm },	 { BQ_FCC, &b->fcc },
		{ BQ_FLAGS, &b->flags }, { BQ_CYCT, &b->cyct },
		{ BQ_DCAP, &b->dcap },
	};
	int v, sample[ARRAY_SIZE(r)];
	unsigned int i;
	u64 begin;
	unsigned long lock_flags;

	if (b->suspended)
		return -EHOSTDOWN;
	if (!force && b->valid && time_before(jiffies, b->stamp + 5 * HZ))
		return 0;
	begin = ktime_get_ns();
	for (i = 0; i < ARRAY_SIZE(r); i++) {
		v = hdq_read16(b, r[i].reg);
		if (v < 0) {
			dev_warn_ratelimited(b->dev, "no answer for %#x\n",
					     r[i].reg);
			hdq_invalidate_locked(b);
			return v;
		}
		/* Capacity's power-supply ABI is a percentage, never raw noise. */
		if (r[i].reg == BQ_SOC && v > 100) {
			hdq_invalidate_locked(b);
			return -ERANGE;
		}
		sample[i] = v;
	}
	/* Do not publish a partial sample if a later HDQ transaction fails. */
	spin_lock_irqsave(&b->sample_lock, lock_flags);
	for (i = 0; i < ARRAY_SIZE(r); i++)
		*r[i].dst = sample[i];
	b->ai = (s16)b->ai;
	b->sample_begin_ns = begin;
	b->sample_end_ns = ktime_get_ns();
	b->generation++;
	b->stamp = jiffies;
	b->valid = true;
	spin_unlock_irqrestore(&b->sample_lock, lock_flags);
	return 0;
}

static int hdq_refresh(struct hdq_battery *b)
{
	return hdq_refresh_locked(b, false);
}

static int hdq_get_property(struct power_supply *psy,
			    enum power_supply_property psp,
			    union power_supply_propval *val)
{
	struct hdq_battery *b = power_supply_get_drvdata(psy);
	int ret;

	if (psp == POWER_SUPPLY_PROP_TECHNOLOGY) {
		val->intval = POWER_SUPPLY_TECHNOLOGY_LIPO;
		return 0;
	}
	mutex_lock(&b->lock);
	ret = hdq_refresh(b);
	if (ret)
		goto out;
	if (psp == POWER_SUPPLY_PROP_PRESENT) {
		val->intval = 1;
		goto out;
	}

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		if (b->flags & BQ_FLAG_FC)
			val->intval = POWER_SUPPLY_STATUS_FULL;
		else if (b->ai > 0)
			val->intval = POWER_SUPPLY_STATUS_CHARGING;
		else if (b->ai < 0 || (b->flags & BQ_FLAG_DSG))
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
		else
			val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = b->soc;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = b->volt * 1000;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		val->intval = b->ai * 1000;
		break;
	case POWER_SUPPLY_PROP_TEMP:
		val->intval = b->temp - 2731;
		break;
	case POWER_SUPPLY_PROP_CHARGE_NOW:
		val->intval = b->rm * 1000;
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL:
		val->intval = b->fcc * 1000;
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		val->intval = b->dcap * 1000;
		break;
	case POWER_SUPPLY_PROP_CYCLE_COUNT:
		val->intval = b->cyct;
		break;
	default:
		ret = -EINVAL;
	}
out:
	mutex_unlock(&b->lock);
	return ret;
}

static const enum power_supply_property hdq_props[] = {
	POWER_SUPPLY_PROP_STATUS,      POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_TECHNOLOGY,  POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_VOLTAGE_NOW, POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_TEMP,	       POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_CHARGE_FULL, POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CYCLE_COUNT,
};

static const struct power_supply_desc hdq_desc = {
	.name = "battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = hdq_props,
	.num_properties = ARRAY_SIZE(hdq_props),
	.get_property = hdq_get_property,
};

static int hdq_get_snapshot(struct hdq_battery *b, u64 max_age_ns,
			    struct apple_hdq_sample *out)
{
	u64 now;
	int ret = 0;
	if (!b || !out || !max_age_ns || max_age_ns > NSEC_PER_SEC)
		return -EINVAL;
	mutex_lock(&b->lock);
	now = ktime_get_ns();
	if (!b->valid || b->sample_begin_ns > now ||
	    now - b->sample_begin_ns >= max_age_ns)
		ret = hdq_refresh_locked(b, true);
	if (ret)
		goto out;
	now = ktime_get_ns();
	if (b->suspended || now < b->sample_begin_ns ||
	    now - b->sample_begin_ns >= max_age_ns) {
		ret = -ESTALE;
		goto out;
	}
	*out = (struct apple_hdq_sample){
		.generation = b->generation,
		.begin_ns = b->sample_begin_ns,
		.end_ns = b->sample_end_ns,
		.valid_until_ns = b->sample_begin_ns + max_age_ns,
		.voltage_uv = b->volt * 1000,
		.current_ua = b->ai * 1000,
		.temp_decic = b->temp - 2731,
		.flags = b->flags,
		.gauge_present = 1,
		.capacity_percent = b->soc,
		.charge_now_uah = b->rm * 1000,
		.charge_full_uah = b->fcc * 1000,
	};
out:
	mutex_unlock(&b->lock);
	return ret;
}

/* Validation performs no UART reads. Generation invalidation covers refresh
 * failure and suspend as well as replacement by a newer complete sample. */
static bool hdq_snapshot_valid(struct hdq_battery *b,
			       const struct apple_hdq_sample *sample)
{
	u64 now;
	unsigned long flags;
	bool valid;
	if (!b || !sample)
		return false;
	if (!spin_trylock_irqsave(&b->sample_lock, flags))
		return false;
	now = ktime_get_ns();
	valid = b->valid && !b->suspended &&
		b->generation == sample->generation &&
		b->sample_begin_ns == sample->begin_ns &&
		b->sample_end_ns == sample->end_ns &&
		sample->begin_ns <= sample->end_ns && sample->end_ns <= now &&
		sample->valid_until_ns > now &&
		sample->valid_until_ns >= sample->begin_ns &&
		sample->valid_until_ns - sample->begin_ns <= NSEC_PER_SEC;
	spin_unlock_irqrestore(&b->sample_lock, flags);
	return valid;
}

/* This driver object's address is used for matching only after a managed
 * link has protected the actual supplier platform-device lifetime. */
static struct platform_driver hdq_driver;
struct apple_hdq_reader {
	struct device *consumer, *supplier;
	struct device_link *link;
	struct power_supply *psy;
	struct hdq_battery *context;
};

static bool hdq_reader_usable(const struct apple_hdq_reader *reader)
{
	enum dl_dev_state cs;
	enum device_link_state ls;
	if (!reader)
		return false;
	cs = READ_ONCE(reader->consumer->links.status);
	if (cs != DL_DEV_PROBING && cs != DL_DEV_DRIVER_BOUND)
		return false;
	ls = READ_ONCE(reader->link->status);
	return (ls == DL_STATE_CONSUMER_PROBE || ls == DL_STATE_ACTIVE) &&
	       READ_ONCE(reader->supplier->links.status) ==
		       DL_DEV_DRIVER_BOUND &&
	       READ_ONCE(reader->supplier->driver) == &hdq_driver.driver;
}

struct apple_hdq_reader *apple_hdq_reader_open(struct device *consumer,
					       struct power_supply *psy)
{
	struct apple_hdq_reader *reader;
	struct device *supplier;
	struct device_link *link;
	struct hdq_battery *b;
	struct power_supply *held;
	enum device_link_state status;

	/* Caller holds its driver-core probe device lock and a live psy reference.
  * A class-device ref alone does NOT protect its producer's devm context. */
	if (!consumer || !dev_is_platform(consumer) || !psy ||
	    READ_ONCE(consumer->links.status) != DL_DEV_PROBING)
		return ERR_PTR(-EINVAL);
	supplier = psy->dev.parent;
	if (!supplier || !dev_is_platform(supplier) || consumer == supplier)
		return ERR_PTR(-EINVAL);
	link = device_link_add(consumer, supplier, DL_FLAG_AUTOREMOVE_CONSUMER);
	if (!link)
		return ERR_PTR(-EPROBE_DEFER);
	status = READ_ONCE(link->status);
	if ((status != DL_STATE_CONSUMER_PROBE && status != DL_STATE_ACTIVE) ||
	    READ_ONCE(supplier->links.status) != DL_DEV_DRIVER_BOUND)
		return ERR_PTR(-EPROBE_DEFER);
	if (!try_module_get(THIS_MODULE))
		return ERR_PTR(-ENODEV);
	/* Only here is the matched platform producer allowed to be dereferenced. */
	if (READ_ONCE(supplier->driver) != &hdq_driver.driver ||
	    psy->desc != &hdq_desc) {
		module_put(THIS_MODULE);
		return ERR_PTR(-ENODEV);
	}
	b = dev_get_drvdata(supplier);
	if (!b || b != power_supply_get_drvdata(psy) || b->dev != supplier ||
	    b->psy != psy) {
		module_put(THIS_MODULE);
		return ERR_PTR(-ENODEV);
	}
	held = power_supply_get_by_name(psy->desc->name);
	if (held != psy) {
		if (held)
			power_supply_put(held);
		module_put(THIS_MODULE);
		return ERR_PTR(-ENODEV);
	}
	reader = kzalloc(sizeof(*reader), GFP_KERNEL);
	if (!reader) {
		power_supply_put(held);
		module_put(THIS_MODULE);
		return ERR_PTR(-ENOMEM);
	}
	reader->consumer = get_device(consumer);
	reader->supplier = get_device(supplier);
	reader->link = link;
	reader->psy = held;
	reader->context = b;
	return reader;
}
EXPORT_SYMBOL_GPL(apple_hdq_reader_open);

/* Consumer remove/error-unwind must first stop/cancel all callbacks/work.
 * The driver core owns and removes the managed link after consumer unbind;
 * this function must not device_link_del() a managed link. */
void apple_hdq_reader_close(struct apple_hdq_reader *reader)
{
	if (!reader || IS_ERR(reader))
		return;
	power_supply_put(reader->psy);
	put_device(reader->supplier);
	put_device(reader->consumer);
	kfree(reader);
	module_put(THIS_MODULE);
}
EXPORT_SYMBOL_GPL(apple_hdq_reader_close);

static void hdq_reader_devm_close(void *context)
{
	apple_hdq_reader_close(context);
}

struct apple_hdq_reader *devm_apple_hdq_reader_open(struct device *consumer,
						    struct power_supply *psy)
{
	struct apple_hdq_reader *reader;
	int ret;
	reader = apple_hdq_reader_open(consumer, psy);
	if (IS_ERR(reader))
		return reader;
	ret = devm_add_action_or_reset(consumer, hdq_reader_devm_close, reader);
	if (ret)
		return ERR_PTR(ret);
	return reader;
}
EXPORT_SYMBOL_GPL(devm_apple_hdq_reader_open);

int apple_hdq_reader_snapshot(struct apple_hdq_reader *reader, u64 max_age_ns,
			      struct apple_hdq_sample *out)
{
	if (!hdq_reader_usable(reader))
		return -ENODEV;
	return hdq_get_snapshot(reader->context, max_age_ns, out);
}
EXPORT_SYMBOL_GPL(apple_hdq_reader_snapshot);

bool apple_hdq_reader_valid(struct apple_hdq_reader *reader,
			    const struct apple_hdq_sample *sample)
{
	/* Nonblocking state check precedes context access. An in-flight read may
  * finish while consumer removal drains it; supplier removal must wait. */
	return hdq_reader_usable(reader) &&
	       hdq_snapshot_valid(reader->context, sample);
}
EXPORT_SYMBOL_GPL(apple_hdq_reader_valid);

static int hdq_suspend(struct device *dev)
{
	struct hdq_battery *b = dev_get_drvdata(dev);
	unsigned long flags;
	mutex_lock(&b->lock);
	spin_lock_irqsave(&b->sample_lock, flags);
	b->suspended = true;
	b->valid = false;
	b->generation++;
	spin_unlock_irqrestore(&b->sample_lock, flags);
	mutex_unlock(&b->lock);
	return 0;
}
static int hdq_resume(struct device *dev)
{
	struct hdq_battery *b = dev_get_drvdata(dev);
	unsigned long flags;
	mutex_lock(&b->lock);
	spin_lock_irqsave(&b->sample_lock, flags);
	b->suspended = false;
	b->valid = false;
	b->generation++;
	spin_unlock_irqrestore(&b->sample_lock, flags);
	mutex_unlock(&b->lock);
	return 0;
}
static DEFINE_SIMPLE_DEV_PM_OPS(hdq_pm, hdq_suspend, hdq_resume);

static int hdq_probe(struct platform_device *pdev)
{
	struct power_supply_config cfg = {};
	struct hdq_battery *b;
	int soc;

	b = devm_kzalloc(&pdev->dev, sizeof(*b), GFP_KERNEL);
	if (!b)
		return -ENOMEM;
	b->dev = &pdev->dev;
	mutex_init(&b->lock);
	spin_lock_init(&b->sample_lock);
	b->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(b->base))
		return PTR_ERR(b->base);

	writel(HDQ_ULCON, b->base + ULCON);
	writel(HDQ_UFCON, b->base + UFCON);
	writel(HDQ_UBRDIV, b->base + UBRDIV);
	writel(0, b->base + UFRACVAL);
	writel(HDQ_UCON, b->base + UCON);

	mutex_lock(&b->lock);
	soc = hdq_refresh(b) ? -1 : b->soc;
	mutex_unlock(&b->lock);
	if (soc < 0)
		dev_warn(&pdev->dev, "the gauge does not answer yet\n");
	else
		dev_info(&pdev->dev,
			 "bq27540: %d%%, %d mV, %d mA, %d/%d mAh, %d cycles\n",
			 b->soc, b->volt, b->ai, b->rm, b->fcc, b->cyct);

	platform_set_drvdata(pdev, b);
	cfg.drv_data = b;
	cfg.of_node = pdev->dev.of_node;
	b->psy = devm_power_supply_register(&pdev->dev, &hdq_desc, &cfg);
	return PTR_ERR_OR_ZERO(b->psy);
}

static const struct of_device_id hdq_of_match[] = {
	{ .compatible = "apple,s5l8940x-hdq-battery" },
	{}
};
MODULE_DEVICE_TABLE(of, hdq_of_match);

static struct platform_driver hdq_driver = {
	.probe = hdq_probe,
	.driver = {
		.name = "apple-hdq-battery",
		.of_match_table = hdq_of_match,
 .pm = pm_sleep_ptr(&hdq_pm),
	},
};
module_platform_driver(hdq_driver);

MODULE_DESCRIPTION("bq27540 gas gauge on HDQ through UART5 (iPad mini 1)");
MODULE_LICENSE("GPL");
