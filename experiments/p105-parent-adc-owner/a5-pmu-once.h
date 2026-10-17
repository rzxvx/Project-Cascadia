// SPDX-License-Identifier: GPL-2.0-only
#ifndef A5_PMU_ONCE_H
#define A5_PMU_ONCE_H
#include <linux/i2c.h>
int apple_s5l8940x_i2c_pmu_reserve(struct i2c_client *client);
int apple_s5l8940x_i2c_pmu_release(struct i2c_client *client);
int apple_s5l8940x_i2c_pmu_once(struct i2c_client *client,
			      struct i2c_msg *msgs, int num, bool atomic);
#endif
