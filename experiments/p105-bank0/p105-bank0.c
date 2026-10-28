// SPDX-License-Identifier: GPL-2.0-only
/* Narrow isolated counterpart to original D1946 3-bank/1-byte transport. */
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/string.h>
#include "p105-bank0.h"
#define PMU_ADDR 0x3c
#define MAX_COUNT 9

int p105_bank0_init(struct p105_bank0 *b)
{
	if (IS_ERR_OR_NULL(b))
		return -EINVAL;
	if (b->initialized)
		return -EALREADY;
	if (memchr_inv(b, 0, sizeof(*b)))
		return -EINVAL;
	mutex_init(&b->io_lock);
	atomic64_set(&b->generation, 1);
	atomic_set(&b->page0_known, 0); /* Initial bank is unknown, never assume0. */
	atomic_set(&b->fault, 0);
	b->initialized = true;
	return 0;
}

void p105_bank0_invalidate(struct p105_bank0 *b)
{
	if (!IS_ERR_OR_NULL(b) && b->initialized) {
		atomic_set(&b->page0_known, 0);
		if (!atomic64_inc_return(&b->generation))
			atomic_set(&b->fault, 1); /* Wrapped generation cannot authorize an old lease. */
	}
}

static int bank0_valid(struct p105_bank0 *b, const struct p105_bank0_lease *lease,
		       u64 generation)
{
	int ret;

	if (atomic_read(&b->fault) || atomic64_read(&b->generation) != generation)
		return -ESTALE;
	ret = b->ops->valid(b->ctx, lease);
	if (ret)
		return ret > 0 ? -EPROTO : ret;
	if (atomic_read(&b->fault) || atomic64_read(&b->generation) != generation)
		return -ESTALE;
	return 0;
}

static bool bank0_range(unsigned int reg, unsigned int count, unsigned int lo,
			unsigned int hi)
{
	return reg >= lo && reg <= hi && count <= hi - reg + 1;
}

/* Deliberately excludes selector reads, other banks, OTP/init tables and unknown bytes. */
static bool bank0_allowed(const struct p105_bank0_lease *lease, bool write,
			 unsigned int reg, unsigned int count)
{
	if (!count || count > MAX_COUNT || reg > 0xff)
		return false;
	if (bank0_range(reg, count, 1, 9))
		return !write && lease->event_owner;
	if (bank0_range(reg, count, 0x0a, 0x0f))
		return !write;
	if (bank0_range(reg, count, 0x12, 0x1a))
		return !write || lease->event_owner;
	if (bank0_range(reg, count, 0x1b, 0x1c))
		return true;
	if ((reg == 0x3d || reg == 0x3f) && count == 1)
		return true;
	if (reg == 0x43 && count == 1)
		return true;
	if (bank0_range(reg, count, 0x44, 0x45))
		return !write;
	return count == 1 && reg >= 0x4b && reg <= 0x53 && (reg & 1);
}

int p105_bank0_transfer(struct p105_bank0 *b, const struct p105_bank0_lease *lease,
		       bool write, unsigned int reg, void *buffer, unsigned int count)
{
	struct i2c_msg msgs[2];
	u8 selector[] = {0x1e, 0};
	u8 data[MAX_COUNT + 1];
	u8 prefix;
	u64 generation;
	int ret, messages;

	if (IS_ERR_OR_NULL(buffer) || !count || count > MAX_COUNT)
		return -EINVAL;
	if (!write)
		memset(buffer, 0, count);
	if (IS_ERR_OR_NULL(b) || !b->initialized || IS_ERR_OR_NULL(lease) ||
	    !bank0_allowed(lease, write, reg, count))
		return -EINVAL;
	mutex_lock(&b->io_lock);
	ret = -EACCES;
	if (!b->enabled || atomic_read(&b->fault))
		goto unlock;
	ret = -EOPNOTSUPP;
	if (IS_ERR_OR_NULL(b->client) || IS_ERR_OR_NULL(b->client->adapter) ||
	    IS_ERR_OR_NULL(b->ops) || !b->ops->valid || !b->ops->hold_unknown ||
	    !lease->exclusive_all_pmu)
		goto unlock;
	ret = -EINVAL;
	if (b->client->addr != PMU_ADDR || (b->client->flags & I2C_CLIENT_TEN))
		goto unlock;
	generation = atomic64_read(&b->generation);
	ret = bank0_valid(b, lease, generation);
	if (ret) {
		if (atomic_read(&b->page0_known))
			goto fault; /* Do not resume a held cache after owner validation loss. */
		goto unlock;
	}
	if (!atomic_read(&b->page0_known) ||
	    b->selected_owner_generation != lease->owner_generation) {
		/* Proven selector WRITE only. No readable-selector semantics assumed. */
		msgs[0] = (struct i2c_msg){.addr = PMU_ADDR, .len = sizeof(selector),
					.buf = selector};
		ret = i2c_transfer(b->client->adapter, msgs, 1);
		if (ret != 1) {
			if (ret >= 0)
				ret = -EIO;
			goto fault;
		}
		ret = bank0_valid(b, lease, generation);
		if (ret)
			goto fault;
		b->selected_owner_generation = lease->owner_generation;
		atomic_set(&b->page0_known, 1);
	}
	ret = bank0_valid(b, lease, generation);
	if (ret)
		goto fault;
	if (write) {
		data[0] = reg;
		memcpy(data + 1, buffer, count);
		msgs[0] = (struct i2c_msg){.addr = PMU_ADDR, .len = count + 1, .buf = data};
		messages = 1;
	} else {
		prefix = reg;
		msgs[0] = (struct i2c_msg){.addr = PMU_ADDR, .len = 1, .buf = &prefix};
		msgs[1] = (struct i2c_msg){.addr = PMU_ADDR, .flags = I2C_M_RD,
					.len = count, .buf = buffer};
		messages = 2;
	}
	ret = i2c_transfer(b->client->adapter, msgs, messages);
	if (ret != messages) {
		if (ret >= 0)
			ret = -EIO;
		goto fault;
	}
	ret = bank0_valid(b, lease, generation);
	if (ret)
		goto fault;
	goto unlock;
fault:
	atomic_set(&b->page0_known, 0);
	atomic_set(&b->fault, 1);
	atomic64_inc(&b->generation);
	b->retained_lease = *lease;
	b->retained_lease_valid = true;
	b->transport_error = ret;
	b->hold_error = b->ops->hold_unknown(b->ctx, lease, ret);
unlock:
	if (ret && !write)
		memset(buffer, 0, count);
	mutex_unlock(&b->io_lock);
	return ret;
}
