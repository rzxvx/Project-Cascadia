// SPDX-License-Identifier: GPL-2.0-only
/* Unregistered research candidate; original13G36 software arithmetic, checked I/O. */
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/string.h>
#include "p105-wall-source.h"

int p105_wall_init(struct p105_wall_source *s)
{
	const unsigned char *bytes;
	size_t i;

	if (IS_ERR_OR_NULL(s))
		return -EINVAL;
	bytes = (const unsigned char *)s;
	for (i = 0; i < sizeof(*s); i++)
		if (bytes[i])
			return -EALREADY;
	mutex_init(&s->io_lock);
	atomic64_set(&s->generation, 1);
	atomic_set(&s->fault, 0);
	s->initialized = true;
	return 0;
}

void p105_wall_invalidate(struct p105_wall_source *s, bool fault)
{
	if (IS_ERR_OR_NULL(s) || !s->initialized)
		return;
	if (!atomic64_inc_return(&s->generation))
		fault = true; /* Old observation identities may not be reused. */
	if (fault)
		atomic_set(&s->fault, 1);
}

/* ENODATA requests the third sample; only bounded original encoded scalars accepted. */
int p105_wall_classify(u32 a, u32 b, bool third_valid, u32 c,
		       struct p105_wall_observation *out)
{
	u32 ratio, id;

	if (IS_ERR_OR_NULL(out))
		return -EINVAL;
	memset(out, 0, sizeof(*out));
	if (a > 4998 || b > 4998 || (third_valid && c > 4998))
		return -EINVAL;
	ratio = b ? a * 1000 / b : 0;
	if (ratio >= 1291 && ratio < 1506)
		id = 2;
	else if (ratio >= 665 && ratio < 776)
		id = 4;
	else
		id = (a <= 2220 ? 0 : a <= 2890 ? 1 : 2) +
		     3 * (b <= 2220 ? 0 : b <= 2890 ? 1 : 2) + 1;
	out->first_scalar = a;
	out->second_scalar = b;
	if (a >= 1000 && b >= 1000) {
		out->source_type = ratio >= 500 && ratio <= 2000 ? 4 : 3;
		if (out->source_type == 4)
			out->divider_id = id;
		return 0;
	}
	if (!third_valid)
		return -ENODATA;
	out->third_sample = true;
	out->third_scalar = c;
	out->source_type = c >= 251 && c <= 799 ? 5 : 3;
	return 0;
}

static int wall_checked(int ret)
{
	return ret > 0 ? -EPROTO : ret;
}

static int wall_valid(struct p105_wall_source *s,
		      const struct p105_wall_lease *lease, u64 generation)
{
	int ret;

	if (atomic_read(&s->fault) || atomic64_read(&s->generation) != generation)
		return -ESTALE;
	ret = wall_checked(s->ops->validate(s->ctx, lease));
	if (ret)
		return ret;
	if (atomic_read(&s->fault) || atomic64_read(&s->generation) != generation)
		return -ESTALE;
	return 0;
}

static int wall_presence(struct p105_wall_source *s,
			 const struct p105_wall_lease *lease, u64 generation)
{
	u8 status[6];
	int ret;

	ret = wall_valid(s, lease, generation);
	if (ret)
		return ret;
	/* No read-to-clear event registers; status0a..0f only. */
	ret = regmap_bulk_read(s->pmu, 0x0a, status, sizeof(status));
	if (ret)
		return ret;
	if (!(status[0] & 8) || (status[1] & 0x38))
		return -ENODATA;
	return wall_valid(s, lease, generation);
}

static int wall_write(struct p105_wall_source *s,
		      const struct p105_wall_lease *lease, u64 generation,
		      unsigned int reg, unsigned int value)
{
	unsigned int check;
	int ret;

	ret = wall_valid(s, lease, generation);
	if (ret)
		return ret;
	ret = regmap_write(s->tristar, reg, value);
	if (ret)
		return ret;
	ret = regmap_read(s->tristar, reg, &check);
	if (ret)
		return ret;
	if (check != value)
		return -EIO;
	return wall_valid(s, lease, generation);
}

static int wall_route(struct p105_wall_source *s,
		      struct p105_wall_lease *lease, u64 generation,
		      unsigned int mode)
{
	unsigned int status, pins, orientation, r1, r3;
	int ret;

	if (mode != 0 && mode != 1 && mode != 2 && mode != 4)
		return -EINVAL;
	ret = wall_presence(s, lease, generation);
	if (ret)
		return ret;
	ret = regmap_read(s->tristar, 0x0c, &status);
	if (ret)
		return ret;
	if ((status & 0x84) != 0x80)
		return -EAGAIN;
	ret = regmap_read(s->tristar, 6, &pins);
	if (ret)
		return ret;
	if (!(((pins >> 2) ^ (pins >> 3)) & 1))
		return -EAGAIN;
	orientation = (status >> 6) & 1;
	if (lease->orientation_seen && lease->orientation != orientation)
		return -ESTALE;
	lease->orientation_seen = true;
	lease->orientation = orientation;
	r1 = mode == 0 ? 0 : mode == 1 ? (orientation ? 0xc0 : 0x0c) :
					(orientation ? 0xd0 : 0x0d);
	r3 = mode == 4 ? (orientation ? 0x0b : 9) : 0;
	ret = wall_write(s, lease, generation, 3, r3);
	if (ret)
		return ret;
	return wall_write(s, lease, generation, 1, r1);
}

static int wall_sample(struct p105_wall_source *s,
		       struct p105_wall_lease *lease, u64 generation,
		       unsigned int route, unsigned int settle, u32 *scalar)
{
	u16 raw;
	int ret;

	ret = wall_route(s, lease, generation, route);
	if (ret)
		return ret;
	ret = wall_checked(s->ops->settle(s->ctx, lease, settle));
	if (ret)
		return ret;
	ret = wall_presence(s, lease, generation);
	if (ret)
		return ret;
	ret = wall_checked(s->ops->adc6_raw(s->ctx, lease, &raw));
	if (ret)
		return ret;
	if (raw > 4095)
		return -ERANGE;
	*scalar = (u32)raw * 5000 >> 12;
	ret = wall_route(s, lease, generation, 0);
	if (ret)
		return ret;
	return wall_presence(s, lease, generation);
}

int p105_wall_capture(struct p105_wall_source *s, struct p105_wall_observation *out)
{
	struct p105_wall_observation candidate;
	struct p105_wall_lease lease = {};
	u64 generation;
	u32 a, b, c;
	int ret, release_ret;

	if (IS_ERR_OR_NULL(out))
		return -EINVAL;
	memset(out, 0, sizeof(*out));
	if (IS_ERR_OR_NULL(s) || !s->initialized)
		return -EINVAL;
	mutex_lock(&s->io_lock);
	ret = -EACCES;
	if (!s->enabled || atomic_read(&s->fault))
		goto unlock;
	ret = -EOPNOTSUPP;
	if (IS_ERR_OR_NULL(s->pmu) || IS_ERR_OR_NULL(s->tristar) ||
	    IS_ERR_OR_NULL(s->ops) || !s->ops->acquire ||
	    !s->ops->validate || !s->ops->adc6_raw || !s->ops->settle ||
	    !s->ops->restore_release)
		goto unlock;
	generation = atomic64_read(&s->generation);
	ret = s->ops->acquire(s->ctx, &lease);
	if (ret > 0) {
		/* Invalid acquisition result cannot establish a releasable lease. */
		s->held_lease = lease;
		s->held_error = -EPROTO;
		s->lease_uncertain = true;
		p105_wall_invalidate(s, true);
		ret = -EPROTO;
		goto unlock;
	}
	if (ret)
		goto unlock;
	/* No concrete provider currently proves this restore contract. */
	ret = -EOPNOTSUPP;
	if (!lease.checked_restore_available)
		goto release;
	ret = wall_sample(s, &lease, generation, 2, 2000, &a);
	if (ret)
		goto release;
	ret = wall_sample(s, &lease, generation, 1, 2000, &b);
	if (ret)
		goto release;
	ret = p105_wall_classify(a, b, false, 0, &candidate);
	if (ret == -ENODATA) {
		ret = wall_sample(s, &lease, generation, 4, 40000, &c);
		if (!ret)
			ret = p105_wall_classify(a, b, true, c, &candidate);
	}
	if (!ret)
		ret = wall_presence(s, &lease, generation);
release:
	/* Always release, including unproved restore/acquire-success/error paths. */
	release_ret = wall_checked(s->ops->restore_release(s->ctx, &lease));
	if (release_ret) {
		/* Keep the failed release identity for diagnosis; never reinit away. */
		s->held_lease = lease;
		s->held_error = release_ret;
		s->lease_uncertain = true;
		p105_wall_invalidate(s, true);
		ret = release_ret;
	}
	if (!ret && (atomic_read(&s->fault) ||
		     atomic64_read(&s->generation) != generation))
		ret = -ESTALE;
	/* Observation only; provider lease has ended and cannot authorize current. */
	if (!ret) {
		candidate.generation = generation;
		*out = candidate;
	}
	if (ret)
		memset(out, 0, sizeof(*out));
unlock:
	mutex_unlock(&s->io_lock);
	return ret;
}
