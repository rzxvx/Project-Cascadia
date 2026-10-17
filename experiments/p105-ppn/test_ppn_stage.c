// SPDX-License-Identifier: GPL-2.0-only
/* Strict process-local fakes; no fd/device/MMIO/ioctl/network APIs. */
#include "ppn_stage_engine.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
struct lease {
	unsigned int channel;
	bool owned, active;
};
struct fake {
	u32 ctl, ce, event, status;
	u64 clock;
	unsigned int calls, fail_at, wait_fail, start_partial, start_null, cancel_fail, full_fifo;
	int injected;
	bool no_event, wrong_ce, rollback, trace, frozen;
	u64 late_end, late_command, late_wait, late_cancel;
	u32 list_count, list_row;
	unsigned int feature_count, end_count, cancel_count, release_count;
	struct lease leases[2];
};
static struct ppn_stage_geometry geometry = {0, 1, 2, 8, 11, 4, 3, 1064, 128, 0, 1064, 24000};
static struct ppn_stage_address address = {0, 0, 0, 0, true};
static u8 data[PPN_STAGE_DATA], meta[PPN_STAGE_META];
static int fault(struct fake *f)
{
	return ++f->calls == f->fail_at ? (f->injected ? f->injected : -EIO) : 0;
}
static int rd(void *v, enum ppn_stage_reg reg, u32 *out)
{
	struct fake *f = v;
	int r = fault(f);
	if (r)
		return r;
	switch (reg) {
	case PPN_S_CONTROL:
		*out = f->ctl;
		break;
	case PPN_S_CE:
		*out = f->wrong_ce ? 2 : f->ce;
		break;
	case PPN_S_EVENT:
		*out = f->no_event ? 0 : f->event;
		break;
	case PPN_S_NAND_STATUS:
		*out = f->status;
		break;
	case PPN_S_COMMAND_DEPTH:
	case PPN_S_PARAMETER_DEPTH:
		*out = f->full_fifo ? 32 : 0;
		break;
	default:
		assert(!"unexpected register read");
	}
	return 0;
}
static int wr(void *v, enum ppn_stage_reg reg, u32 val)
{
	struct fake *f = v;
	int r = fault(f);
	if (r)
		return r;
	switch (reg) {
	case PPN_S_CONTROL:
		f->ctl = val;
		break;
	case PPN_S_CE:
		f->ce = val;
		break;
	case PPN_S_COMMAND:
		if ((val & 0xff000000) == 0x09000000 && (val & 0xff))
			f->ce = val & 0xff;
		break;
	case PPN_S_EVENT:
	case PPN_S_SEQ_TICKS:
	case PPN_S_CONFIG:
	case PPN_S_PIO:
	case PPN_S_ADDRNUM:
	case PPN_S_PARAMETER:
		break;
	default:
		assert(!"unexpected register write");
	}
	if (f->trace)
		printf("M %u %u\n", reg, val);
	return 0;
}
static int start(void *v, unsigned int ch, enum ppn_stage_fifo fifo, const void *buf, size_t len, unsigned int word, unsigned int block, void **cookie)
{
	struct fake *f = v;
	struct lease *l = &f->leases[fifo];
	int r = fault(f);
	assert(!l->owned && ch == (fifo == PPN_S_DATA_FIFO ? 5u : 6u) + 2 * geometry.bus);
	assert(buf == (fifo == PPN_S_DATA_FIFO ? (const void *)data : (const void *)meta));
	assert(len == (fifo == PPN_S_DATA_FIFO ? 16384u : 64u));
	assert(word == (fifo == PPN_S_DATA_FIFO ? 4u : 1u) && block == (fifo == PPN_S_DATA_FIFO ? 8u : 1u));
	*cookie = NULL;
	if (r)
		return r;
	if (ch == f->start_null)
		return 0;
	l->channel = ch;
	l->owned = l->active = true;
	*cookie = l;
	if (f->trace)
		printf("S %u %zu %u %u\n", ch, len, word, block);
	return ch == f->start_partial ? -ENOMEM : 0;
}
static struct lease *owned(struct fake *f, void *cookie)
{
	struct lease *l = cookie;
	assert(l == &f->leases[0] || l == &f->leases[1]);
	assert(l->owned);
	return l;
}
static int wait_dma(void *v, void *cookie, u64 left)
{
	struct fake *f = v;
	struct lease *l = owned(f, cookie);
	int r = fault(f);
	assert(left > 0 && l->active);
	if (f->trace)
		printf("W %u\n", l->channel);
	if (r)
		return r;
	f->clock += f->late_wait;
	if (l->channel == f->wait_fail)
		return -ETIMEDOUT;
	l->active = false;
	return 0;
}
static int cancel_dma(void *v, void *cookie, u64 budget)
{
	struct fake *f = v;
	struct lease *l = owned(f, cookie);
	assert(l->active);
	f->cancel_count++;
	assert(budget == PPN_STAGE_CLEANUP_TIMEOUT_US);
	f->clock += f->late_cancel;
	if (l->channel == f->cancel_fail)
		return -EIO;
	l->active = false;
	return 0;
}
static void release_dma(void *v, void *cookie)
{
	struct fake *f = v;
	struct lease *l = owned(f, cookie);
	assert(!l->active);
	l->owned = false;
	f->release_count++;
}
static int command(void *v, u8 first, u8 second, u32 row, unsigned int naddr)
{
	struct fake *f = v;
	assert(first == 0x6a && second == 0xd7 && row == 0x800000 && naddr == 3);
	f->clock += f->late_command;
	return fault(f);
}
static int phase(void *v, u8 *raw, u64 left)
{
	struct fake *f = v;
	assert(left > 0);
	int r = fault(f);
	if (!r)
		*raw = 0x40;
	return r;
}
static int entry(void *v, u8 *raw, u64 left)
{
	struct fake *f = v;
	assert(left > 0);
	int r = fault(f);
	if (!r)
		*raw = (u8)f->status;
	return r;
}
static int ending(void *v, u8 cmd, u64 left)
{
	struct fake *f = v;
	assert(cmd == 0x77 && left > 0);
	f->end_count++;
	f->clock += f->late_end;
	return fault(f);
}
static void le32(u8 *p, u32 v)
{
	p[0] = v;
	p[1] = v >> 8;
	p[2] = v >> 16;
	p[3] = v >> 24;
}
static int feature(void *v, u8 ce, unsigned int feat, u8 out[512], u64 left)
{
	struct fake *f = v;
	int r = fault(f);
	assert(ce == 0 && left > 0 && (feat == 0x3080 || feat == 0x3280 || feat == 0x3180));
	assert(!f->leases[0].owned && !f->leases[1].owned);
	f->feature_count++;
	if (r)
		return r;
	memset(out, 0, 512);
	le32(out, f->list_count);
	le32(out + 4, f->list_row);
	return 0;
}
static u64 now(void *v)
{
	struct fake *f = v;
	if (f->frozen)
		return 0;
	if (f->rollback && f->clock > 3)
		return 0;
	return f->clock++;
}
static int delay(void *v, unsigned int us)
{
	struct fake *f = v;
	assert(us == 1);
	if (!f->frozen)
		f->clock += us;
	return fault(f);
}
static const struct ppn_stage_ops ops = {rd, wr, start, wait_dma, cancel_dma, release_dma, command, phase, entry, ending, feature, now, delay};
static void fresh(struct fake *f, struct ppn_stage_engine *e)
{
	memset(f, 0, sizeof(*f));
	memset(e, 0, sizeof(*e));
	f->ce = 1;
	f->event = 1;
	f->status = 0x40;
	assert(!ppn_stage_bind(e, &ops, f, &geometry));
}
static int program(struct ppn_stage_engine *e, struct ppn_stage_result *r, u64 timeout)
{
	return ppn_stage_program(e, &address, data, sizeof(data), meta, sizeof(meta), timeout, r);
}
static void idle(struct fake *f, struct ppn_stage_engine *e)
{
	assert(!f->leases[0].owned && !f->leases[1].owned && !e->busy && !e->data_lease && !e->meta_lease && !e->loaned_data && !e->loaned_meta);
}
int main(int argc, char **argv)
{
	struct fake f;
	struct ppn_stage_engine e;
	struct ppn_stage_result r;
	unsigned int cases = 0, maximum;
	u32 row;
	int error;
	if (argc == 2 && (!strcmp(argv[1], "--trace") || !strcmp(argv[1], "--trace-bus1"))) {
		geometry.bus = !strcmp(argv[1], "--trace-bus1");
		fresh(&f, &e);
		f.trace = true;
		assert(!program(&e, &r, 1000000));
		assert(r.transport_complete && r.durability_unknown);
		idle(&f, &e);
		return 0;
	}
	assert(argc == 1);
	fresh(&f, &e);
	assert(!program(&e, &r, 1000000));
	maximum = f.calls;
	assert(r.transport_complete && r.durability_unknown && r.raw_status == 0x40);
	idle(&f, &e);
	cases++;
	for (unsigned int n = 1; n <= maximum; n++) {
		fresh(&f, &e);
		f.fail_at = n;
		assert(program(&e, &r, 1000000) == -EIO);
		assert(e.blocked && !r.transport_complete && r.durability_unknown);
		idle(&f, &e);
		unsigned int old = f.calls;
		assert(program(&e, &r, 1000000) == -EIO);
		assert(f.calls == old);
		cases++;
	}
	for (unsigned int channel = 5; channel <= 6; channel++) {
		fresh(&f, &e);
		f.wait_fail = channel;
		assert(program(&e, &r, 1000000) == -ETIMEDOUT);
		assert(e.blocked && !r.transport_complete);
		idle(&f, &e);
		cases++;
		fresh(&f, &e);
		f.start_partial = channel;
		assert(program(&e, &r, 1000000) == -ENOMEM);
		assert(e.blocked);
		idle(&f, &e);
		cases++;
		fresh(&f, &e);
		f.start_null = channel;
		assert(program(&e, &r, 1000000) == -EPROTO);
		idle(&f, &e);
		cases++;
	}
	fresh(&f, &e);
	f.wait_fail = 5;
	f.cancel_fail = 5;
	assert(program(&e, &r, 1000000) == -ETIMEDOUT);
	assert(r.unquiesced_dma && r.cleanup_error == -EIO && e.loaned_data == data && e.data_lease && e.blocked);
	assert(!e.meta_lease);
	cases++;
	fresh(&f, &e);
	f.no_event = true;
	assert(program(&e, &r, 500) == -ETIMEDOUT);
	idle(&f, &e);
	cases++;
	fresh(&f, &e);
	f.full_fifo = 1;
	assert(program(&e, &r, 500) == -ETIMEDOUT);
	idle(&f, &e);
	cases++;
	fresh(&f, &e);
	f.frozen = true;
	f.no_event = true;
	assert(program(&e, &r, 1000000) == -EIO);
	assert(f.calls < 100);
	idle(&f, &e);
	cases++;
	fresh(&f, &e);
	f.frozen = true;
	f.full_fifo = 1;
	assert(program(&e, &r, 1000000) == -EIO);
	assert(f.calls < 100);
	idle(&f, &e);
	cases++;
	fresh(&f, &e);
	f.late_wait = 1000000;
	assert(program(&e, &r, 1000000) == -ETIMEDOUT);
	idle(&f, &e);
	cases++;
	fresh(&f, &e);
	f.late_cancel = PPN_STAGE_CLEANUP_TIMEOUT_US;
	f.wait_fail = 5;
	assert(program(&e, &r, 1000000) == -ETIMEDOUT);
	assert(r.cleanup_error == -ETIMEDOUT && r.unquiesced_dma && e.data_lease && e.meta_lease);
	cases++;
	fresh(&f, &e);
	f.wrong_ce = true;
	assert(program(&e, &r, 1000000) == -ESTALE);
	idle(&f, &e);
	cases++;
	fresh(&f, &e);
	f.event = 0x80;
	assert(program(&e, &r, 1000000) == -EIO);
	idle(&f, &e);
	cases++;
	fresh(&f, &e);
	f.rollback = true;
	assert(program(&e, &r, 1000000) == -EIO);
	idle(&f, &e);
	cases++;
	fresh(&f, &e);
	f.injected = 1;
	f.fail_at = 1;
	assert(program(&e, &r, 1000000) == -EPROTO);
	idle(&f, &e);
	cases++;
	for (unsigned int n = 0; n < 5; n++) {
		fresh(&f, &e);
		f.status = 0x41;
		f.list_count = n == 4 ? 128 : n;
		f.list_row = n == 3 ? 0x800100 : 0x800000;
		error = program(&e, &r, 1000000);
		assert(error == (n == 3 ? -EPROTO : n > 1 ? -EPROTO : -EIO));
		assert(e.blocked && !r.transport_complete);
		idle(&f, &e);
		cases++;
	}
	fresh(&f, &e);
	f.status = 0x41;
	f.list_count = 1;
	f.list_row = 0x800100;
	assert(program(&e, &r, 1000000) == -ESTALE);
	idle(&f, &e);
	cases++;
	fresh(&f, &e);
	assert(!ppn_stage_erase(&e, &address, 1000000, &r));
	assert(f.end_count == 1 && r.transport_complete && r.durability_unknown);
	maximum = f.calls;
	cases++;
	for (unsigned int n = 1; n <= maximum; n++) {
		fresh(&f, &e);
		f.fail_at = n;
		assert(ppn_stage_erase(&e, &address, 1000000, &r) == -EIO);
		assert(e.blocked && !r.transport_complete);
		cases++;
	}
	fresh(&f, &e);
	f.late_end = 1000000;
	assert(ppn_stage_erase(&e, &address, 1000000, &r) == -ETIMEDOUT);
	assert(!r.transport_complete && e.blocked);
	cases++;
	fresh(&f, &e);
	f.late_command = 1000000;
	assert(ppn_stage_erase(&e, &address, 1000000, &r) == -ETIMEDOUT);
	assert(!r.transport_complete && e.blocked && f.end_count == 0);
	cases++;
	memset(&e, 0, sizeof(e));
	assert(program(&e, &r, 1000000) == -ENODEV);
	assert(ppn_stage_bind(&e, NULL, &f, &geometry) == -ENODEV);
	cases++;
	fresh(&f, &e);
	assert(ppn_stage_bind(&e, &ops, &f, &geometry) == -EALREADY);
	cases++;
	fresh(&f, &e);
	assert(program(&e, &r, 0) == -EINVAL && f.calls == 0);
	assert(program(&e, &r, 60000001) == -EINVAL && f.calls == 0);
	assert(ppn_stage_program(&e, &address, data, 16383, meta, 64, 1000, &r) == -EINVAL && f.calls == 0);
	cases++;
	for (unsigned int page = 0; page < 256; page++) {
		struct ppn_stage_address a = address;
		a.page = page;
		assert(ppn_stage_encode(&geometry, &a, &row) == (page < 128 ? 0 : -EINVAL));
		if (page < 128)
			assert(row == 0x800000 + page);
		cases++;
	}
	for (unsigned int block = 0; block < 1065; block++) {
		struct ppn_stage_address a = address;
		a.block = block;
		assert(ppn_stage_encode(&geometry, &a, &row) == (block < 1064 ? 0 : -EINVAL));
		if (block < 1064)
			assert(row == 0x800000 + block * 256);
		cases++;
	}
	for (unsigned int ce = 0; ce < 256; ce++) {
		struct ppn_stage_address a = address;
		a.physical_ce = ce;
		assert(ppn_stage_encode(&geometry, &a, &row) == (ce == 0 ? 0 : -EINVAL));
		cases++;
	}
	for (unsigned int cau = 0; cau < 256; cau++) {
		struct ppn_stage_address a = address;
		a.cau = cau;
		assert(ppn_stage_encode(&geometry, &a, &row) == (cau < 2 ? 0 : -EINVAL));
		cases++;
	}
	{
		struct ppn_stage_address a = address;
		a.slc = false;
		assert(ppn_stage_encode(&geometry, &a, &row) == -EINVAL);
		cases++;
	}
	printf("PASS: %u C engine adversarial/failure/bounds cases; all callbacks process-local, transport completion never durability.\n", cases);
	return 0;
}
