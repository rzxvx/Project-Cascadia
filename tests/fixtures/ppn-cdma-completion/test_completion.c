// SPDX-License-Identifier: GPL-2.0-only
/* Actual production-candidate calls in regular process memory only. */
#include "ppn_cdma_completion.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "oracle.h"

static unsigned int cases;
#define CHECK(v) do { assert(v); cases++; } while (0)
static struct ppn_cdma_completion s, before;
static struct ppn_cdma_identity key;
static struct ppn_cdma_sg sg = { 0x40000000, 16384 };
static struct ppn_cdma_request request = {0, 0, 1, 0, 0, 0x50000000, 96, 16384, 4, 8, 1, &sg};
static struct ppn_cdma_reservation reservation;

static void fresh(unsigned int channel)
{
	memset(&s, 0, sizeof(s));
	request.bus = (channel - 5) / 2;
	request.metadata = (channel - 5) % 2;
	key = (struct ppn_cdma_identity){ 11, 22, 33, 44, channel };
	reservation = (struct ppn_cdma_reservation){ key, true, true, true };
	CHECK(ppn_cdma_completion_begin(&s, &request, &reservation) == 0);
}
static struct ppn_cdma_action observe(u32 control, u32 pending, u32 remaining, int expected)
{
	struct ppn_cdma_observation o = { key, 55, control, pending, remaining, true };
	struct ppn_cdma_action action;

	memset(&action, 0xa5, sizeof(action));
	CHECK(ppn_cdma_completion_observe(&s, &key, &o, &action) == expected);
	return action;
}
static struct ppn_cdma_action ready(void)
{
	return observe(PPN_CDMA_IRQ_DONE, 0, 0, 0);
}
static void applied(struct ppn_cdma_action a)
{
	struct ppn_cdma_action_receipt r = { a, a.mask, 0, 0, true };

	CHECK(ppn_cdma_completion_actions(&s, &key, &r) == 0);
	CHECK(ppn_cdma_completion_wait(&s, &key) == -EAGAIN);
}
static void fence_ok(void)
{
	struct ppn_cdma_fence_receipt r = { key, 55, PPN_CDMA_FENCE_ALL, 0 };

	CHECK(ppn_cdma_completion_fence(&s, &key, &r) == 0);
	CHECK(ppn_cdma_completion_wait(&s, &key) == 0);
}
static void original_cases(void)
{
	for (size_t i = 0; i < sizeof(oracles) / sizeof(oracles[0]); i++) {
		const struct oracle *o = &oracles[i];
		struct ppn_cdma_action a;
		struct ppn_cdma_action_receipt r;

		fresh(o->channel);
		a = observe(o->control, o->pending, o->remaining, o->result);
		CHECK(a.mask == o->mask);
		if (!a.mask) {
			CHECK(ppn_cdma_completion_wait(&s, &key) != 0);
			continue;
		}
		CHECK(a.ack_offset == o->channel * 4096 && a.ack_value == 0x80000);
		CHECK(a.disable_value == (o->finish ? 1u << o->channel : 0));
		CHECK(a.disable_offset == (o->finish ? 8u : 0));
		CHECK(ppn_cdma_completion_wait(&s, &key) == -EAGAIN);
		r = (struct ppn_cdma_action_receipt){ a, a.mask, 0, 0, true };
		CHECK(ppn_cdma_completion_actions(&s, &key, &r) == o->post_error);
		CHECK(ppn_cdma_completion_wait(&s, &key) != 0);
		if (o->finish)
			fence_ok();
	}
}
static void adversarial(void)
{
	struct ppn_cdma_action a;
	struct ppn_cdma_action_receipt r;
	struct ppn_cdma_fence_receipt f;
	struct ppn_cdma_observation o;
	struct ppn_cdma_identity other;
	struct ppn_cdma_plan p;

	memset(&s, 0, sizeof(s));
	CHECK(ppn_cdma_completion_wait(&s, &key) == -EINVAL);
	CHECK(ppn_cdma_completion_begin(NULL, &request, &reservation) == -EINVAL);
	for (unsigned int flags = 0; flags < 7; flags++) {
		memset(&s, 0, sizeof(s));
		reservation = (struct ppn_cdma_reservation){key, flags & 1, flags & 2, flags & 4};
		CHECK(ppn_cdma_completion_begin(&s, &request, &reservation) == -EPERM);
		CHECK(s.phase == PPN_CDMA_UNINITIALIZED);
	}
	fresh(5);
	before = s;
	CHECK(ppn_cdma_completion_begin(&s, &request, &reservation) == -EALREADY);
	CHECK(!memcmp(&s, &before, sizeof(s)));
	CHECK(ppn_cdma_build_plan(&request, &p) == 0);
	CHECK(!memcmp(&p, &s.plan, sizeof(p)));
	for (unsigned int field = 0; field < 5; field++) {
		fresh(5); before = s; other = key;
		if (field == 0) other.controller++;
		if (field == 1) other.epoch++;
		if (field == 2) other.transfer++;
		if (field == 3) other.lease_cookie++;
		if (field == 4) other.channel++;
		o = (struct ppn_cdma_observation){ key, 55, 0x80000, 0, 0, true };
		CHECK(ppn_cdma_completion_observe(&s, &other, &o, &a) == -EINVAL);
		CHECK(!memcmp(&s, &before, sizeof(s)));
		o.identity = other;
		CHECK(ppn_cdma_completion_observe(&s, &key, &o, &a) == -ESTALE);
		CHECK(s.phase == PPN_CDMA_HOLD && a.mask == 0);
		CHECK(s.identity.transfer == key.transfer && s.identity.lease_cookie == key.lease_cookie);
		CHECK(ppn_cdma_completion_wait(&s, &key) == -ESTALE);
		CHECK(ppn_cdma_completion_observe(&s, &key, &o, &a) == -EALREADY);
	}
	fresh(5);
	o = (struct ppn_cdma_observation){ key, 55, 0x80000, 0, 0, false };
	CHECK(ppn_cdma_completion_observe(&s, &key, &o, &a) == -ESTALE);
	for (unsigned int phase = 0; phase < 4; phase++) {
		fresh(5);
		if (phase) a = ready();
		if (phase > 1) applied(a);
		if (phase > 2) fence_ok();
		before = s;
		CHECK(ppn_cdma_completion_begin(&s, &request, &reservation) == -EALREADY);
		CHECK(!memcmp(&s, &before, sizeof(s)));
	}
	/* Zero observation has no authority; neither wait nor an early fence works. */
	fresh(5);
	o = (struct ppn_cdma_observation){ key, 0, 0x80000, 0, 0, true };
	before = s;
	CHECK(ppn_cdma_completion_observe(&s, &key, &o, &a) == -EINVAL);
	CHECK(!memcmp(&s, &before, sizeof(s)));
	f = (struct ppn_cdma_fence_receipt){ key, 55, 15, 0 };
	CHECK(ppn_cdma_completion_fence(&s, &key, &f) == -EALREADY);
	/* Each planned action field is checked, including exact W1C, no all-mask ACK. */
	for (unsigned int field = 0; field < 11; field++) {
		fresh(5); a = ready();
		r = (struct ppn_cdma_action_receipt){ a, a.mask, 0, 0, true };
		if (field == 0) r.action.identity.controller++;
		if (field == 1) r.action.identity.epoch++;
		if (field == 2) r.action.identity.transfer++;
		if (field == 3) r.action.identity.lease_cookie++;
		if (field == 4) r.action.identity.channel++;
		if (field == 5) r.action.observation++;
		if (field == 6) r.action.mask++;
		if (field == 7) r.action.ack_offset++;
		if (field == 8) r.action.ack_value = 0xffffffff;
		if (field == 9) r.action.disable_offset++;
		if (field == 10) r.action.disable_value = 0xffffffff;
		CHECK(ppn_cdma_completion_actions(&s, &key, &r) == -ESTALE);
		CHECK(ppn_cdma_completion_wait(&s, &key) == -ESTALE);
		CHECK(ppn_cdma_completion_actions(&s, &key, &r) == -EALREADY);
	}
	fresh(5); a = ready();
	r = (struct ppn_cdma_action_receipt){ a, a.mask, 0, 0, false };
	CHECK(ppn_cdma_completion_actions(&s, &key, &r) == -ESTALE);
	CHECK(ppn_cdma_completion_wait(&s, &key) == -ESTALE);
	for (unsigned int attempt = 0; attempt < 8; attempt++) {
		if (attempt == 3) continue;
		fresh(5); a = ready();
		r = (struct ppn_cdma_action_receipt){ a, attempt, 0, 0, true };
		CHECK(ppn_cdma_completion_actions(&s, &key, &r) == -EPROTO);
		CHECK(ppn_cdma_completion_wait(&s, &key) == -EPROTO);
	}
	for (unsigned int stage = 0; stage < 4; stage++) {
		fresh(5); a = ready();
		r = (struct ppn_cdma_action_receipt){ a, stage ? 3 : 1, stage ? 0 : -ETIMEDOUT, stage ? -EIO : 0, true };
		if (stage == 2) r.ack_error = 1;
		if (stage == 3) r.disable_error = 1;
		CHECK(ppn_cdma_completion_actions(&s, &key, &r) == (stage == 0 ? -ETIMEDOUT : stage == 1 ? -EIO : -EPROTO));
		CHECK(ppn_cdma_completion_wait(&s, &key) != 0);
	}
	for (unsigned int flags = 0; flags < 15; flags++) {
		fresh(5); applied(ready());
		f = (struct ppn_cdma_fence_receipt){ key, 55, flags, 0 };
		before = s;
		CHECK(ppn_cdma_completion_fence(&s, &key, &f) == -EAGAIN);
		CHECK(!memcmp(&s, &before, sizeof(s)));
		f.flags = 15;
		CHECK(ppn_cdma_completion_fence(&s, &key, &f) == 0);
		CHECK(ppn_cdma_completion_fence(&s, &key, &f) == -EALREADY);
	}
	for (unsigned int field = 0; field < 9; field++) {
		fresh(5); applied(ready());
		f = (struct ppn_cdma_fence_receipt){ key, 55, 15, 0 };
		if (field == 0) f.identity.controller++;
		if (field == 1) f.identity.epoch++;
		if (field == 2) f.identity.transfer++;
		if (field == 3) f.identity.lease_cookie++;
		if (field == 4) f.identity.channel++;
		if (field == 5) f.observation++;
		if (field == 6) f.flags |= 16;
		if (field == 7) f.error = 1;
		if (field == 8) f.error = -ETIMEDOUT;
		CHECK(ppn_cdma_completion_fence(&s, &key, &f) == (field < 6 ? -ESTALE : field < 8 ? -EPROTO : -ETIMEDOUT));
		CHECK(ppn_cdma_completion_wait(&s, &key) != 0);
	}
	/* Channel-only completion must not cross-confirm a concurrent data/OOB lease. */
	fresh(5); a = ready();
	fresh(6); (void)ready();
	r = (struct ppn_cdma_action_receipt){ a, 3, 0, 0, true };
	CHECK(ppn_cdma_completion_actions(&s, &key, &r) == -ESTALE);
	/* Maximal unsigned values cannot wrap into zero progress/success. */
	for (unsigned int field = 0; field < 2; field++) {
		fresh(5); a = observe(0x80000, field ? 0 : 0xffffffff, field ? 0xffffffff : 0, 0);
		r = (struct ppn_cdma_action_receipt){ a, a.mask, 0, 0, true };
		CHECK(a.mask == 1);
		CHECK(ppn_cdma_completion_actions(&s, &key, &r) == (field ? -EPROTO : -EOPNOTSUPP));
		CHECK(ppn_cdma_completion_wait(&s, &key) != 0);
	}
}
int main(void)
{
	original_cases();
	adversarial();
	printf("PASS %u actual C assertions, %zu original IRQ comparisons\n", cases, sizeof(oracles)/sizeof(oracles[0]));
	return 0;
}
