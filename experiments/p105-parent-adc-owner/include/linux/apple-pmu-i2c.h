/*
 * Driver for I2C-based PMUs found on mobile devices with S5L8940X (Apple A5) SoCs.
 *
 * Copyright (C) 2020 Corellium LLC
 */

#ifndef _LINUX_INCLUDE_HX_PMU_I2C_H_
#define _LINUX_INCLUDE_HX_PMU_I2C_H_

#include <linux/device.h>
#include <linux/regmap.h>

struct d1946_bank0_bus;
struct p105_owned_adc6;

struct apple_pmu_i2c {
    struct device *dev;
    struct regmap *regmap;
    struct regmap_config config;
    struct d1946_bank0_bus *bank0;
#if IS_ENABLED(CONFIG_APPLE_PMU_D1946_ADC6_OWNER_EXPERIMENT)
    struct p105_owned_adc6 *adc6;
#endif /* Optional experimental parent transport. */
};

#endif
