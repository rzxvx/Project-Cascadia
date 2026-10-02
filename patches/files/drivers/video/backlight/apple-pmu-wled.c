// SPDX-License-Identifier: GPL-2.0
/*
 * The iPad mini 1's backlight: the D1946 PMU's white-LED driver.
 *
 * What iOS does (AppleD1946PMU, the backlight update at 0x80ed0768 in the
 * 12H321 kernelcache): read register 0x3f and clear bit 6; for a non-zero
 * brightness write the level -- value / 40, 0..63 -- into the string current
 * registers 0x4b, 0x4d, 0x4f, 0x51, 0x53, each clamped to that string's own
 * maximum; for zero set bit 6 instead; write 0x3f back.  This panel uses the
 * three strings at 0x4f, 0x51 and 0x53 (the other two read 0 under iOS and
 * iBoot alike); iBoot leaves them at 0x3f, iOS ran them at 0x38.
 *
 * A child of the PMU node, like apple-pmu-pwrsw, on the PMU's regmap.
 */
#include <linux/backlight.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/apple-pmu-i2c.h>

#define WLED_CTRL_OFF	BIT(6)
#define WLED_MAX	63
#define WLED_MAX_STRINGS 5

struct apple_wled {
	struct regmap *map;
	u32 ctrl;			/* 0x3f */
	u32 strings[WLED_MAX_STRINGS];	/* 0x4f 0x51 0x53 */
	int nstrings;
};

static int apple_wled_update(struct backlight_device *bl)
{
	struct apple_wled *w = bl_get_data(bl);
	int level = backlight_get_brightness(bl);
	int i, ret;

	if (level) {
		for (i = 0; i < w->nstrings; i++) {
			ret = regmap_write(w->map, w->strings[i], level);
			if (ret)
				return ret;
		}
		return regmap_update_bits(w->map, w->ctrl, WLED_CTRL_OFF, 0);
	}
	return regmap_update_bits(w->map, w->ctrl, WLED_CTRL_OFF, WLED_CTRL_OFF);
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
	unsigned int ctrl, cur = WLED_MAX;
	int n;

	w = devm_kzalloc(&pdev->dev, sizeof(*w), GFP_KERNEL);
	if (!w)
		return -ENOMEM;
	w->map = pmu->regmap;
	if (of_property_read_u32(np, "reg", &w->ctrl))
		return -EINVAL;
	n = of_property_count_u32_elems(np, "apple,strings");
	if (n <= 0 || n > WLED_MAX_STRINGS)
		return -EINVAL;
	w->nstrings = n;
	of_property_read_u32_array(np, "apple,strings", w->strings, n);

	/* start where iBoot left it */
	if (regmap_read(w->map, w->strings[0], &cur) || cur > WLED_MAX)
		cur = WLED_MAX;
	if (regmap_read(w->map, w->ctrl, &ctrl))
		ctrl = 0;

	props.type = BACKLIGHT_RAW;
	props.max_brightness = WLED_MAX;
	props.brightness = cur ? cur : WLED_MAX;
	props.power = (ctrl & WLED_CTRL_OFF) ? BACKLIGHT_POWER_OFF : BACKLIGHT_POWER_ON;
	bl = devm_backlight_device_register(&pdev->dev, "apple-pmu-wled", &pdev->dev, w,
					    &apple_wled_ops, &props);
	if (IS_ERR(bl))
		return PTR_ERR(bl);
	platform_set_drvdata(pdev, bl);
	dev_info(&pdev->dev, "backlight %u/%d on %d strings, %s\n", cur, WLED_MAX, n,
		 (ctrl & WLED_CTRL_OFF) ? "off" : "on");
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
