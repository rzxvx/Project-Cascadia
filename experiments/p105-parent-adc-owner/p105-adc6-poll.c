// SPDX-License-Identifier: GPL-2.0-only
/* Source-only checked polling counterpart to pinned13G36 iBoot c88c ADC6. */
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/ktime.h>
#include <linux/string.h>
#include "p105-adc6-poll.h"
#define ADC6_INTERVAL_US 1000
#define ADC6_TIMEOUT_NS (50000ULL * 1000)
#define ADC6_MAX_POLLS 64

/* Caller supplies fresh zeroed storage; never reset a live lock/fault/lease. */
int p105_adc6_init(struct p105_adc6 *a)
{
	if (IS_ERR_OR_NULL(a))
		return -EINVAL;
	if (a->initialized)
		return -EALREADY;
	if (memchr_inv(a, 0, sizeof(*a)))
		return -EINVAL;
	mutex_init(&a->io_lock);
	atomic64_set(&a->generation, 1);
	atomic_set(&a->unknown, 0);
	a->initialized = true;
	return 0;
}

void p105_adc6_invalidate(struct p105_adc6 *a)
{
	if (!IS_ERR_OR_NULL(a) && a->initialized &&
	    !atomic64_inc_return(&a->generation))
		atomic_set(&a->unknown, 1);
}

static int adc6_checked(int ret)
{
	return ret > 0 ? -EPROTO : ret;
}

static int adc6_valid(struct p105_adc6 *a, const struct p105_adc6_lease *lease,
		      u64 generation)
{
	int ret;

	if (atomic_read(&a->unknown) || atomic64_read(&a->generation) != generation)
		return -ESTALE;
	ret = adc6_checked(a->ops->valid(a->ctx, lease));
	if (ret)
		return ret;
	if (atomic_read(&a->unknown) || atomic64_read(&a->generation) != generation)
		return -ESTALE;
	return 0;
}

int p105_adc6_read(struct p105_adc6 *a, const struct p105_adc6_lease *lease,
		   struct p105_adc6_sample *out)
{
	struct p105_adc6_sample candidate = {};
	u64 generation, now;
	unsigned int control, polls;
	u8 raw[2];
	bool started = false, uncertain = false;
	int ret, cleanup_ret = 0, hold_ret;

	if (IS_ERR_OR_NULL(out))
		return -EINVAL;
	memset(out, 0, sizeof(*out));
	if (IS_ERR_OR_NULL(a) || !a->initialized || IS_ERR_OR_NULL(lease))
		return -EINVAL;
	mutex_lock(&a->io_lock);
	ret = -EACCES;
	if (!a->enabled || atomic_read(&a->unknown))
		goto unlock;
	ret = -EOPNOTSUPP;
	if (IS_ERR_OR_NULL(a->pmu) || IS_ERR_OR_NULL(a->ops) ||
	    !a->ops->valid || !a->ops->hold_unknown ||
	    !lease->exclusive_all_adc || !lease->quiescence_proven)
		goto unlock;
	generation = atomic64_read(&a->generation);
	ret = adc6_valid(a, lease, generation);
	if (ret)
		goto unlock;
	ret = regmap_read(a->pmu, 0x43, &control);
	if (ret)
		goto unknown;
	/* A read alone cannot prove this; the owner's quiescence proof is mandatory. */
	if (control != 0) {
		ret = -EBUSY;
		goto unknown;
	}
	ret = adc6_valid(a, lease, generation);
	if (ret)
		goto unlock;
	candidate.started_ns = ktime_get_ns();
	started = true; /* Even a failed I2C write may have reached the PMU. */
	ret = regmap_write(a->pmu, 0x43, 6 | 0x20);
	if (ret)
		goto cleanup;
	for (polls = 0; polls < ADC6_MAX_POLLS; polls++) {
		usleep_range(ADC6_INTERVAL_US, ADC6_INTERVAL_US + 100);
		now = ktime_get_ns();
		if (now < candidate.started_ns ||
		    now - candidate.started_ns > ADC6_TIMEOUT_NS) {
			ret = -ETIMEDOUT;
			goto cleanup;
		}
		ret = adc6_valid(a, lease, generation);
		if (ret)
			goto cleanup;
		ret = regmap_read(a->pmu, 0x43, &control);
		if (ret)
			goto cleanup;
		/* Busy20 can legitimately clear before the first read. */
		if (control != 6 && control != (6 | 0x20)) {
			ret = -ESTALE;
			goto cleanup;
		}
		if (!(control & 0x20))
			break;
	}
	if (polls == ADC6_MAX_POLLS) {
		ret = -ETIMEDOUT;
		goto cleanup;
	}
	ret = adc6_valid(a, lease, generation);
	if (ret)
		goto cleanup;
	ret = regmap_bulk_read(a->pmu, 0x44, raw, sizeof(raw));
	if (ret)
		goto cleanup;
	candidate.raw = (raw[0] & 15) | ((u16)raw[1] << 4);
	candidate.completed_ns = ktime_get_ns();
	if (candidate.completed_ns < candidate.started_ns ||
	    candidate.completed_ns - candidate.started_ns > ADC6_TIMEOUT_NS) {
		ret = -ETIMEDOUT;
		goto cleanup;
	}
	ret = adc6_valid(a, lease, generation);
cleanup:
	uncertain = ret != 0;
	/* Stop is allowed with the held resource lifetime even after epoch invalidation. */
	if (started) {
		cleanup_ret = regmap_write(a->pmu, 0x43, 0);
		if (!cleanup_ret) {
			cleanup_ret = regmap_read(a->pmu, 0x43, &control);
			if (!cleanup_ret && control != 0)
				cleanup_ret = -EIO;
		}
		if (cleanup_ret) {
			if (!ret)
				ret = cleanup_ret;
			uncertain = true;
		}
	}
	if (!ret)
		ret = adc6_valid(a, lease, generation);
	if (ret)
		uncertain = true;
	if (uncertain)
		goto unknown;
	candidate.generation = generation;
	*out = candidate;
	goto unlock;
unknown:
	/* Checked stop does not prove cancellation/late-completion quiescence. */
	atomic_set(&a->unknown, 1);
	atomic64_inc_return(&a->generation);
	a->retained_lease = *lease;
	a->retained_lease_valid = true;
	a->first_error = ret;
	a->cleanup_error = cleanup_ret;
	hold_ret = adc6_checked(a->ops->hold_unknown(a->ctx, lease, ret));
	a->hold_error = hold_ret;
unlock:
	mutex_unlock(&a->io_lock);
	return ret;
}
