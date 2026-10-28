// SPDX-License-Identifier: GPL-2.0-only
/* Pinned P105 12H321 host subset. No probe, clk-provider or role publication. */
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/lockdep.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/string.h>
#include "cascadia-native-clock.h"

#define CLOCK_ENABLE 0x80000000U
#define CLOCK_BUSY 0x40000000U
#define GATE_CLEAR_MASK 0x10fU
#define POLL_MAX 1000
#define ZERO_OFF_SUPPORTED ((1ULL << 32) | (1ULL << 34) | (1ULL << 38))
/* Exact decoded metadata from the 44-byte ADT table; no firmware bytes. */
struct native_node {
	u8 id;
	u8 parents[4]; /* indexes in this fixed acyclic table; 0xff terminates */
	u8 clock;
	u8 gate;
};
static const struct native_node nodes[CASCADIA_NATIVE_NODES] = {
	{ 2, { 0xff, 0xff, 0xff, 0xff }, 4, 0 },
	{ 3, { 0xff, 0xff, 0xff, 0xff }, 5, 0 },
	{ 4, { 0xff, 0xff, 0xff, 0xff }, 6, 0 },
	{ 5, { 0xff, 0xff, 0xff, 0xff }, 7, 0 },
	{ 40, { 0, 1, 2, 3 }, 25, 32 },
	{ 42, { 4, 0xff, 0xff, 0xff }, 45, 38 },
	{ 87, { 4, 5, 0xff, 0xff }, 0, 0 },
	{ 89, { 4, 0xff, 0xff, 0xff }, 0, 34 },
};

int cascadia_native_clock_init(struct cascadia_native_clock *c,
	struct regmap *pmgr, struct mutex *owner_lock)
{
	if (!c || IS_ERR_OR_NULL(pmgr) || IS_ERR_OR_NULL(owner_lock))
		return -EINVAL;
	if (c->initialized)
		return -EALREADY;
	memset(c, 0, sizeof(*c));
	c->pmgr = pmgr;
	c->owner_lock = owner_lock;
	c->poll_limit = 100;
	c->initialized = true;
	return 0;
}

static int latch(struct cascadia_native_clock *c, int ret)
{
	c->fault = true;
	c->fault_error = ret < 0 ? ret : -EIO;
	return c->fault_error;
}

static int epoch_check(struct cascadia_native_clock *c, u64 epoch)
{
	int ret = c->validate_epoch(c->context, epoch);

	return ret > 0 ? -EIO : ret;
}

static bool certificate_complete(const struct cascadia_native_adoption *a)
{
	return a->p105_source_binding_verified &&
		a->dynamic_provider_association_verified &&
		a->all_global_owners_adopted && a->inherited_hardware_verified &&
		a->all_shared_clients_quiesced && a->gate_power_policy_owned;
}

static int admit(struct cascadia_native_clock *c, u64 epoch)
{
	int ret;

	if (!c || !c->initialized)
		return -EINVAL;
	if (!mutex_is_locked(c->owner_lock))
		return -EPERM;
	lockdep_assert_held(c->owner_lock);
	if (c->fault)
		return c->fault_error;
	if (!c->permitted)
		return -EACCES;
	if (!c->adopted || !certificate_complete(&c->certificate))
		return -EOPNOTSUPP;
	if (!c->validate_epoch)
		return latch(c, -ENODEV);
	if (epoch != c->certificate.epoch)
		return -ESTALE;
	if (!c->poll_limit || c->poll_limit > POLL_MAX)
		return -EINVAL;
	ret = epoch_check(c, epoch);
	if (ret && c->adopted)
		return latch(c, ret);
	return ret;
}

static int read_checked(struct cascadia_native_clock *c, u64 epoch,
	unsigned int reg, unsigned int *value)
{
	int ret = epoch_check(c, epoch);

	if (ret)
		return ret;
	ret = regmap_read(c->pmgr, reg, value);
	if (ret)
		return ret;
	return epoch_check(c, epoch);
}

static int write_checked(struct cascadia_native_clock *c, u64 epoch,
	unsigned int reg, unsigned int value)
{
	int ret = epoch_check(c, epoch);

	if (ret)
		return ret;
	/* A regmap error can be returned after the hardware write happened. */
	c->touched = true;
	ret = regmap_write(c->pmgr, reg, value);
	if (ret)
		return ret;
	return epoch_check(c, epoch);
}

static bool active(const struct cascadia_native_state *s, unsigned int n)
{
	if (n < 6)
		return s->refs[n] != 0;
	return s->owners[n - 6] || (s->foreign_roots & (1U << (n - 6)));
}

static int state_valid(const struct cascadia_native_state *s)
{
	unsigned int i;

	if (s->foreign_roots & ~3U)
		return -EINVAL;
	if (s->refs[5] && !s->refs[4])
		return -EINVAL;
	for (i = 0; i < 4; i++)
		if (s->refs[4] && !s->refs[i])
			return -EINVAL;
	if (active(s, 6) && (s->refs[4] < 2 || !s->refs[5]))
		return -EINVAL;
	if (active(s, 7) && !s->refs[4])
		return -EINVAL;
	if (active(s, 6) && active(s, 7) && s->refs[4] < 3)
		return -EINVAL;
	return 0;
}

static int clock_apply(struct cascadia_native_clock *c, u64 epoch,
	unsigned int index, bool on)
{
	unsigned int reg = 0x38 + index * 4, value, next, attempt;
	int ret;

	/* These are only direct codes1..51; encoded C0/domain paths excluded. */
	if (!index || index > 51)
		return -EOPNOTSUPP;
	ret = read_checked(c, epoch, reg, &value);
	if (ret)
		return ret;
	next = on ? value | CLOCK_ENABLE : value & ~CLOCK_ENABLE;
	if (next != value) {
		ret = write_checked(c, epoch, reg, next);
		if (ret)
			return ret;
	}
	for (attempt = 0; attempt < c->poll_limit; attempt++) {
		ret = read_checked(c, epoch, reg, &value);
		if (ret)
			return ret;
		if (!(value & CLOCK_BUSY) && !!(value & CLOCK_ENABLE) == on)
			return 0;
		usleep_range(1, 2);
	}
	return -ETIMEDOUT;
}

static int gate_apply(struct cascadia_native_clock *c, u64 epoch,
	unsigned int index, bool on)
{
	unsigned int reg = 0x1000 + index * 4, value, next, attempt;
	unsigned int target = on ? 15 :
		((c->certificate.zero_off_gates & (1ULL << index)) ? 0 : 4);
	int ret = read_checked(c, epoch, reg, &value);

	if (ret)
		return ret;
	if (value & 0x80000000U)
		return -EBUSY; /* Controller reset belongs to its separate owner. */
	/* Native monotonic target policy, but readiness is ALWAYS checked. */
	if ((on && (value & 15) < target) ||
	    (!on && (value & 15) > target)) {
		next = (value & ~GATE_CLEAR_MASK) | target;
		ret = write_checked(c, epoch, reg, next);
		if (ret)
			return ret;
	}
	for (attempt = 0; attempt < c->poll_limit; attempt++) {
		ret = read_checked(c, epoch, reg, &value);
		if (ret)
			return ret;
		if (value & 0x80000000U)
			return -EBUSY;
		if ((value & 15) == target && ((value >> 4) & 15) == target)
			return 0;
		usleep_range(1, 2);
	}
	return -ETIMEDOUT;
}

static int verify_active(struct cascadia_native_clock *c, u64 epoch,
	const struct cascadia_native_state *s)
{
	unsigned int i, value;
	int ret;

	for (i = 0; i < CASCADIA_NATIVE_NODES; i++) {
		if (!active(s, i))
			continue;
		if (nodes[i].clock) {
			ret = read_checked(c, epoch, 0x38 + 4 * nodes[i].clock, &value);
			if (ret)
				return ret;
			if ((value & (CLOCK_ENABLE | CLOCK_BUSY)) != CLOCK_ENABLE)
				return -EHOSTDOWN;
		}
		if (nodes[i].gate) {
			ret = read_checked(c, epoch, 0x1000 + 4 * nodes[i].gate, &value);
			if (ret)
				return ret;
			if ((value & 0x800000ffU) != 0xff)
				return -EHOSTDOWN;
		}
	}
	return epoch_check(c, epoch);
}

int cascadia_native_clock_adopt(struct cascadia_native_clock *c,
	const struct cascadia_native_adoption *a)
{
	int ret;

	if (!c || !c->initialized || !a)
		return -EINVAL;
	if (!mutex_is_locked(c->owner_lock))
		return -EPERM;
	lockdep_assert_held(c->owner_lock);
	if (c->fault)
		return c->fault_error;
	if (c->adopted)
		return -EALREADY;
	if (!c->permitted)
		return -EACCES;
	if (!certificate_complete(a))
		return -EOPNOTSUPP;
	if (a->zero_off_gates & ~ZERO_OFF_SUPPORTED)
		return -EOPNOTSUPP;
	if (!c->validate_epoch)
		return -ENODEV;
	if (a->baseline.owners[0] || a->baseline.owners[1])
		return -EINVAL; /* Own references are acquired after adoption. */
	ret = state_valid(&a->baseline);
	if (ret)
		return ret;
	ret = verify_active(c, a->epoch, &a->baseline);
	if (ret)
		return ret; /* No mutation, no write, no adopted lease on failure. */
	c->certificate = *a;
	c->state = a->baseline;
	c->adopted = true;
	return 0;
}

static int parent_change(struct cascadia_native_state *s,
	unsigned int n, bool on)
{
	unsigned int i;
	bool changed;
	int ret;

	if (n >= 6)
		return -EINVAL;
	if (on) {
		if (s->refs[n] == ~0U)
			return -EOVERFLOW;
		changed = s->refs[n]++ == 0;
	} else {
		if (!s->refs[n])
			return -EINVAL;
		changed = --s->refs[n] == 0;
	}
	if (!changed)
		return 0;
	for (i = 0; i < 4 && nodes[n].parents[i] != 0xff; i++) {
		ret = parent_change(s, nodes[n].parents[i], on);
		if (ret)
			return ret;
	}
	return 0;
}

static int apply_plan(struct cascadia_native_clock *c, u64 epoch, bool on)
{
	int index, i, ret;

	/* Real native order: clocks asc/gates asc; gates desc/clocks desc. */
	if (on) {
		for (index = 1; index <= 51; index++)
			for (i = 0; i < CASCADIA_NATIVE_NODES; i++)
				if (nodes[i].clock == index && !active(&c->state, i) &&
				    active(&c->pending, i)) {
					ret = clock_apply(c, epoch, index, true);
					if (ret)
						return ret;
				}
	}
	for (index = on ? 1 : 95; on ? index <= 95 : index > 0;
	     on ? index++ : index--)
		for (i = 0; i < CASCADIA_NATIVE_NODES; i++)
			if (nodes[i].gate == index &&
			    active(&c->state, i) != active(&c->pending, i)) {
				ret = gate_apply(c, epoch, index, on);
				if (ret)
					return ret;
			}
	if (!on) {
		for (index = 51; index > 0; index--)
			for (i = 0; i < CASCADIA_NATIVE_NODES; i++)
				if (nodes[i].clock == index && active(&c->state, i) &&
				    !active(&c->pending, i)) {
					ret = clock_apply(c, epoch, index, false);
					if (ret)
						return ret;
				}
	}
	return verify_active(c, epoch, &c->pending);
}

int cascadia_native_clock_request(struct cascadia_native_clock *c, u64 epoch,
	unsigned int root, unsigned int owner, bool enable)
{
	unsigned int r, n, i, bit;
	bool before, after;
	int ret = admit(c, epoch);

	if (ret)
		return ret;
	if (root != 87 && root != 89)
		return -EOPNOTSUPP; /* 88 domain path/OHCI/C0 formats not implemented. */
	if (owner >= 32)
		return -EINVAL;
	r = root == 87 ? 0 : 1;
	n = r + 6;
	bit = 1U << owner;
	if (enable && (c->state.owners[r] & bit))
		return -EALREADY;
	if (!enable && !(c->state.owners[r] & bit))
		return -ENOENT;
	c->pending = c->state;
	before = active(&c->pending, n);
	if (enable)
		c->pending.owners[r] |= bit;
	else
		c->pending.owners[r] &= ~bit;
	after = active(&c->pending, n);
	if (before != after) {
		for (i = 0; i < 4 && nodes[n].parents[i] != 0xff; i++) {
			ret = parent_change(&c->pending, nodes[n].parents[i], enable);
			if (ret)
				return ret; /* Pending software plan only; no I/O occurred. */
		}
	}
	ret = state_valid(&c->pending);
	if (ret)
		return ret;
	c->pending_root = root;
	c->pending_enable = enable;
	c->pending_valid = true;
	ret = apply_plan(c, epoch, enable);
	if (ret)
		return latch(c, ret); /* Retain committed + unknown pending disposition. */
	ret = epoch_check(c, epoch);
	if (ret)
		return latch(c, ret);
	c->state = c->pending;
	c->pending_valid = false;
	c->completed_operations++;
	return 0;
}
