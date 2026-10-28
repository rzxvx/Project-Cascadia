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

struct apple_pmu_i2c {
    struct device *dev;
    struct regmap *regmap;
    struct regmap_config config;
    struct d1946_bank0_bus *bank0; /* Optional experimental parent transport. */
};

#if IS_ENABLED(CONFIG_APPLE_PMU_D1946_BANK0_EXPERIMENT)
/* Probe-time direct-child API. Managed supplier link; no I/O or safety grant. */
struct regmap *apple_pmu_d1946_bank0_get_for_child(struct device *child);
#endif

#endif
