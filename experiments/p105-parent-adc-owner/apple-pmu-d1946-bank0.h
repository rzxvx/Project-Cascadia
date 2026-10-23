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
	bool reservation_held;
	int release_error;
	bool adc_claimed;
	struct task_struct *adc_task;
	atomic64_t adc_epoch;
	atomic_t adc_quiescent;
	atomic64_t adc_witness_epoch;
	int first_error; /* Adapter lock protects diagnostic writes. */
};
int d1946_bank0_init(struct d1946_bank0_bus *b, struct i2c_client *client);
void d1946_bank0_config(struct d1946_bank0_bus *b, struct regmap_config *config);
const struct regmap_bus *d1946_bank0_regmap_bus(void);
int d1946_bank0_seal(struct d1946_bank0_bus *b, int prepare_error);
void d1946_bank0_stop(void *context);
int d1946_bank0_power_off_atomic(struct d1946_bank0_bus *b);
int d1946_bank0_adc_claim(struct d1946_bank0_bus *b);
void d1946_bank0_adc_invalidate(struct d1946_bank0_bus *b);
int d1946_bank0_adc_begin(struct d1946_bank0_bus *b, u64 *epoch);
int d1946_bank0_adc_valid(struct d1946_bank0_bus *b, u64 epoch);
void d1946_bank0_adc_end(struct d1946_bank0_bus *b);
int d1946_bank0_adc_write(void *context, const void *data, size_t count);
int d1946_bank0_adc_read(void *context, const void *reg, size_t reg_size,
		       void *value, size_t count);
#endif
