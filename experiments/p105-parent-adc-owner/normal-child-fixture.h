/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2020 Corellium LLC. */
/* Actual unchanged Corellium/Upstream callback bodies; full sources pinned. */
static int apple_pmu_gpio_cfg(struct gpio_chip *gc, unsigned int n, unsigned int *cfg)
{
    struct apple_pmu_i2c *pmu = gpiochip_get_data(gc);

    return regmap_read(pmu->regmap, D1946_GPIO_CFG + n, cfg);
}
static int apple_pmu_gpio_get_direction(struct gpio_chip *gc, unsigned int n)
{
    unsigned int cfg;
    int ret = apple_pmu_gpio_cfg(gc, n, &cfg);

    if (ret)
        return ret;
    return D1946_GPIO_IS_INPUT(cfg) ? GPIO_LINE_DIRECTION_IN : GPIO_LINE_DIRECTION_OUT;
}
static int apple_pmu_gpio_get(struct gpio_chip *gc, unsigned int n)
{
    unsigned int cfg;
    int ret = apple_pmu_gpio_cfg(gc, n, &cfg);

    return ret ? ret : !!(cfg & D1946_GPIO_LEVEL);
}
static void apple_pmu_gpio_set(struct gpio_chip *gc, unsigned int n, int val)
{
    struct apple_pmu_i2c *pmu = gpiochip_get_data(gc);

    regmap_update_bits(pmu->regmap, D1946_GPIO_CFG + n, D1946_GPIO_LEVEL,
                       val ? D1946_GPIO_LEVEL : 0);
}
static int apple_pmu_gpio_direction_output(struct gpio_chip *gc, unsigned int n, int val)
{
    struct apple_pmu_i2c *pmu = gpiochip_get_data(gc);
    unsigned int cfg;
    int ret = apple_pmu_gpio_cfg(gc, n, &cfg);

    if (ret)
        return ret;
    if (D1946_GPIO_IS_INPUT(cfg))
        return -EPERM;
    return regmap_update_bits(pmu->regmap, D1946_GPIO_CFG + n,
                              D1946_GPIO_LEVEL, val ? D1946_GPIO_LEVEL : 0);
}
static int apple_pmu_gpio_direction_input(struct gpio_chip *gc, unsigned int n)
{
    int direction = apple_pmu_gpio_get_direction(gc, n);

    if (direction < 0)
        return direction;
    return direction == GPIO_LINE_DIRECTION_IN ? 0 : -EPERM;
}
static int apple_pmu_d1946_prepare_checked(struct apple_pmu_i2c *pmu)
{
    struct regmap *map = pmu->regmap;
    unsigned int i, v;
    int ret;

    ret = regmap_write(map, D1946_WLED_ENABLE, 0);
    if (ret)
        return ret;
    for (i = 0; i < D1946_NEVENTS; i++) {
        ret = regmap_read(map, D1946_EVENTS + i, &v);
        if (ret)
            return ret;
    }
    for (i = 0; i < D1946_NEVENTS; i++) {
        ret = regmap_write(map, D1946_EVENT_MASKS + i, d1946_off_masks[i]);
        if (ret)
            return ret;
    }
    ret = regmap_write(map, D1946_ALARM_CTRL, 0x01);
    if (ret)
        return ret;
    ret = regmap_write(map, D1946_REG_A1, 0);
    if (ret)
        return ret;
    ret = regmap_update_bits(map, D1946_BOOT_FLAGS, 0xd0, 0);
    if (ret)
        return ret;
    for (i = 0; i < D1946_NGPIO; i++) {
        ret = regmap_read(map, D1946_GPIO_CFG + i, &v);
        if (ret)
            return ret;
        if (v >= 0x60 && (v & D1946_GPIO_WAKE)) {
            ret = regmap_write(map, D1946_GPIO_CFG + i, v & ~D1946_GPIO_WAKE);
            if (ret)
                return ret;
        }
    }
    return 0;
}
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
static int apple_pmu_i2c_pwrsw_set(struct regulator_dev *rdev, bool on)
{
    struct apple_pmu_i2c_pwrsw *pwrsw = rdev->reg_data;
    struct regmap *map = rdev_get_regmap(rdev);
    unsigned int want = on ? pwrsw->enable : (pwrsw->enable ^ pwrsw->mask);
    unsigned int v = 0;
    int i, ret;

    for (i = 1; i <= 5; i++) {
        ret = regmap_update_bits(map, pwrsw->base, pwrsw->mask, want);
        if (ret)
            return ret;
        ret = regmap_read(map, pwrsw->base, &v);
        if (ret)
            return ret;
        if ((v & pwrsw->mask) == want) {
            if (i > 1)
                dev_info(pwrsw->dev, "0x%02x %s: took %d writes\n",
                         pwrsw->base, on ? "on" : "off", i);
            return 0;
        }
        msleep(1);
    }
    dev_err(pwrsw->dev, "0x%02x %s: still 0x%02x after %d writes\n",
            pwrsw->base, on ? "on" : "off", v, i - 1);
    return -EIO;
}
