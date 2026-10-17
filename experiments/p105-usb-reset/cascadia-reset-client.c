// SPDX-License-Identifier: GPL-2.0-only
/* Source-only P105 standard PHY reset ownership; no driver/DT registration. */
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/lockdep.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include "cascadia-reset-client.h"

static int epoch(struct cascadia_reset_client *c, u64 value)
{
	int ret = c->validate_epoch(c->context, value);

	return ret > 0 ? -EIO : ret;
}

static int lease(struct cascadia_reset_client *c, u64 value, bool take)
{
	int ret = epoch(c, value);

	if (ret)
		return ret;
	c->touched = true;
	/* An error can follow applied effects, including on release. */
	c->lease_maybe_owned = true;
	c->lease_known = false;
	ret = c->temporary_power_mask(c->context, value, take ? 16 : 0);
	if (ret)
		return ret > 0 ? -EIO : ret;
	c->lease_maybe_owned = take;
	c->lease_known = true;
	return epoch(c, value);
}

static int reset(struct cascadia_reset_client *c, u64 value, bool assert)
{
	unsigned int old;
	int ret = epoch(c, value);

	if (ret)
		return ret;
	ret = regmap_read(c->phy, 8, &old);
	if (ret)
		return ret > 0 ? -EIO : ret;
	ret = epoch(c, value);
	if (ret)
		return ret;
	c->touched = true;
	ret = regmap_write(c->phy, 8, assert ? old | 0x10 : old & ~0x10U);
	if (ret)
		return ret > 0 ? -EIO : ret;
	return epoch(c, value);
}

int cascadia_reset_client_init(struct cascadia_reset_client *c,
	struct regmap *phy, struct mutex *shared_lock)
{
	if (!c || IS_ERR_OR_NULL(phy) || IS_ERR_OR_NULL(shared_lock))
		return -EINVAL;
	if (c->initialized)
		return -EALREADY;
	/* Fresh zero storage; never erase held/unknown ownership on reinit. */
	c->phy = phy;
	c->shared_lock = shared_lock;
	c->initialized = true;
	return 0;
}

int cascadia_reset_client_update(struct cascadia_reset_client *c,
	u64 value, unsigned int owner, unsigned int kind, bool add)
{
	unsigned int i, old_kind = 0, group = 0;
	u8 bit, next;
	int ret;

	if (!c || !c->initialized || owner >= CASCADIA_RESET_OWNERS)
		return -EINVAL;
	/* Only original standard-reset group0/1/5. No HSIC2/3/4 writes. */
	if (kind != 0 && kind != 1 && kind != 5)
		return -EOPNOTSUPP;
	lockdep_assert_held(c->shared_lock);
	if (c->fault)
		return c->fault_error;
	if (!c->permitted)
		return -EACCES;
	if (!c->p105_binding_verified || !c->complete_standard_group_owned ||
	    !c->all_owners_quiesced || !c->state_adopted)
		return -EOPNOTSUPP;
	if (!c->validate_epoch || !c->temporary_power_mask) {
		ret = -ENODEV;
		goto failed;
	}
	ret = epoch(c, value);
	if (ret)
		goto failed;
	if (c->lease_maybe_owned)
		return -EIO;
	if (c->memberships[owner] && c->owner_epochs[owner] != value) {
		ret = -ESTALE;
		goto failed;
	}
	bit = 1U << kind;
	next = add ? c->memberships[owner] | bit : c->memberships[owner] & ~bit;
	/* Deliberate checked idempotence: firmware takes a temporary power
	 * lease even for these no-op cases; no register/membership effect needs
	 * it. Skip those needless native power transitions here.
	 */
	if (next == c->memberships[owner]) {
		ret = epoch(c, value);
		if (ret)
			goto failed;
		return 0;
	}
	c->pending_owner = owner;
	c->pending_mask = next;
	for (i = 0; i < CASCADIA_RESET_OWNERS; i++) {
		if (c->memberships[i] & ~35U)
			return -EINVAL;
		old_kind += !!(c->memberships[i] & bit);
		group += !!(c->memberships[i] & 1);
		group += !!(c->memberships[i] & 2);
		group += !!(c->memberships[i] & 32);
	}
	ret = lease(c, value, true);
	if (ret)
		goto failed;
	if ((add && !group) || (!add && group == 1)) {
		ret = reset(c, value, !add);
		if (ret)
			goto failed;
	}
	/* Original delay when the last member of this kind is removed, even
	 * if another standard kind still retains shared resetbit0x10.
	 */
	if (!add && old_kind == 1) {
		ret = epoch(c, value);
		if (ret)
			goto failed;
		udelay(20);
		ret = epoch(c, value);
		if (ret)
			goto failed;
	}
	ret = lease(c, value, false);
	if (ret)
		goto failed;
	ret = epoch(c, value);
	if (ret)
		goto failed;
	c->memberships[owner] = next;
	c->owner_epochs[owner] = next ? value : 0;
	return 0;
failed:
	/* Adoption includes every peer, even when this caller has no slot yet. */
	for (i = 0; i < CASCADIA_RESET_OWNERS; i++)
		if (c->memberships[i])
			break;
	if (c->touched || i < CASCADIA_RESET_OWNERS) {
		c->fault = true;
		c->fault_error = ret;
	}
	return ret;
}
