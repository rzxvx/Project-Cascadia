// SPDX-License-Identifier: GPL-2.0-only
/* Strict regular-memory callbacks, no system/device/IRQ/IO API. */
#include "ppn_cdma_cancel.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "cancel_oracle.h"

struct event { u32 offset, value; };
struct fake {
	struct ppn_cdma_identity key;
	u32 active, control, clear_after, abort_reads, polls;
	u64 clock, timeout;
	bool aborting, frozen, reject_enable, reject_disable, rollback;
	unsigned int calls, fail_at, epoch_at, late_at, writes;
	int injected;
	struct event trace[8];
};
static unsigned int assertions;
#define CHECK(v) do { assert(v); assertions++; } while (0)
static struct ppn_cdma_cancel s, before;
static struct fake f;
static struct ppn_cdma_identity key;
static int fault(struct fake *f)
{
	f->calls++;
	if (f->calls == f->epoch_at)
		f->key.epoch++;
	if (f->calls == f->late_at)
		f->clock += f->timeout;
	return f->calls == f->fail_at ? f->injected : 0;
}
static int identity(struct fake *f, const struct ppn_cdma_identity *k)
{
	return f->key.controller == k->controller && f->key.epoch == k->epoch &&
		f->key.transfer == k->transfer && f->key.lease_cookie == k->lease_cookie &&
		f->key.channel == k->channel ? 0 : -ESTALE;
}
static int validate(void *ctx, const struct ppn_cdma_identity *k)
{
	struct fake *f = ctx;
	int error = fault(f);

	return error ? error : identity(f, k);
}
static int read_fake(void *ctx, const struct ppn_cdma_identity *k,
		     u32 off, u32 *out, u64 left)
{
	struct fake *f = ctx;
	int error = fault(f);

	CHECK(left > 0 && (off == 16 || off == k->channel << 12));
	if (error)
		return error;
	error = identity(f, k);
	if (error)
		return error;
	if (off == 16)
		*out = f->active;
	else {
		if (f->aborting && ++f->abort_reads > f->clear_after)
			f->control = 0;
		*out = f->control;
		f->polls++;
	}
	if (f->rollback)
		f->clock--;
	return 0;
}
static int write_fake(void *ctx, const struct ppn_cdma_identity *k,
		      u32 off, u32 value, u64 left)
{
	struct fake *f = ctx;
	int error = fault(f);

	CHECK(left > 0 && ((off == 0 || off == 8) ? value == 1u << k->channel :
		off == k->channel << 12 && value == 2));
	if (error)
		return error;
	error = identity(f, k);
	if (error)
		return error;
	CHECK(f->writes < 8);
	f->trace[f->writes++] = (struct event){off, value};
	if (off == 0) {
		if (!f->reject_enable)
			f->active |= value;
	} else if (off == 8) {
		if (!f->reject_disable)
			f->active &= ~value;
	} else
		f->aborting = true;
	return 0;
}
static u64 now_fake(void *ctx)
{
	return ((struct fake *)ctx)->clock;
}
static int delay_fake(void *ctx, unsigned int us, u64 left)
{
	struct fake *f = ctx;
	int error = fault(f);

	CHECK(us == 1 && left > 0);
	if (error)
		return error;
	if (!f->frozen)
		f->clock++;
	return 0;
}
static const struct ppn_cdma_cancel_ops ops = {validate, read_fake, write_fake, now_fake, delay_fake};
static void fresh(u32 channel, bool enabled, u32 control, u32 clear_after)
{
	struct ppn_cdma_cancel_adoption a;

	memset(&s, 0, sizeof(s));
	memset(&f, 0, sizeof(f));
	key = (struct ppn_cdma_identity){1, 2, 3, 4, channel};
	f.key = key;
	f.active = 0x40000000 | (enabled ? 1u << channel : 0);
	f.control = control;
	f.clear_after = clear_after;
	f.timeout = 10000;
	f.clock = 0xfffffff0;
	a = (struct ppn_cdma_cancel_adoption){key, true, true, true};
	CHECK(ppn_cdma_cancel_begin(&s, &a, &ops, &f) == 0);
}
static void pending_and_no_retry(void)
{
	unsigned int calls = f.calls;

	CHECK(s.phase == PPN_CDMA_CANCEL_WAIT_FENCE);
	CHECK(ppn_cdma_cancel_wait(&s, &key) == -EINPROGRESS);
	CHECK(ppn_cdma_cancel_run(&s, &key, 10000) == -EALREADY);
	CHECK(calls == f.calls);
	/* A late DONE event cannot become a callback, ACK or release through this API. */
	f.control = 0x80000;
	CHECK(ppn_cdma_cancel_wait(&s, &key) == -EINPROGRESS);
	CHECK(calls == f.calls);
}
static void original_control_cases(void)
{
	for (size_t i = 0; i < sizeof(cancel_oracles) / sizeof(cancel_oracles[0]); i++) {
		const struct cancel_oracle *o = &cancel_oracles[i];
		unsigned int n = 0;

		fresh(o->channel, o->enabled, o->control, o->clear_after);
		CHECK(ppn_cdma_cancel_run(&s, &key, f.timeout) == -EINPROGRESS);
		if (!o->enabled) {
			CHECK(f.trace[n].offset == 0 && f.trace[n].value == 1u << o->channel);
			n++;
		}
		if ((o->control & 0x30000) == 0x10000) {
			CHECK(f.trace[n].offset == o->channel << 12 && f.trace[n].value == 2);
			n++;
		}
		CHECK(f.trace[n].offset == 8 && f.trace[n].value == 1u << o->channel);
		n++;
		CHECK(f.writes == n && f.active == 0x40000000);
		pending_and_no_retry();
	}
}
static void fault_cases(void)
{
	unsigned int maximum;

	fresh(5, false, 0x10000, 2);
	CHECK(ppn_cdma_cancel_run(&s, &key, f.timeout) == -EINPROGRESS);
	maximum = f.calls;
	for (unsigned int at = 1; at <= maximum; at++) {
		for (unsigned int kind = 0; kind < 3; kind++) {
			fresh(5, false, 0x10000, 2);
			f.fail_at = at;
			f.injected = kind == 0 ? -ETIMEDOUT : kind == 1 ? -ENOMEM : 1;
			CHECK(ppn_cdma_cancel_run(&s, &key, f.timeout) == (kind < 2 ? f.injected : -EPROTO));
			CHECK(s.phase == PPN_CDMA_CANCEL_HOLD);
			CHECK(ppn_cdma_cancel_wait(&s, &key) != 0);
			CHECK(s.identity.transfer == key.transfer && s.identity.lease_cookie == key.lease_cookie);
			before = s;
			unsigned int calls = f.calls;
			CHECK(ppn_cdma_cancel_run(&s, &key, f.timeout) == -EALREADY);
			CHECK(!memcmp(&s, &before, sizeof(s)) && calls == f.calls);
		}
		fresh(5, false, 0x10000, 2);
		f.epoch_at = at;
		CHECK(ppn_cdma_cancel_run(&s, &key, f.timeout) == -ESTALE);
		CHECK(s.phase == PPN_CDMA_CANCEL_HOLD && s.identity.epoch == key.epoch);
		CHECK(ppn_cdma_cancel_wait(&s, &key) == -ESTALE);
		fresh(5, false, 0x10000, 2);
		f.late_at = at;
		CHECK(ppn_cdma_cancel_run(&s, &key, f.timeout) == -ETIMEDOUT);
		CHECK(s.phase == PPN_CDMA_CANCEL_HOLD);
		CHECK(ppn_cdma_cancel_wait(&s, &key) == -ETIMEDOUT);
	}
	fresh(5, false, 0x10000, 0xffffffff);
	f.timeout = 8;
	CHECK(ppn_cdma_cancel_run(&s, &key, f.timeout) == -ETIMEDOUT);
	CHECK(s.phase == PPN_CDMA_CANCEL_HOLD && f.active & (1u << 5));
	CHECK(f.writes == 2 && f.polls < 20);
	fresh(5, false, 0x10000, 0xffffffff);
	f.frozen = true;
	CHECK(ppn_cdma_cancel_run(&s, &key, f.timeout) == -EIO);
	CHECK(s.phase == PPN_CDMA_CANCEL_HOLD && f.polls < 4);
	fresh(5, false, 0x10000, 2);
	f.rollback = true;
	CHECK(ppn_cdma_cancel_run(&s, &key, f.timeout) == -EIO);
	CHECK(f.writes == 0 && s.phase == PPN_CDMA_CANCEL_HOLD);
	for (unsigned int stage = 0; stage < 2; stage++) {
		fresh(5, false, 0x10000, 0);
		f.reject_enable = stage == 0;
		f.reject_disable = stage == 1;
		CHECK(ppn_cdma_cancel_run(&s, &key, f.timeout) == -EIO);
		CHECK(s.phase == PPN_CDMA_CANCEL_HOLD);
	}
}
static void input_cases(void)
{
	struct ppn_cdma_identity foreign;
	struct ppn_cdma_cancel_adoption a;
	struct ppn_cdma_cancel_ops incomplete;

	for (unsigned int flags = 0; flags < 7; flags++) {
		memset(&s, 0, sizeof(s));
		a = (struct ppn_cdma_cancel_adoption){key, flags & 1, flags & 2, flags & 4};
		CHECK(ppn_cdma_cancel_begin(&s, &a, &ops, &f) == -EPERM);
		CHECK(s.phase == PPN_CDMA_CANCEL_UNINITIALIZED);
	}
	fresh(5, false, 0, 0);
	for (unsigned int field = 0; field < 5; field++) {
		foreign = key; before = s;
		if (field == 0) foreign.controller++;
		if (field == 1) foreign.epoch++;
		if (field == 2) foreign.transfer++;
		if (field == 3) foreign.lease_cookie++;
		if (field == 4) foreign.channel++;
		CHECK(ppn_cdma_cancel_run(&s, &foreign, 10000) == -EINVAL);
		CHECK(!memcmp(&s, &before, sizeof(s)) && f.calls == 0);
	}
	for (unsigned int field = 0; field < 6; field++) {
		memset(&s, 0, sizeof(s));
		foreign = key;
		if (field == 0) foreign.controller = 0;
		if (field == 1) foreign.epoch = 0;
		if (field == 2) foreign.transfer = 0;
		if (field == 3) foreign.lease_cookie = 0;
		if (field == 4) foreign.channel = 4;
		if (field == 5) foreign.channel = 9;
		a = (struct ppn_cdma_cancel_adoption){foreign, true, true, true};
		CHECK(ppn_cdma_cancel_begin(&s, &a, &ops, &f) == -EINVAL);
		CHECK(s.phase == PPN_CDMA_CANCEL_UNINITIALIZED);
	}
	fresh(5, false, 0, 0); before = s;
	CHECK(ppn_cdma_cancel_run(&s, &key, 0) == -EINVAL);
	CHECK(!memcmp(&s, &before, sizeof(s)) && f.calls == 0);
	CHECK(ppn_cdma_cancel_run(&s, &key, 2000001) == -EINVAL);
	CHECK(!memcmp(&s, &before, sizeof(s)) && f.calls == 0);
	for (unsigned int stage = 0; stage < 3; stage++) {
		fresh(5, false, 0x10000, stage == 2 ? 0xffffffff : 0);
		if (stage) (void)ppn_cdma_cancel_run(&s, &key, stage == 2 ? 2 : 10000);
		before = s;
		a = (struct ppn_cdma_cancel_adoption){key, true, true, true};
		CHECK(ppn_cdma_cancel_begin(&s, &a, &ops, &f) == -EALREADY);
		CHECK(!memcmp(&s, &before, sizeof(s)));
	}
	CHECK(ppn_cdma_cancel_begin(NULL, &a, &ops, &f) == -EINVAL);
	CHECK(ppn_cdma_cancel_run(NULL, &key, 1) == -EINVAL);
	CHECK(ppn_cdma_cancel_wait(NULL, &key) == -EINVAL);
	for (unsigned int field = 0; field < 5; field++) {
		memset(&s, 0, sizeof(s));
		incomplete = ops;
		if (field == 0) incomplete.validate_lease = NULL;
		if (field == 1) incomplete.read = NULL;
		if (field == 2) incomplete.write = NULL;
		if (field == 3) incomplete.now_us = NULL;
		if (field == 4) incomplete.delay_us = NULL;
		CHECK(ppn_cdma_cancel_begin(&s, &a, &incomplete, &f) == -EINVAL);
		CHECK(s.phase == PPN_CDMA_CANCEL_UNINITIALIZED);
	}
	memset(&s, 0, sizeof(s));
	s.last_us = 1;
	before = s;
	CHECK(ppn_cdma_cancel_begin(&s, &a, &ops, &f) == -EALREADY);
	CHECK(!memcmp(&s, &before, sizeof(s)));
}
static void boundary_cases(void)
{
	struct ppn_cdma_cancel_adoption a;
	struct ppn_cdma_identity foreign;

	fresh(8, false, 0x90000, 1);
	key.controller = 0xffffffff00000001ULL;
	key.epoch = 0xfffffffe00000002ULL;
	key.transfer = 0xfffffffd00000003ULL;
	key.lease_cookie = 0xfffffffc00000004ULL;
	f.key = key;
	memset(&s, 0, sizeof(s));
	a = (struct ppn_cdma_cancel_adoption){key, true, true, true};
	CHECK(ppn_cdma_cancel_begin(&s, &a, &ops, &f) == 0);
	foreign = key;
	foreign.epoch &= 0xffffffffu;
	before = s;
	CHECK(ppn_cdma_cancel_run(&s, &foreign, 10000) == -EINVAL);
	CHECK(!memcmp(&s, &before, sizeof(s)) && !f.calls);
	CHECK(ppn_cdma_cancel_run(&s, &key, 10000) == -EINPROGRESS);
	CHECK(f.writes == 3 && f.trace[1].offset == 0x8000 && f.trace[1].value == 2);
	CHECK(s.identity.epoch == key.epoch && s.identity.lease_cookie == key.lease_cookie);
	pending_and_no_retry();
	/* A u64 monotonic clock wrap must poison, never look like positive completion. */
	fresh(5, false, 0x10000, 2);
	f.clock = ~0ULL;
	CHECK(ppn_cdma_cancel_run(&s, &key, 10000) == -EIO);
	CHECK(s.phase == PPN_CDMA_CANCEL_HOLD && f.writes == 2);
	CHECK(ppn_cdma_cancel_wait(&s, &key) == -EIO);
}
int main(void)
{
	original_control_cases();
	fault_cases();
	input_cases();
	boundary_cases();
	printf("PASS %u actual C assertions, %zu successful original cancellation trace comparisons\n", assertions, sizeof(cancel_oracles) / sizeof(cancel_oracles[0]));
	return 0;
}
