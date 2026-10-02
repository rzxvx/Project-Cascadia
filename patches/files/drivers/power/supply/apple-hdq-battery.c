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
 * A transaction starts with a break (UCON bit 4: one frame low).
 *
 * Only reads (command bit 7 clear) are ever sent: register numbers are the
 * bq27541 family's standard commands, 16 bits as two 8-bit reads.
 * README ("Battery gauge") has the summary.
 */
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>

#define ULCON		0x00
#define UCON		0x04
#define  UCON_SBREAK	BIT(4)
#define UFCON		0x08
#define UTRSTAT		0x10
#define  UTRSTAT_RXRDY	BIT(0)	/* Apple's UFSTAT layout differs from Samsung's */
#define UTXH		0x20
#define URXH		0x24
#define UBRDIV		0x28
#define UFRACVAL	0x2c

/* iBoot's setup: 8N2, RX/TX polled, FIFO on, 24 MHz / 16 / 26 = 57692 baud */
#define HDQ_ULCON	0x07
#define HDQ_UCON	0x405
#define HDQ_UFCON	0x01
#define HDQ_UBRDIV	25

/* bq27541-family standard commands */
#define BQ_TEMP		0x06	/* 0.1 K */
#define BQ_VOLT		0x08	/* mV */
#define BQ_FLAGS	0x0a
#define  BQ_FLAG_DSG	BIT(0)
#define  BQ_FLAG_FC	BIT(9)
#define BQ_RM		0x10	/* mAh */
#define BQ_FCC		0x12	/* mAh */
#define BQ_AI		0x14	/* mA, signed; negative discharging */
#define BQ_CYCT		0x2a
#define BQ_SOC		0x2c	/* % */
#define BQ_DCAP		0x3c	/* mAh */

struct hdq_battery {
	struct device *dev;
	void __iomem *base;
	struct mutex lock;
	struct power_supply *psy;
	unsigned long stamp;		/* jiffies of the last refresh */
	int soc, volt, ai, temp, rm, fcc, flags, cyct, dcap;
	bool valid;
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
	unsigned long end;
	int i, n = 0, v = 0;

	hdq_drain(b);
	writel(readl(b->base + UCON) | UCON_SBREAK, b->base + UCON);
	end = jiffies + msecs_to_jiffies(5);
	while ((readl(b->base + UCON) & UCON_SBREAK) && time_before(jiffies, end))
		udelay(10);
	udelay(200);			/* break recovery */
	hdq_drain(b);

	for (i = 0; i < 8; i++)		/* LSB first; bit 7 clear: read */
		writel(((reg >> i) & 1) ? 0xFE : 0xE0, b->base + UTXH);

	/* 8 echoed bits (1.5 ms), a turnaround, the gauge's 8 (1.7 ms) */
	usleep_range(4500, 5000);
	while (n < ARRAY_SIZE(rx) && (readl(b->base + UTRSTAT) & UTRSTAT_RXRDY))
		rx[n++] = readl(b->base + URXH) & 0xff;
	if (n < 16)
		return -EIO;
	for (i = 0; i < 8; i++)		/* the last 8 are the gauge's */
		v |= ((rx[n - 8 + i] & 0x04) ? 1 : 0) << i;
	return v;
}

static int hdq_read16(struct hdq_battery *b, u8 reg)
{
	int lo, hi, tries;

	for (tries = 0; tries < 3; tries++) {
		lo = hdq_read8(b, reg);
		hi = lo < 0 ? lo : hdq_read8(b, reg + 1);
		if (lo >= 0 && hi >= 0)
			return lo | hi << 8;
	}
	return -EIO;
}

/* everything at once, at most every 5 s: a 16-bit read is ~10 ms of bus */
static int hdq_refresh(struct hdq_battery *b)
{
	struct { u8 reg; int *dst; } r[] = {
		{ BQ_SOC, &b->soc }, { BQ_VOLT, &b->volt }, { BQ_AI, &b->ai },
		{ BQ_TEMP, &b->temp }, { BQ_RM, &b->rm }, { BQ_FCC, &b->fcc },
		{ BQ_FLAGS, &b->flags }, { BQ_CYCT, &b->cyct }, { BQ_DCAP, &b->dcap },
	};
	int i, v;

	if (b->valid && time_before(jiffies, b->stamp + 5 * HZ))
		return 0;
	for (i = 0; i < ARRAY_SIZE(r); i++) {
		v = hdq_read16(b, r[i].reg);
		if (v < 0) {
			dev_warn_ratelimited(b->dev, "no answer for %#x\n", r[i].reg);
			return v;
		}
		*r[i].dst = v;
	}
	b->ai = (s16)b->ai;
	b->stamp = jiffies;
	b->valid = true;
	return 0;
}

static int hdq_get_property(struct power_supply *psy, enum power_supply_property psp,
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
	mutex_unlock(&b->lock);
	if (psp == POWER_SUPPLY_PROP_PRESENT) {
		val->intval = ret == 0;
		return 0;
	}
	if (ret)
		return ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		if (b->flags & BQ_FLAG_FC)
			val->intval = POWER_SUPPLY_STATUS_FULL;
		else if (b->ai > 0)
			val->intval = POWER_SUPPLY_STATUS_CHARGING;
		else if (b->flags & BQ_FLAG_DSG)
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
		return -EINVAL;
	}
	return 0;
}

static const enum power_supply_property hdq_props[] = {
	POWER_SUPPLY_PROP_STATUS, POWER_SUPPLY_PROP_PRESENT, POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CAPACITY, POWER_SUPPLY_PROP_VOLTAGE_NOW, POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_TEMP, POWER_SUPPLY_PROP_CHARGE_NOW, POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN, POWER_SUPPLY_PROP_CYCLE_COUNT,
};

static const struct power_supply_desc hdq_desc = {
	.name = "battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = hdq_props,
	.num_properties = ARRAY_SIZE(hdq_props),
	.get_property = hdq_get_property,
};

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
		dev_info(&pdev->dev, "bq27540: %d%%, %d mV, %d mA, %d/%d mAh, %d cycles\n",
			 b->soc, b->volt, b->ai, b->rm, b->fcc, b->cyct);

	cfg.drv_data = b;
	cfg.of_node = pdev->dev.of_node;
	b->psy = devm_power_supply_register(&pdev->dev, &hdq_desc, &cfg);
	return PTR_ERR_OR_ZERO(b->psy);
}

static const struct of_device_id hdq_of_match[] = {
	{ .compatible = "apple,s5l8940x-hdq-battery" },
	{ }
};
MODULE_DEVICE_TABLE(of, hdq_of_match);

static struct platform_driver hdq_driver = {
	.probe = hdq_probe,
	.driver = {
		.name = "apple-hdq-battery",
		.of_match_table = hdq_of_match,
	},
};
module_platform_driver(hdq_driver);

MODULE_DESCRIPTION("bq27540 gas gauge on HDQ through UART5 (iPad mini 1)");
MODULE_LICENSE("GPL");
