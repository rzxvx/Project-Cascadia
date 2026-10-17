// SPDX-License-Identifier: GPL-2.0-only
#include "ppn_cdma_completion.h"
#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#else
#include <errno.h>
#include <string.h>
#endif

static bool same_identity(const struct ppn_cdma_identity *a,
			  const struct ppn_cdma_identity *b)
{
	return a->controller == b->controller && a->epoch == b->epoch &&
		a->transfer == b->transfer && a->lease_cookie == b->lease_cookie &&
		a->channel == b->channel;
}

static bool valid_identity(const struct ppn_cdma_identity *i)
{
	return i->controller && i->epoch && i->transfer && i->lease_cookie &&
		i->channel >= 5 && i->channel <= 8;
}

static int validate(const struct ppn_cdma_completion *s,
		    const struct ppn_cdma_identity *key)
{
	if (!s || !key || s->phase == PPN_CDMA_UNINITIALIZED ||
	    !same_identity(&s->identity, key))
		return -EINVAL;
	return 0;
}

static int hold(struct ppn_cdma_completion *s, int error)
{
	s->phase = PPN_CDMA_HOLD;
	s->terminal_error = error;
	return error;
}

int ppn_cdma_completion_begin(struct ppn_cdma_completion *s,
			      const struct ppn_cdma_request *r,
			      const struct ppn_cdma_reservation *owned)
{
	const unsigned char *bytes;
	size_t i;
	int error;

	if (!s || !r || !owned)
		return -EINVAL;
	bytes = (const unsigned char *)s;
	for (i = 0; i < sizeof(*s); i++)
		if (bytes[i])
			return -EALREADY;
	if (!valid_identity(&owned->identity) || !owned->exclusive ||
	    !owned->handoff_quiesced || !owned->prior_irq_drained)
		return -EPERM;
	if (r->bus > 1 || r->metadata > 1 ||
	    owned->identity.channel != 5 + r->bus * 2 + r->metadata)
		return -EINVAL;
	error = ppn_cdma_build_plan(r, &s->plan);
	if (error)
		return error;
	s->identity = owned->identity;
	s->phase = PPN_CDMA_ACTIVE;
	return 0;
}

int ppn_cdma_completion_observe(struct ppn_cdma_completion *s,
				const struct ppn_cdma_identity *key,
				const struct ppn_cdma_observation *o,
				struct ppn_cdma_action *a)
{
	int error = validate(s, key);

	if (error || !o || !a)
		return error ? error : -EINVAL;
	memset(a, 0, sizeof(*a));
	if (s->phase != PPN_CDMA_ACTIVE)
		return -EALREADY;
	if (!same_identity(&s->identity, &o->identity) || !o->exclusive_current)
		return hold(s, -ESTALE);
	if (!o->observation)
		return -EINVAL;
	if (o->control & PPN_CDMA_IRQ_ERROR)
		return hold(s, -EIO);
	if ((o->control & PPN_CDMA_IRQ_SPURIOUS) ||
	    !(o->control & PPN_CDMA_IRQ_DONE))
		return -EAGAIN;
	/* Qualified complete-batch leaf only; unknown non-event CONTROL bits are
	 * retained raw in the observation and not invented as failure meanings.
	 */
	s->action.identity = s->identity;
	s->action.observation = o->observation;
	s->action.mask = PPN_CDMA_ACTION_ACK;
	s->action.ack_offset = s->identity.channel << 12;
	s->action.ack_value = PPN_CDMA_IRQ_DONE;
	if (o->pending)
		s->terminal_error = -EOPNOTSUPP;
	else if (o->remaining)
		s->terminal_error = -EPROTO;
	else {
		s->action.mask |= PPN_CDMA_ACTION_DISABLE;
		s->action.disable_offset = 8;
		s->action.disable_value = 1u << s->identity.channel;
	}
	s->phase = PPN_CDMA_WAIT_ACTION;
	*a = s->action;
	return 0;
}

static bool same_action(const struct ppn_cdma_action *a,
			const struct ppn_cdma_action *b)
{
	return same_identity(&a->identity, &b->identity) &&
		a->observation == b->observation && a->mask == b->mask &&
		a->ack_offset == b->ack_offset && a->ack_value == b->ack_value &&
		a->disable_offset == b->disable_offset &&
		a->disable_value == b->disable_value;
}

int ppn_cdma_completion_actions(struct ppn_cdma_completion *s,
				const struct ppn_cdma_identity *key,
				const struct ppn_cdma_action_receipt *r)
{
	int error = validate(s, key);

	if (error || !r)
		return error ? error : -EINVAL;
	if (s->phase != PPN_CDMA_WAIT_ACTION)
		return -EALREADY;
	if (!same_action(&s->action, &r->action) || !r->exclusive_current)
		return hold(s, -ESTALE);
	if ((r->attempted & ~s->action.mask) ||
	    !(r->attempted & PPN_CDMA_ACTION_ACK) || r->ack_error > 0 ||
	    r->disable_error > 0 ||
	    (!(s->action.mask & PPN_CDMA_ACTION_DISABLE) && r->disable_error))
		return hold(s, -EPROTO);
	if (r->ack_error) {
		if (r->attempted != PPN_CDMA_ACTION_ACK || r->disable_error)
			return hold(s, -EPROTO);
		return hold(s, r->ack_error);
	}
	if (r->attempted != s->action.mask)
		return hold(s, -EPROTO);
	if (r->disable_error)
		return hold(s, r->disable_error);
	if (s->terminal_error)
		return hold(s, s->terminal_error);
	s->phase = PPN_CDMA_WAIT_FENCE;
	return 0;
}

int ppn_cdma_completion_fence(struct ppn_cdma_completion *s,
			      const struct ppn_cdma_identity *key,
			      const struct ppn_cdma_fence_receipt *r)
{
	int error = validate(s, key);

	if (error || !r)
		return error ? error : -EINVAL;
	if (s->phase != PPN_CDMA_WAIT_FENCE)
		return -EALREADY;
	if (!same_identity(&s->identity, &r->identity) ||
	    s->action.observation != r->observation)
		return hold(s, -ESTALE);
	if (r->flags & ~PPN_CDMA_FENCE_ALL || r->error > 0)
		return hold(s, -EPROTO);
	if (r->error)
		return hold(s, r->error);
	if (r->flags != PPN_CDMA_FENCE_ALL)
		return -EAGAIN;
	s->phase = PPN_CDMA_FINISHED;
	return 0;
}

int ppn_cdma_completion_wait(const struct ppn_cdma_completion *s,
			     const struct ppn_cdma_identity *key)
{
	int error = validate(s, key);

	if (error)
		return error;
	if (s->phase == PPN_CDMA_FINISHED)
		return 0;
	if (s->phase == PPN_CDMA_HOLD)
		return s->terminal_error ? s->terminal_error : -EIO;
	return -EAGAIN;
}
