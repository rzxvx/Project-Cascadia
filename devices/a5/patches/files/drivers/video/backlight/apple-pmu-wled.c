// SPDX-License-Identifier: GPL-2.0
/*
 * The iPad mini 1's backlight: the D1946 PMU's white-LED driver.
 *
 * Brightness is an 11-bit current DAC split over two registers, the high
 * eight bits in 0xd0 and the low three in 0xd1 -- AppleD1946PMU reads it back
 * as d0 << 3 | (d1 & 7) and writes it the same way, 0xd0 first.  The scale is
 * exponential: the ADT's iDAC2MilliAmpsTable steps the current by 2.5% per
 * unit of the high byte, and 2047 is the calibrated maximum, 22.5 mA (400
 * nits; ADT backlight calibratedMaxCurrent, milliAmps2DACTablePart2's last
 * entry).  iBoot leaves 1626, about 8 mA, the 140-nit middle.  Measured at the
 * battery: 2047 draws 520 mA more than 200; 0 and "off" are indistinguishable.
 *
 * Register 0xd2 switches the strings: AppleD1946PMU's backlight enable writes
 * the ADT's wled-enables (0x3f) there to switch them on and 0 to switch them
 * off.  This driver does the same for brightness 0 and for bl_power.
 *
 * The ADT routes iOS's brightness updates through the DWI block instead
 * (function-backlight_update = arm-io/dwi, a serial link to the PMU, used to
 * ramp without I2C traffic).  That path needs PMU state iBoot does not set up;
 * plain I2C does the same job.
 *
 * A child of the PMU node, like apple-pmu-pwrsw, on the PMU's regmap.
 */
#include <linux/backlight.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/apple-pmu-i2c.h>

#define WLED_MAX	2047

struct apple_wled {
	struct regmap *map;
	u32 level;		/* 0xd0: level[10:3]; +1: level[2:0] */
	u32 enable;		/* 0xd2 */
	u32 enables;		/* what "on" writes there: ADT wled-enables */
};

static int apple_wled_update(struct backlight_device *bl)
{
	struct apple_wled *w = bl_get_data(bl);
	int level = backlight_get_brightness(bl);
	int ret;

	if (!level)
		return regmap_write(w->map, w->enable, 0);
	ret = regmap_write(w->map, w->level, level >> 3);
	if (!ret)
		ret = regmap_write(w->map, w->level + 1, level & 7);
	if (!ret)
		ret = regmap_write(w->map, w->enable, w->enables);
	return ret;
}

static const struct backlight_ops apple_wled_ops = {
	.options = BL_CORE_SUSPENDRESUME,
	.update_status = apple_wled_update,
};

static int apple_wled_probe(struct platform_device *pdev)
{
	struct apple_pmu_i2c *pmu = dev_get_drvdata(pdev->dev.parent);
	struct device_node *np = pdev->dev.of_node;
	struct backlight_properties props = {};
	struct backlight_device *bl;
	struct apple_wled *w;
	unsigned int hi, lo, on;
	int cur;

	w = devm_kzalloc(&pdev->dev, sizeof(*w), GFP_KERNEL);
	if (!w)
		return -ENOMEM;
	w->map = pmu->regmap;
	if (of_property_read_u32(np, "reg", &w->level) ||
	    of_property_read_u32(np, "apple,enable-reg", &w->enable) ||
	    of_property_read_u32(np, "apple,wled-enables", &w->enables))
		return -EINVAL;

	/* start where iBoot left it */
	if (regmap_read(w->map, w->level, &hi) || regmap_read(w->map, w->level + 1, &lo) ||
	    regmap_read(w->map, w->enable, &on))
		return -EIO;
	cur = hi << 3 | (lo & 7);

	props.type = BACKLIGHT_RAW;
	props.scale = BACKLIGHT_SCALE_NON_LINEAR;
	props.max_brightness = WLED_MAX;
	props.brightness = cur;
	props.power = on ? BACKLIGHT_POWER_ON : BACKLIGHT_POWER_OFF;
	bl = devm_backlight_device_register(&pdev->dev, "apple-pmu-wled", &pdev->dev, w,
					    &apple_wled_ops, &props);
	if (IS_ERR(bl))
		return PTR_ERR(bl);
	platform_set_drvdata(pdev, bl);
	dev_info(&pdev->dev, "backlight %d/%d, %s\n", cur, WLED_MAX, on ? "on" : "off");
	return 0;
}

static const struct of_device_id apple_wled_of_match[] = {
	{ .compatible = "apple,pmu-wled" },
	{ }
};
MODULE_DEVICE_TABLE(of, apple_wled_of_match);

static struct platform_driver apple_wled_driver = {
	.probe = apple_wled_probe,
	.driver = {
		.name = "apple-pmu-wled",
		.of_match_table = apple_wled_of_match,
	},
};
module_platform_driver(apple_wled_driver);

MODULE_DESCRIPTION("D1946 PMU white-LED backlight (iPad mini 1)");
MODULE_LICENSE("GPL");
