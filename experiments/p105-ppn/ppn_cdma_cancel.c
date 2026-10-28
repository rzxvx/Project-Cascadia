// SPDX-License-Identifier: GPL-2.0-only
#include "ppn_cdma_cancel.h"
#ifdef __KERNEL__
#include <linux/errno.h>
#else
#include <errno.h>
#endif

static bool same_key(const struct ppn_cdma_identity *a,
		     const struct ppn_cdma_identity *b)
{
	return a->controller == b->controller && a->epoch == b->epoch &&
		a->transfer == b->transfer && a->lease_cookie == b->lease_cookie &&
		a->channel == b->channel;
}

static int blocked(struct ppn_cdma_cancel *s, int error)
{
	s->phase = PPN_CDMA_CANCEL_HOLD;
	s->error = error;
	return error;
}

static int checked(int error)
{
	return error > 0 ? -EPROTO : error;
}

static int valid_request(const struct ppn_cdma_cancel *s,
			 const struct ppn_cdma_identity *key)
{
	if (!s || !key || s->phase == PPN_CDMA_CANCEL_UNINITIALIZED ||
	    !same_key(&s->identity, key))
		return -EINVAL;
	return 0;
}

int ppn_cdma_cancel_begin(struct ppn_cdma_cancel *s,
			  const struct ppn_cdma_cancel_adoption *a,
			  const struct ppn_cdma_cancel_ops *ops, void *ctx)
{
	const unsigned char *bytes;
	size_t i;

	if (!s || !a || !ops || !ops->validate_lease || !ops->read ||
	    !ops->write || !ops->now_us || !ops->delay_us)
		return -EINVAL;
	bytes = (const unsigned char *)s;
	for (i = 0; i < sizeof(*s); i++)
		if (bytes[i])
			return -EALREADY;
	if (!a->identity.controller || !a->identity.epoch ||
	    !a->identity.transfer || !a->identity.lease_cookie ||
	    a->identity.channel < 5 || a->identity.channel > 8)
		return -EINVAL;
	if (!a->own_submitted_lease || !a->exclusive || !a->initial_handoff_fenced)
		return -EPERM;
	s->identity = a->identity;
	s->ops = ops;
	s->ctx = ctx;
	s->phase = PPN_CDMA_CANCEL_READY;
	return 0;
}

static int time_left(struct ppn_cdma_cancel *s, u64 *left)
{
	u64 now = s->ops->now_us(s->ctx);

	if (now < s->last_us)
		return blocked(s, -EIO);
	s->last_us = now;
	if (now - s->start_us >= s->timeout_us)
		return blocked(s, -ETIMEDOUT);
	*left = s->timeout_us - (now - s->start_us);
	return 0;
}

static int checkpoint(struct ppn_cdma_cancel *s, u64 *left)
{
	int error = time_left(s, left);

	if (error)
		return error;
	error = checked(s->ops->validate_lease(s->ctx, &s->identity));
	if (error)
		return blocked(s, error);
	return time_left(s, left);
}

static int read_reg(struct ppn_cdma_cancel *s, u32 off, u32 *value)
{
	u64 left;
	int error = checkpoint(s, &left);

	if (error)
		return error;
	error = checked(s->ops->read(s->ctx, &s->identity, off, value, left));
	if (error)
		return blocked(s, error);
	return checkpoint(s, &left);
}

static int write_reg(struct ppn_cdma_cancel *s, u32 off, u32 value)
{
	u64 left;
	int error = checkpoint(s, &left);

	if (error)
		return error;
	error = checked(s->ops->write(s->ctx, &s->identity, off, value, left));
	if (error)
		return blocked(s, error);
	return checkpoint(s, &left);
}

static int pause_poll(struct ppn_cdma_cancel *s)
{
	u64 before, left;
	int error = checkpoint(s, &left);

	if (error)
		return error;
	before = s->last_us;
	error = checked(s->ops->delay_us(s->ctx, 1, left));
	if (error)
		return blocked(s, error);
	error = checkpoint(s, &left);
	if (error)
		return error;
	if (s->last_us <= before)
		return blocked(s, -EIO);
	return 0;
}

int ppn_cdma_cancel_run(struct ppn_cdma_cancel *s,
			const struct ppn_cdma_identity *key, u64 timeout_us)
{
	u32 active, control, bit, off;
	u64 left;
	int error = valid_request(s, key);

	if (error)
		return error;
	if (s->phase != PPN_CDMA_CANCEL_READY)
		return -EALREADY;
	if (!timeout_us || timeout_us > PPN_CDMA_CANCEL_MAX_US)
		return -EINVAL;
	s->phase = PPN_CDMA_CANCEL_RUNNING;
	s->timeout_us = timeout_us;
	s->start_us = s->ops->now_us(s->ctx);
	s->last_us = s->start_us;
	bit = 1u << s->identity.channel;
	off = s->identity.channel << 12;
	error = read_reg(s, 16, &active);
	if (error)
		return error;
	if (!(active & bit)) {
		error = write_reg(s, 0, bit);
		if (error)
			return error;
		error = read_reg(s, 16, &active);
		if (error)
			return error;
		if (!(active & bit))
			return blocked(s, -EIO);
	}
	error = read_reg(s, off, &control);
	if (error)
		return error;
	s->last_control = control;
	if ((control & PPN_CDMA_CANCEL_BUSY_MASK) == PPN_CDMA_CANCEL_BUSY) {
		error = write_reg(s, off, 2);
		if (error)
			return error;
		for (;;) {
			error = read_reg(s, off, &control);
			if (error)
				return error;
			s->last_control = control;
			if ((control & PPN_CDMA_CANCEL_BUSY_MASK) != PPN_CDMA_CANCEL_BUSY)
				break;
			/* Independent budget even if an invalid callback freezes time. */
			if (++s->polls > timeout_us)
				return blocked(s, -ETIMEDOUT);
			error = pause_poll(s);
			if (error)
				return error;
		}
	}
	error = read_reg(s, 16, &active);
	if (error)
		return error;
	if (active & bit) {
		error = write_reg(s, 8, bit);
		if (error)
			return error;
	}
	error = read_reg(s, 16, &active);
	if (error)
		return error;
	if (active & bit)
		return blocked(s, -EIO);
	error = checkpoint(s, &left);
	if (error)
		return error;
	s->phase = PPN_CDMA_CANCEL_WAIT_FENCE;
	return -EINPROGRESS;
}

int ppn_cdma_cancel_wait(const struct ppn_cdma_cancel *s,
			 const struct ppn_cdma_identity *key)
{
	int error = valid_request(s, key);

	if (error)
		return error;
	if (s->phase == PPN_CDMA_CANCEL_HOLD)
		return s->error ? s->error : -EIO;
	return -EINPROGRESS;
}
