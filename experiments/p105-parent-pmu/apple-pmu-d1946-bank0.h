// SPDX-License-Identifier: GPL-2.0-only
/* Source-only opt-in parent transport. No charger/protection authorization. */
#ifndef APPLE_PMU_D1946_BANK0_H
#define APPLE_PMU_D1946_BANK0_H
#include <linux/atomic.h>
#include <linux/i2c.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
struct d1946_bank0_bus {
	struct i2c_client *client;
	struct mutex map_lock;
	atomic_t state;
	atomic_t fault;
	atomic_t off_ready;
	int first_error; /* Adapter lock protects diagnostic writes. */
};
int d1946_bank0_check_running(struct d1946_bank0_bus *b,
                            struct i2c_client *client);
int d1946_bank0_init(struct d1946_bank0_bus *b, struct i2c_client *client);
void d1946_bank0_config(struct d1946_bank0_bus *b, struct regmap_config *config);
const struct regmap_bus *d1946_bank0_regmap_bus(void);
int d1946_bank0_seal(struct d1946_bank0_bus *b, int prepare_error);
void d1946_bank0_stop(void *context);
int d1946_bank0_power_off_atomic(struct d1946_bank0_bus *b);
#endif
