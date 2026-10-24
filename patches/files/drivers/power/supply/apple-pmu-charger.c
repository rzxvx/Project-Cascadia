// SPDX-License-Identifier: GPL-2.0
/*
 * The iPad mini 1's USB input current limit: D1946 PMU register 0x3d.
 *
 * iBEC hands over with 0x3d at 0x02, about 100 mA, and nothing in Linux used
 * to raise it, so the iPad drained on any cable -- measured -386 mA on a
 * host port and -388 mA on a wall charger that charges iOS at +1082 mA.
 * The charger itself is already on: 0x3f and the temperature bands
 * 0x4b..0x53 are left as iBoot set them, and so is the input-suspend pair
 * 0x1b bit 2 / 0x1c bit 7.  This driver only moves the input limit, the
 * way iBEC's own apply step does it (0x9ff0c540 in the 12H321 iBEC):
 *
 *   code = (8 * mA - 600) / 100, then iBEC's correction: below 0x49 as is,
 *   0x49..0x4a -> 0x48, 0x4b..0x6e -> -2, 0x6f..0x72 -> 0x6e, 0x73 and up
 *   -> -4; under 100 mA it is 0x02, and nothing goes above 2100 mA.
 *
 *   Upwards it ramps, 8 codes every 10 ms.  If 0x3d then reads back 0x02
 *   the PMU has dropped the limit itself because VBUS sagged; iBEC and
 *   iOS (AppleD1946PMUPowerSource::handleInterrupt) both treat that as
 *   "this source cannot give more" and back off 8 codes.  So does this.
 *
 * Online is 0x0a bit 6: 0x6b plugged, 0x03 or 0x23 not, the same for a host
 * port and a charger -- telling those apart takes the D+/D- measurement on
 * the PMU's ADC that iBEC makes, which is not here.  Until it is, the
 * target is 500 mA, what any USB 2.0 port gives once configured and what
 * iOS itself takes from a Linux host, and a charger has to be asked for by
 * hand: raise max_input_ma, then write input_current_limit.
 *
 * The rules this file keeps:
 *  - 0x3d is the only register it writes.  pmu_chg_write() refuses any
 *    other.  It never reads 0x01..0x09: those clear on read.
 *  - It writes nothing until armed (module parameter, default off); until
 *    then it logs what it would do.  Disarming puts back the value it found
 *    at probe.
 *  - Above 45 C on the battery gauge it drops to 100 mA, and stays there
 *    until below 40 C.  iBEC's charge table ends at 45 C.
 */
#include <linux/delay.h>
#include <linux/devm-helpers.h>
#include <linux/jiffies.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>
#include <linux/apple-pmu-i2c.h>

#define PMU_STATUS0		0x0a	/* bit 6: VBUS there */
#define PMU_STATUS0_VBUS	BIT(6)
#define PMU_ILIM		0x3d	/* the only register written */

#define ILIM_MIN_CODE		0x02	/* < 100 mA; also the PMU's own sag value */
#define ILIM_STEP		8	/* codes per ramp step, 100 mA */
#define ILIM_STEP_MS		10
#define ILIM_HARD_MAX_MA	2100
#define ILIM_DEFAULT_MA		500

#define POLL_MS			1000
#define SAG_HOLD_MS		60000
#define HOT_DECI_C		450
#define COOL_DECI_C		400

static bool armed;
module_param(armed, bool, 0644);
MODULE_PARM_DESC(armed, "write 0x3d (default off: log only)");

static int max_input_ma = ILIM_DEFAULT_MA;
module_param(max_input_ma, int, 0644);
MODULE_PARM_DESC(max_input_ma, "ceiling for input_current_limit, 100..2100 mA (default 500)");

struct pmu_chg {
	struct device *dev;
	struct regmap *map;
	struct power_supply *psy;
	struct delayed_work work;
	struct mutex lock;		/* everything below */
	unsigned int req_ma;		/* asked for: default or input_current_limit */
	u8 probe_code;			/* 0x3d as iBEC left it */
	u8 last_written;		/* 0 = nothing written while online */
	u8 ceiling;			/* after a sag; 0xff = none */
	unsigned long ceiling_until;
	bool online;
	bool was_armed;
	bool hot;
	u8 logged_code;			/* last "would set" logged, to say it once */
};

/* iBEC's encoding, 0x9ff0c618..0x9ff0c662 */
static u8 pmu_chg_ma_to_code(unsigned int ma)
{
	unsigned int code;

	ma = min_t(unsigned int, ma, ILIM_HARD_MAX_MA);
	if (ma < 100)
		return ILIM_MIN_CODE;
	code = (8 * ma - 600) / 100;
	if (code >= 0x73)
		return code - 4;
	if (code > 0x6e)
		return 0x6e;
	if (code >= 0x4b)
		return code - 2;
	if (code > 0x48)
		return 0x48;
	return code;
}

/* The nominal current of a code, for the log only. */
static unsigned int pmu_chg_code_to_ma(u8 code)
{
	return code <= ILIM_MIN_CODE ? 100 : (code * 100 + 600) / 8;
}

static int pmu_chg_write(struct pmu_chg *c, unsigned int reg, u8 val)
{
	int ret;

	if (reg != PMU_ILIM) {
		dev_err(c->dev, "refusing PMU write 0x%02x = 0x%02x: only 0x%02x is ours\n",
			reg, val, PMU_ILIM);
		return -EPERM;
	}
	ret = regmap_write(c->map, reg, val);
	if (!ret)
		c->last_written = val;
	return ret;
}

static int pmu_chg_read(struct pmu_chg *c, unsigned int reg, u8 *val)
{
	unsigned int v;
	int ret = regmap_read(c->map, reg, &v);

	if (!ret)
		*val = v;
	return ret;
}

static void pmu_chg_sagged(struct pmu_chg *c, u8 at)
{
	c->ceiling = max_t(int, ILIM_MIN_CODE, at - ILIM_STEP);
	c->ceiling_until = jiffies + msecs_to_jiffies(SAG_HOLD_MS);
	dev_warn(c->dev, "VBUS sagged at 0x%02x: limiting USB input current to %u mA\n",
		 at, pmu_chg_code_to_ma(c->ceiling));
}

/* Ramp 0x3d from cur up to target, 8 codes every 10 ms, watching for a sag. */
static void pmu_chg_ramp(struct pmu_chg *c, u8 cur, u8 target)
{
	while (cur < target) {
		u8 next = min_t(int, cur + ILIM_STEP, target), rb;

		if (pmu_chg_write(c, PMU_ILIM, next))
			return;
		msleep(ILIM_STEP_MS);
		if (pmu_chg_read(c, PMU_ILIM, &rb))
			return;
		if (rb == ILIM_MIN_CODE && next > ILIM_MIN_CODE) {
			pmu_chg_sagged(c, next);
			if (c->ceiling < next)
				pmu_chg_write(c, PMU_ILIM, c->ceiling);
			return;
		}
		if (rb != next) {
			dev_warn(c->dev, "0x3d reads 0x%02x after writing 0x%02x; stopping the ramp\n",
				 rb, next);
			return;
		}
		cur = rb;
	}
}

static bool pmu_chg_battery_hot(struct pmu_chg *c)
{
	union power_supply_propval t;
	struct power_supply *bat = power_supply_get_by_name("battery");

	if (!bat)
		return c->hot;
	if (!power_supply_get_property(bat, POWER_SUPPLY_PROP_TEMP, &t)) {
		if (!c->hot && t.intval > HOT_DECI_C) {
			c->hot = true;
			dev_warn(c->dev, "battery at %d.%d C: input limited to 100 mA\n",
				 t.intval / 10, t.intval % 10);
		} else if (c->hot && t.intval < COOL_DECI_C) {
			c->hot = false;
			dev_info(c->dev, "battery back to %d.%d C\n", t.intval / 10, t.intval % 10);
		}
	}
	power_supply_put(bat);
	return c->hot;
}

static u8 pmu_chg_target(struct pmu_chg *c)
{
	unsigned int ma = min_t(unsigned int, c->req_ma,
				clamp(READ_ONCE(max_input_ma), 100, ILIM_HARD_MAX_MA));
	u8 code = pmu_chg_ma_to_code(ma);

	if (c->ceiling != 0xff && time_before(jiffies, c->ceiling_until))
		code = min(code, c->ceiling);
	else
		c->ceiling = 0xff;
	if (pmu_chg_battery_hot(c))
		code = ILIM_MIN_CODE;
	return code;
}

static void pmu_chg_work(struct work_struct *w)
{
	struct pmu_chg *c = container_of(to_delayed_work(w), struct pmu_chg, work);
	bool is_armed = READ_ONCE(armed), online, changed = false;
	u8 st, cur, target;

	mutex_lock(&c->lock);
	if (pmu_chg_read(c, PMU_STATUS0, &st) || pmu_chg_read(c, PMU_ILIM, &cur))
		goto out;
	online = st & PMU_STATUS0_VBUS;
	changed = online != c->online;
	c->online = online;
	if (changed && online)
		c->last_written = 0;	/* a new session: a low 0x3d now is not a sag */

	if (c->was_armed && !is_armed) {
		dev_info(c->dev, "disarmed: 0x3d back to 0x%02x\n", c->probe_code);
		pmu_chg_write(c, PMU_ILIM, c->probe_code);
		c->last_written = 0;
		c->logged_code = 0;
		c->was_armed = false;
		goto out;
	}
	if (!online)
		goto out;

	target = pmu_chg_target(c);
	if (!is_armed) {
		if (cur != target && target != c->logged_code) {
			dev_info(c->dev, "disarmed: would set 0x3d 0x%02x -> 0x%02x (%u mA)\n",
				 cur, target, pmu_chg_code_to_ma(target));
			c->logged_code = target;
		}
		goto out;
	}
	c->was_armed = true;

	/* steady state: the PMU dropped what we set, so the source sagged */
	if (cur == ILIM_MIN_CODE && c->last_written > ILIM_MIN_CODE) {
		pmu_chg_sagged(c, c->last_written);
		target = min(target, c->ceiling);
	}
	if (cur < target)
		pmu_chg_ramp(c, cur, target);
	else if (cur > target)
		pmu_chg_write(c, PMU_ILIM, target);
out:
	mutex_unlock(&c->lock);
	if (changed)
		power_supply_changed(c->psy);
	schedule_delayed_work(&c->work, msecs_to_jiffies(POLL_MS));
}

static int pmu_chg_get(struct power_supply *psy, enum power_supply_property psp,
		       union power_supply_propval *val)
{
	struct pmu_chg *c = power_supply_get_drvdata(psy);

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = c->online;
		return 0;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		val->intval = c->req_ma * 1000;
		return 0;
	default:
		return -EINVAL;
	}
}

static int pmu_chg_set(struct power_supply *psy, enum power_supply_property psp,
		       const union power_supply_propval *val)
{
	struct pmu_chg *c = power_supply_get_drvdata(psy);
	int ma = val->intval / 1000;

	if (psp != POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT)
		return -EINVAL;
	if (ma < 100 || ma > clamp(READ_ONCE(max_input_ma), 100, ILIM_HARD_MAX_MA))
		return -ERANGE;
	mutex_lock(&c->lock);
	c->req_ma = ma;
	c->logged_code = 0;
	mutex_unlock(&c->lock);
	mod_delayed_work(system_wq, &c->work, 0);
	return 0;
}

static int pmu_chg_writeable(struct power_supply *psy, enum power_supply_property psp)
{
	return psp == POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT;
}

static enum power_supply_property pmu_chg_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
};

static char *pmu_chg_supplied_to[] = { "battery" };

static const struct power_supply_desc pmu_chg_desc = {
	.name = "apple-pmu-usb",
	.type = POWER_SUPPLY_TYPE_USB,
	.properties = pmu_chg_props,
	.num_properties = ARRAY_SIZE(pmu_chg_props),
	.get_property = pmu_chg_get,
	.set_property = pmu_chg_set,
	.property_is_writeable = pmu_chg_writeable,
};

static int pmu_chg_probe(struct platform_device *pdev)
{
	struct apple_pmu_i2c *pmu = dev_get_drvdata(pdev->dev.parent);
	struct power_supply_config cfg = {};
	struct pmu_chg *c;
	u8 st;
	int ret;

	c = devm_kzalloc(&pdev->dev, sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;
	c->dev = &pdev->dev;
	c->map = pmu->regmap;
	c->req_ma = ILIM_DEFAULT_MA;
	c->ceiling = 0xff;
	mutex_init(&c->lock);

	if (pmu_chg_read(c, PMU_ILIM, &c->probe_code) || pmu_chg_read(c, PMU_STATUS0, &st))
		return -EIO;
	c->online = st & PMU_STATUS0_VBUS;

	cfg.drv_data = c;
	cfg.of_node = pdev->dev.of_node;
	cfg.supplied_to = pmu_chg_supplied_to;
	cfg.num_supplicants = ARRAY_SIZE(pmu_chg_supplied_to);
	c->psy = devm_power_supply_register(&pdev->dev, &pmu_chg_desc, &cfg);
	if (IS_ERR(c->psy))
		return PTR_ERR(c->psy);
	platform_set_drvdata(pdev, c);

	ret = devm_delayed_work_autocancel(&pdev->dev, &c->work, pmu_chg_work);
	if (ret)
		return ret;
	schedule_delayed_work(&c->work, msecs_to_jiffies(POLL_MS));
	dev_info(&pdev->dev, "0x3d = 0x%02x (%u mA) at probe, VBUS %s, %s\n",
		 c->probe_code, pmu_chg_code_to_ma(c->probe_code),
		 c->online ? "on" : "off", armed ? "armed" : "disarmed (log only)");
	return 0;
}

static const struct of_device_id pmu_chg_of_match[] = {
	{ .compatible = "apple,pmu-d1946-charger" },
	{ }
};
MODULE_DEVICE_TABLE(of, pmu_chg_of_match);

static struct platform_driver pmu_chg_driver = {
	.probe = pmu_chg_probe,
	.driver = {
		.name = "apple-pmu-charger",
		.of_match_table = pmu_chg_of_match,
	},
};
module_platform_driver(pmu_chg_driver);

MODULE_DESCRIPTION("D1946 PMU USB input current limit (iPad mini 1)");
MODULE_LICENSE("GPL");
