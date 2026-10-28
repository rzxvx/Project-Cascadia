// SPDX-License-Identifier: GPL-2.0-only
/* Actual D1946 descriptor01/03/57/1e: known parent page0 transport only. */
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/lockdep.h>
#include <linux/string.h>
#include "apple-pmu-d1946-bank0.h"
#define BANK0_RUNNING 0
#define BANK0_SEALED 1
#define BANK0_STOPPED 2
#define BANK0_ADDR 0x3c
#define BANK0_SELECTOR 0x1e

int d1946_bank0_init(struct d1946_bank0_bus *b, struct i2c_client *client)
{
	/* i2c-dev can change adapter retries without taking the segment lock.
	 * Refuse both builtin and modular access, including forced builds. */
#if defined(CONFIG_I2C_CHARDEV) || defined(CONFIG_I2C_CHARDEV_MODULE)
	return -EOPNOTSUPP;
#endif
	if (IS_ERR_OR_NULL(b) || IS_ERR_OR_NULL(client) ||
	    IS_ERR_OR_NULL(client->adapter) || client->addr != BANK0_ADDR ||
	    client->flags || IS_ERR_OR_NULL(client->adapter->algo) ||
	    client->adapter->retries ||
	    !i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EINVAL;
	if (memchr_inv(b, 0, sizeof(*b)))
		return -EALREADY;
	b->client = client;
	mutex_init(&b->map_lock);
	atomic_set(&b->state, BANK0_RUNNING);
	atomic_set(&b->fault, 0);
	atomic_set(&b->off_ready, 0);
	return 0;
}


/* No I/O and no lasting lease: caller has already established parent lifetime.
 * The regmap callbacks still enforce admission after later revoke/seal/stop. */
int d1946_bank0_check_running(struct d1946_bank0_bus *b,
                            struct i2c_client *client)
{
	if (IS_ERR_OR_NULL(b) || IS_ERR_OR_NULL(client))
		return -EINVAL;
	if (b->client != client)
		return -ENODEV;
	if (atomic_read(&b->state) != BANK0_RUNNING)
		return -ESHUTDOWN;
	if (atomic_read(&b->fault))
		return -EIO;
	return 0;
}

static int bank0_failed(struct d1946_bank0_bus *b, int ret)
{
	if (ret >= 0)
		ret = -EIO;
	if (!atomic_cmpxchg(&b->fault, 0, 1))
		b->first_error = ret;
	atomic_set(&b->off_ready, 0);
	return ret;
}

/* Caller holds adapter segment lock; __i2c_transfer must never relock it. */
static int bank0_xfer(struct d1946_bank0_bus *b, struct i2c_msg *msgs, int n)
{
	int ret;

	if (b->client->adapter->retries)
		return -EOPNOTSUPP; /* Do not mutate global retry policy. */
	ret = __i2c_transfer(b->client->adapter, msgs, n);
	return ret == n ? 0 : bank0_failed(b, ret);
}

static int bank0_select(struct d1946_bank0_bus *b)
{
	u8 selector[] = {BANK0_SELECTOR, 0};
	struct i2c_msg msg = {.addr = BANK0_ADDR, .len = 2, .buf = selector};

	return bank0_xfer(b, &msg, 1);
}

static bool bank0_range(u8 reg, size_t count)
{
	/* Selector is private transport state, never a child data register. */
	return count && count <= 256 - (unsigned int)reg &&
	       !(reg <= BANK0_SELECTOR && reg + count > BANK0_SELECTOR);
}

static void bank0_map_lock(void *context)
{
	struct d1946_bank0_bus *b = context;

	mutex_lock(&b->map_lock);
	i2c_lock_bus(b->client->adapter, I2C_LOCK_SEGMENT);
}

static void bank0_map_unlock(void *context)
{
	struct d1946_bank0_bus *b = context;

	i2c_unlock_bus(b->client->adapter, I2C_LOCK_SEGMENT);
	mutex_unlock(&b->map_lock);
}

static int bank0_admit(struct d1946_bank0_bus *b)
{
	lockdep_assert_held(&b->map_lock);
	if (atomic_read(&b->state) != BANK0_RUNNING)
		return -ESHUTDOWN;
	if (atomic_read(&b->fault))
		return -EIO;
	return 0;
}

static int bank0_write(void *context, const void *data, size_t count)
{
	struct d1946_bank0_bus *b = context;
	const u8 *wire = data;
	struct i2c_msg msg;
	int ret;

	if (IS_ERR_OR_NULL(data) || count < 2 || count > 257 ||
	    !bank0_range(wire[0], count - 1))
		return -EINVAL;
	ret = bank0_admit(b);
	if (ret)
		return ret;
	ret = bank0_select(b);
	if (ret)
		return ret;
	ret = bank0_admit(b);
	if (ret)
		return ret;
	msg = (struct i2c_msg){.addr = BANK0_ADDR, .len = count,
			      .buf = (u8 *)data};
	ret = bank0_xfer(b, &msg, 1);
	return ret ? ret : bank0_admit(b);
}

static int bank0_read(void *context, const void *reg, size_t reg_size,
		      void *value, size_t count)
{
	struct d1946_bank0_bus *b = context;
	struct i2c_msg msgs[2];
	int ret;

	if (IS_ERR_OR_NULL(value) || !count || count > 256)
		return -EINVAL;
	memset(value, 0, count);
	if (IS_ERR_OR_NULL(reg) || reg_size != 1 ||
	    !bank0_range(*(const u8 *)reg, count))
		return -EINVAL;
	ret = bank0_admit(b);
	if (ret)
		return ret;
	ret = bank0_select(b);
	if (ret)
		return ret;
	ret = bank0_admit(b);
	if (ret)
		return ret;
	msgs[0] = (struct i2c_msg){.addr = BANK0_ADDR, .len = 1, .buf = (u8 *)reg};
	msgs[1] = (struct i2c_msg){.addr = BANK0_ADDR, .flags = I2C_M_RD,
				.len = count, .buf = value};
	ret = bank0_xfer(b, msgs, 2);
	if (!ret)
		ret = bank0_admit(b);
	if (ret)
		memset(value, 0, count);
	return ret;
}

static const struct regmap_bus bank0_bus = {
	.write = bank0_write,
	.read = bank0_read,
	.max_raw_read = 256,
	.max_raw_write = 256,
};

const struct regmap_bus *d1946_bank0_regmap_bus(void)
{
	return &bank0_bus;
}

static bool bank0_precious(struct device *dev, unsigned int reg)
{
	(void)dev;
	return reg >= 1 && reg <= 9; /* Existing read-to-clear events, not debug dump. */
}

void d1946_bank0_config(struct d1946_bank0_bus *b, struct regmap_config *config)
{
	config->reg_bits = 8;
	config->val_bits = 8;
	config->max_register = 255;
	config->cache_type = REGCACHE_NONE;
	config->lock = bank0_map_lock;
	config->unlock = bank0_map_unlock;
	config->lock_arg = b;
	config->can_sleep = true;
	config->precious_reg = bank0_precious;
}

int d1946_bank0_seal(struct d1946_bank0_bus *b, int prepare_error)
{
	int ret = prepare_error > 0 ? -EPROTO : prepare_error;

	if (atomic_cmpxchg(&b->state, BANK0_RUNNING, BANK0_SEALED) != BANK0_RUNNING)
		return -EALREADY;
	/* No new normal transfer; drain any currently held adapter transaction. */
	i2c_lock_bus(b->client->adapter, I2C_LOCK_SEGMENT);
	if (!ret && atomic_read(&b->state) != BANK0_SEALED)
		ret = -ESHUTDOWN;
	if (!ret && atomic_read(&b->fault))
		ret = -EIO;
	if (!ret)
		atomic_set(&b->off_ready, 1);
	i2c_unlock_bus(b->client->adapter, I2C_LOCK_SEGMENT);
	return ret;
}

void d1946_bank0_stop(void *context)
{
	struct d1946_bank0_bus *b = context;

	atomic_set(&b->state, BANK0_STOPPED);
	atomic_set(&b->off_ready, 0);
	i2c_lock_bus(b->client->adapter, I2C_LOCK_SEGMENT);
	i2c_unlock_bus(b->client->adapter, I2C_LOCK_SEGMENT);
	/* No nested reverse lock order: finish any map unlock after adapter drain. */
	mutex_lock(&b->map_lock);
	mutex_unlock(&b->map_lock);
}

static int bank0_off_admit(struct d1946_bank0_bus *b)
{
	return atomic_read(&b->state) == BANK0_SEALED && !atomic_read(&b->fault) ?
		0 : -ESHUTDOWN;
}

static int bank0_off_xfer(struct d1946_bank0_bus *b, struct i2c_msg *msgs, int n)
{
	int ret = bank0_off_admit(b);

	if (!ret)
		ret = bank0_xfer(b, msgs, n);
	return ret ? ret : bank0_off_admit(b);
}

static int bank0_off_select(struct d1946_bank0_bus *b)
{
	int ret = bank0_off_admit(b);

	if (!ret)
		ret = bank0_select(b);
	return ret ? ret : bank0_off_admit(b);
}

int d1946_bank0_power_off_atomic(struct d1946_bank0_bus *b)
{
	struct i2c_adapter *adap = b->client->adapter;
	u8 reg = 0x1b, value = 0, wire[2];
	struct i2c_msg msgs[2];
	int ret = -EOPNOTSUPP;

	if (!irqs_disabled() || system_state <= SYSTEM_RUNNING ||
	    !adap->algo->master_xfer_atomic || adap->retries) {
		atomic_set(&b->off_ready, 0);
		return ret;
	} /* Core's atomic path must not fall back to sleeping xfer. */
	if (!i2c_trylock_bus(adap, I2C_LOCK_SEGMENT)) {
		atomic_set(&b->off_ready, 0);
		return -EBUSY;
	}
	ret = -ESHUTDOWN;
	if (atomic_read(&b->state) != BANK0_SEALED || atomic_read(&b->fault) ||
	    atomic_cmpxchg(&b->off_ready, 1, 0) != 1)
		goto out;
	ret = bank0_off_select(b);
	if (ret)
		goto out;
	msgs[0] = (struct i2c_msg){.addr = BANK0_ADDR, .len = 1, .buf = &reg};
	msgs[1] = (struct i2c_msg){.addr = BANK0_ADDR, .flags = I2C_M_RD,
				.len = 1, .buf = &value};
	ret = bank0_off_xfer(b, msgs, 2);
	if (ret)
		goto out;
	ret = bank0_off_select(b);
	if (ret)
		goto out;
	wire[0] = reg;
	wire[1] = value | 1;
	msgs[0] = (struct i2c_msg){.addr = BANK0_ADDR, .len = 2, .buf = wire};
	ret = bank0_off_xfer(b, msgs, 1);
out:
	i2c_unlock_bus(adap, I2C_LOCK_SEGMENT);
	return ret;
}
