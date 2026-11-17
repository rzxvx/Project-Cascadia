// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple T8020 touchscreen clock driver
 *
 * Adapted from HoolockLinux, validated against J171 21B91 ApplePMGR.
 * T8020 touch group: PMGR bank 0 + 0x78000; divider 1..1023.
 *
 * Copyright (C) Corellium LLC
 * Copyright (C) Paul Praschl
 */

#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/iopoll.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#define APPLE_TOUCH_CLK_DISABLE		BIT(31)
#define APPLE_TOUCH_CLK_ENABLE		BIT(19)
#define APPLE_TOUCH_CLK_BUSY		BIT(18)
#define APPLE_TOUCH_CLK_DIV		GENMASK(9, 0)
#define APPLE_TOUCH_CLK_DIV_MAX		1023

struct apple_t8020_touch_clk {
	struct clk_hw hw;
	struct device *dev;
	struct regmap *regmap;
	u32 offset;
	u32 target_rate;
};

#define to_apple_t8020_touch_clk(_hw) \
	container_of(_hw, struct apple_t8020_touch_clk, hw)

static int apple_t8020_touch_clk_prepare(struct clk_hw *hw)
{
	struct apple_t8020_touch_clk *clk = to_apple_t8020_touch_clk(hw);
	unsigned long parent_rate = clk_hw_get_rate(clk_hw_get_parent(hw));
	u32 value, divider, before;
	int ret;

	divider = DIV_ROUND_CLOSEST(parent_rate, clk->target_rate);
	if (!parent_rate || !divider || divider > APPLE_TOUCH_CLK_DIV_MAX)
		return -EINVAL;

	ret = regmap_read(clk->regmap, clk->offset, &value);
	if (ret)
		return ret;

	before = value;
	value &= ~APPLE_TOUCH_CLK_DISABLE;
	ret = regmap_write(clk->regmap, clk->offset, value);
	if (ret)
		return ret;
	udelay(8);

	value &= ~APPLE_TOUCH_CLK_DIV;
	value |= APPLE_TOUCH_CLK_ENABLE | (divider & APPLE_TOUCH_CLK_DIV);
	ret = regmap_write(clk->regmap, clk->offset, value);
	if (ret)
		goto disable;

	ret = regmap_read_poll_timeout(clk->regmap, clk->offset, value,
					      !(value & APPLE_TOUCH_CLK_BUSY),
					      10, 50000);
	if (!ret) {
		dev_info(clk->dev, "T8020 TCLK offset=%#x divider=%u before=%#x ready=%#x\n",
			 clk->offset, divider, before, value);
		return 0;
	}

	dev_err(clk->dev, "touch clock failed to become ready: %#x\n", value);
disable:
	value &= ~APPLE_TOUCH_CLK_ENABLE;
	value |= APPLE_TOUCH_CLK_DISABLE;
	regmap_write(clk->regmap, clk->offset, value);
	return ret;
}

static void apple_t8020_touch_clk_unprepare(struct clk_hw *hw)
{
	struct apple_t8020_touch_clk *clk = to_apple_t8020_touch_clk(hw);
	u32 value;
	int ret;

	ret = regmap_read(clk->regmap, clk->offset, &value);
	if (ret)
		return;

	value &= ~APPLE_TOUCH_CLK_ENABLE;
	ret = regmap_write(clk->regmap, clk->offset, value);
	if (ret)
		return;

	ret = regmap_read_poll_timeout(clk->regmap, clk->offset, value,
					      !(value & APPLE_TOUCH_CLK_BUSY),
					      10, 50000);
	if (ret)
		dev_err(clk->dev, "touch clock failed to stop: %#x\n", value);

	value |= APPLE_TOUCH_CLK_DISABLE;
	regmap_write(clk->regmap, clk->offset, value);
	udelay(100);
}

static int apple_t8020_touch_clk_is_prepared(struct clk_hw *hw)
{
	struct apple_t8020_touch_clk *clk = to_apple_t8020_touch_clk(hw);
	u32 value;

	if (regmap_read(clk->regmap, clk->offset, &value))
		return 0;

	return !!(value & APPLE_TOUCH_CLK_ENABLE);
}

static unsigned long
apple_t8020_touch_clk_recalc_rate(struct clk_hw *hw,
				  unsigned long parent_rate)
{
	struct apple_t8020_touch_clk *clk = to_apple_t8020_touch_clk(hw);
	u32 divider;

	if (regmap_read(clk->regmap, clk->offset, &divider))
		return 0;

	divider &= APPLE_TOUCH_CLK_DIV;
	if (!divider)
		return 0;

	return parent_rate / divider;
}

static const struct clk_ops apple_t8020_touch_clk_ops = {
	.prepare = apple_t8020_touch_clk_prepare,
	.unprepare = apple_t8020_touch_clk_unprepare,
	.is_prepared = apple_t8020_touch_clk_is_prepared,
	.recalc_rate = apple_t8020_touch_clk_recalc_rate,
};

static int apple_t8020_touch_clk_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct apple_t8020_touch_clk *clk;
	struct clk_init_data init = {};
	const char *parent_name;
	const char *name;
	int ret;

	clk = devm_kzalloc(dev, sizeof(*clk), GFP_KERNEL);
	if (!clk)
		return -ENOMEM;
	clk->dev = dev;

	clk->regmap = syscon_node_to_regmap(dev->of_node->parent);
	if (IS_ERR(clk->regmap))
		return dev_err_probe(dev, PTR_ERR(clk->regmap),
				     "failed to get parent PMGR regmap\n");

	ret = of_property_read_u32_index(dev->of_node, "reg", 0,
					 &clk->offset);
	if (ret)
		return dev_err_probe(dev, ret, "missing PMGR offset\n");
	if (clk->offset != 0x78000)
		return dev_err_probe(dev, -EINVAL, "unsupported T8020 touch offset\n");

	ret = of_property_read_u32(dev->of_node, "clock-frequency",
				   &clk->target_rate);
	if (ret || !clk->target_rate)
		return dev_err_probe(dev, ret ?: -EINVAL,
				     "invalid clock-frequency\n");

	parent_name = of_clk_get_parent_name(dev->of_node, 0);
	if (!parent_name)
		return dev_err_probe(dev, -EINVAL, "missing parent clock\n");

	name = dev->of_node->name;
	of_property_read_string(dev->of_node, "clock-output-names", &name);
	init.name = name;
	init.ops = &apple_t8020_touch_clk_ops;
	init.flags = CLK_GET_RATE_NOCACHE;
	init.parent_names = &parent_name;
	init.num_parents = 1;
	clk->hw.init = &init;

	ret = devm_clk_hw_register(dev, &clk->hw);
	if (ret)
		return ret;

	return devm_of_clk_add_hw_provider(dev, of_clk_hw_simple_get,
					   &clk->hw);
}

static const struct of_device_id apple_t8020_touch_clk_of_match[] = {
	{ .compatible = "apple,t8020-touch-clock" },
	{ }
};
MODULE_DEVICE_TABLE(of, apple_t8020_touch_clk_of_match);

static struct platform_driver apple_t8020_touch_clk_driver = {
	.probe = apple_t8020_touch_clk_probe,
	.driver = {
		.name = "apple-t8020-touch-clk",
		.of_match_table = apple_t8020_touch_clk_of_match,
	},
};
module_platform_driver(apple_t8020_touch_clk_driver);

MODULE_DESCRIPTION("Apple T8020 touchscreen clock driver");
MODULE_AUTHOR("Paul Praschl");
MODULE_LICENSE("Dual MIT/GPL");
