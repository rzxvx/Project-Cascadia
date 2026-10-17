// SPDX-License-Identifier: GPL-2.0-only
/* Private, uninstalled owner: real shared exclusion; physical admission closed. */
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/string.h>
#include "p105-owned-adc6.h"

struct p105_owned_adc6 {
	struct mutex serial;
	atomic_t stopping;
	struct d1946_bank0_bus *bus;
	struct p105_adc6 poll;
	u64 epoch;
};

static int owned_valid(void *context, const struct p105_adc6_lease *lease)
{
	struct p105_owned_adc6 *o = context;

	if (atomic_read(&o->stopping) || lease->owner_generation != o->epoch)
		return -ESHUTDOWN;
	return d1946_bank0_adc_valid(o->bus, o->epoch);
}

static int owned_hold(void *context, const struct p105_adc6_lease *lease, int error)
{
	struct p105_owned_adc6 *o = context;

	(void)lease;
	(void)error;
	d1946_bank0_adc_invalidate(o->bus);
	return 0;
}

static const struct p105_adc6_ops owned_ops = {
	.valid = owned_valid,
	.hold_unknown = owned_hold,
};

static const struct regmap_bus owned_bus = {
	.write = d1946_bank0_adc_write,
	.read = d1946_bank0_adc_read,
	.max_raw_read = 2,
	.max_raw_write = 1,
};

static void owned_regmap_exit(void *context)
{
	regmap_exit(context);
}

struct p105_owned_adc6 *p105_owned_adc6_create(struct device *dev,
					    struct d1946_bank0_bus *bus)
{
	const struct regmap_config cfg = {
		.reg_bits = 8,
		.val_bits = 8,
		.max_register = 0x45,
		.cache_type = REGCACHE_NONE,
		.disable_locking = true, /* Parent transaction holds both locks. */
		.can_sleep = true,
	};
	struct p105_owned_adc6 *o;
	int ret;

	if (IS_ERR_OR_NULL(dev) || IS_ERR_OR_NULL(bus) ||
	    IS_ERR_OR_NULL(bus->client) || dev != &bus->client->dev)
		return ERR_PTR(-EINVAL);
	o = devm_kzalloc(dev, sizeof(*o), GFP_KERNEL);
	if (!o)
		return ERR_PTR(-ENOMEM);
	mutex_init(&o->serial);
	atomic_set(&o->stopping, 0);
	o->bus = bus;
	ret = p105_adc6_init(&o->poll);
	if (ret)
		return ERR_PTR(ret);
	/* NULL device prevents dev_get_regmap(parent, NULL) from seeing the
	 * lockless private facade; disable_locking also disables its debugfs. */
	o->poll.pmu = regmap_init(NULL, &owned_bus, bus, &cfg);
	if (IS_ERR(o->poll.pmu))
		return ERR_CAST(o->poll.pmu);
	ret = devm_add_action_or_reset(dev, owned_regmap_exit, o->poll.pmu);
	if (ret)
		return ERR_PTR(ret);
	o->poll.ops = &owned_ops;
	o->poll.ctx = o;
	ret = d1946_bank0_adc_claim(bus);
	if (ret)
		return ERR_PTR(ret);
	ret = devm_add_action_or_reset(dev, p105_owned_adc6_close, o);
	if (ret)
		return ERR_PTR(ret);
	/* Software ready, but begin denies EUCLEAN before any PMU access.
	 * No current API can mint a physical first-quiescence witness. */
	o->poll.enabled = true;
	return o;
}

int p105_owned_adc6_read(struct p105_owned_adc6 *o, struct p105_adc6_sample *out)
{
	struct p105_adc6_lease lease;
	int ret;

	if (IS_ERR_OR_NULL(out))
		return -EINVAL;
	memset(out, 0, sizeof(*out));
	if (IS_ERR_OR_NULL(o))
		return -EINVAL;
	mutex_lock(&o->serial);
	if (atomic_read(&o->stopping)) {
		ret = -ESHUTDOWN;
		goto unlock;
	}
	ret = d1946_bank0_adc_begin(o->bus, &o->epoch);
	if (ret)
		goto unlock;
	lease = (struct p105_adc6_lease){
		.owner_generation = o->epoch,
		.exclusive_all_adc = true, /* Actual map+adapter locks held. */
		.quiescence_proven = true, /* Begin checked parent's witness. */
	};
	ret = p105_adc6_read(&o->poll, &lease, out);
	if (!ret)
		ret = owned_valid(o, &lease);
	if (ret) {
		memset(out, 0, sizeof(*out));
		d1946_bank0_adc_invalidate(o->bus);
	}
	d1946_bank0_adc_end(o->bus);
unlock:
	mutex_unlock(&o->serial);
	return ret;
}

void p105_owned_adc6_close(void *context)
{
	struct p105_owned_adc6 *o = context;

	atomic_set(&o->stopping, 1);
	d1946_bank0_adc_invalidate(o->bus);
	mutex_lock(&o->serial);
	mutex_unlock(&o->serial);
	/* Calling framework must have stopped new users before devres free.
	 * This drains admitted calls, not escaped handle lifetime. */
}
