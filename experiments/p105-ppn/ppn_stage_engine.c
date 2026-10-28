// SPDX-License-Identifier: GPL-2.0-only
#include "ppn_stage_engine.h"
#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#else
#include <errno.h>
#include <string.h>
#endif
/* Derived semantic facts from hash-pinned12H321, not copied firmware routines.
 * Single-page program:93b8/9aa4..a068, erase6744, row builder80e2174c.
 * Selected sequencer constants alone do not establish microcode correctness;
 * an eventual backend must independently establish installed sequencer state.
 */
static int checked(int r)
{
	return r > 0 ? -EPROTO : r;
}

static int geometry_ok(const struct ppn_stage_geometry *g)
{
	unsigned int sum;

	if (!g || g->bus > 1 || !g->ce_mask || !g->caus || !g->blocks_per_cau ||
	    !g->slc_pages || !g->owned_blocks || !g->seq_ticks ||
	    g->addrbytes < 3 || g->addrbytes > 4 || !g->page_bits || !g->block_bits ||
	    !g->cau_bits || g->page_bits > 30 || g->block_bits > 30 || g->cau_bits > 30)
		return -EINVAL;
	sum = g->page_bits + g->block_bits + g->cau_bits;
	if (sum >= 32 || sum >= g->addrbytes * 8 || g->caus > (1u << g->cau_bits) ||
	    g->blocks_per_cau > (1u << g->block_bits) || g->slc_pages > (1u << g->page_bits) ||
	    g->owned_first_block >= g->blocks_per_cau ||
	    g->owned_blocks > g->blocks_per_cau - g->owned_first_block)
		return -EINVAL;
	return 0;
}

int ppn_stage_encode(const struct ppn_stage_geometry *g, const struct ppn_stage_address *a, u32 *row)
{
	unsigned int p, b;

	if (geometry_ok(g) || !a || !row || !a->slc || a->physical_ce >= 8 ||
	    !(g->ce_mask & (1u << a->physical_ce)) || a->cau >= g->caus ||
	    a->page >= g->slc_pages || a->block < g->owned_first_block ||
	    a->block - g->owned_first_block >= g->owned_blocks)
		return -EINVAL;
	p = g->page_bits;
	b = g->block_bits;
	*row = a->page | (a->block << p) | ((u32)a->cau << (p + b)) | (1u << (p + b + g->cau_bits));
	return 0;
}

int ppn_stage_bind(struct ppn_stage_engine *e, const struct ppn_stage_ops *o, void *ctx,
		   const struct ppn_stage_geometry *g)
{
	if (!e)
		return -EINVAL;
	if (e->bound || e->blocked || e->busy)
		return -EALREADY;
	if (!o || !o->read_reg || !o->write_reg || !o->dma_start || !o->dma_wait ||
	    !o->dma_cancel || !o->dma_release || !o->nand_command || !o->nand_phase_status ||
	    !o->nand_entry_status || !o->nand_end || !o->feature_copy ||
	    !o->now_us || !o->delay_us)
		return -ENODEV;
	if (geometry_ok(g))
		return -EINVAL;
	e->ops = o;
	e->ctx = ctx;
	e->g = *g;
	e->bound = true;
	return 0;
}
struct operation {
	struct ppn_stage_engine *e;
	u64 start, last, timeout;

};
static int remaining(struct operation *p, u64 *left)
{
	u64 now = p->e->ops->now_us(p->e->ctx), elapsed;

	if (now < p->last || now < p->start)
		return -EIO;
	p->last = now;
	elapsed = now - p->start;
	if (elapsed >= p->timeout)
		return -ETIMEDOUT;
	*left = p->timeout - elapsed;
	return 0;
}

static int readreg(struct operation *p, enum ppn_stage_reg reg, u32 *v)
{
	u64 left;
	int r = remaining(p, &left);

	if (r)
		return r;
	*v = 0;
	r = checked(p->e->ops->read_reg(p->e->ctx, reg, v));
	return r ? r : remaining(p, &left);
}

static int wr(struct operation *p, enum ppn_stage_reg reg, u32 v)
{
	u64 left;
	int r = remaining(p, &left);

	if (r)
		return r;
	r = checked(p->e->ops->write_reg(p->e->ctx, reg, v));
	return r ? r : remaining(p, &left);
}

static int pause_poll(struct operation *p)
{
	u64 before = p->last, left;
	int r = checked(p->e->ops->delay_us(p->e->ctx, 1));

	if (r)
		return r;
	r = remaining(p, &left);
	if (r)
		return r;
	/*
	 * Successful 1us delay without clock progress violates the callback contract.
	 * Fail immediately; frozen/defective clocks cannot create infinite polling.
	 */
	return p->last > before ? 0 : -EIO;
}

static int cancel_own(struct ppn_stage_engine *e, void *lease)
{
	u64 start = e->ops->now_us(e->ctx), end;
	int r;

	/* Cleanup gets its own budget even after the main operation deadline expires. */
	r = checked(e->ops->dma_cancel(e->ctx, lease, PPN_STAGE_CLEANUP_TIMEOUT_US));
	if (r)
		return r;
	end = e->ops->now_us(e->ctx);
	if (end < start)
		return -EIO;
	if (end - start >= PPN_STAGE_CLEANUP_TIMEOUT_US)
		return -ETIMEDOUT;
	return 0;
}

static int fifo(struct operation *p, enum ppn_stage_reg target, u32 word)
{
	enum ppn_stage_reg depth = target == PPN_S_COMMAND ? PPN_S_COMMAND_DEPTH : PPN_S_PARAMETER_DEPTH;
	u32 n;
	int r;

	do {
		r = readreg(p, depth, &n);
		if (r)
			return r;
		if ((n & 0x3f) < 32)
			return wr(p, target, word);
		r = pause_poll(p);
		if (r)
			return r;
	} while (1);
}

static int drain(struct operation *p, const struct ppn_stage_address *a, u8 *status)
{
	u32 event, raw, ctl;
	int r;

	r = fifo(p, PPN_S_COMMAND, 0x11000605);
	if (r)
		return r;
	do {
		r = readreg(p, PPN_S_EVENT, &event);
		if (r)
			return r;
		if (event & 0x81)
			break;
		r = pause_poll(p);
		if (r)
			return r;
	} while (1);
	r = wr(p, PPN_S_EVENT, event & 0x81);
	if (r)
		return r;
	if (!(event & 1) || (event & 0x80))
		return -EIO;
	r = readreg(p, PPN_S_NAND_STATUS, &raw);
	if (r)
		return r;
	*status = (u8)raw;
	/* Check exact physical CE ownership even on failure. No global GEB recovery. */
	r = readreg(p, PPN_S_CE, &ctl);
	if (r)
		return r;
	if (ctl != (1u << a->physical_ce))
		return -ESTALE;
	r = readreg(p, PPN_S_CONTROL, &ctl);
	if (r)
		return r;
	return wr(p, PPN_S_CONTROL, ctl | 0x100);
}

static int begin(struct ppn_stage_engine *e, u64 timeout, struct ppn_stage_result *r, struct operation *p)
{
	if (!e || !r)
		return -EINVAL;
	memset(r, 0, sizeof(*r));
	r->durability_unknown = true;
	/* Rejected retries must preserve the held-buffer retention warning. */
	r->unquiesced_dma = !!(e->data_lease || e->meta_lease);
	if (!e->bound || !e->ops)
		return -ENODEV;
	if (e->blocked)
		return -EIO;
	if (e->busy)
		return -EBUSY;
	if (!timeout || timeout > PPN_STAGE_TIMEOUT_MAX_US)
		return -EINVAL;
	p->e = e;
	p->timeout = timeout;
	p->start = e->ops->now_us(e->ctx);
	p->last = p->start;
	return 0;
}

static int done(struct ppn_stage_engine *e, int error, struct ppn_stage_result *r)
{
	e->busy = false;
	if (error)
		e->blocked = true;
	r->transport_complete = !error;
	return error;
}

static u32 leword(const u8 *p)
{
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static int collect_lists(struct operation *p, const struct ppn_stage_address *a, u32 row,
			 struct ppn_stage_result *r)
{
	static const unsigned int features[] = {0x3080, 0x3280, 0x3180};
	unsigned int i, j;
	u64 left;
	u32 count;
	int error;

	for (i = 0; i < 3; i++) {
		error = remaining(p, &left);
		if (error)
			return error;
		error = checked(p->e->ops->feature_copy(p->e->ctx, a->physical_ce, features[i], r->list_snapshot[i], left));
		if (error)
			return error;
		error = remaining(p, &left);
		if (error)
			return error;
		count = leword(r->list_snapshot[i]);
		if (count > PPN_STAGE_LIST_MAX)
			return -EPROTO;
		/* Single-page request: a nonempty list can contain our exact row once only. */
		if (count > 1)
			return -EPROTO;
		for (j = 0; j < count; j++)
			if (leword(r->list_snapshot[i] + 4 + 4 * j) != row)
				return -ESTALE;
		r->list_valid[i] = true;
	}
	/* EF3380 meaning/durability unresolved: do not acknowledge or replay. */
	return -EIO;
}

int ppn_stage_program(struct ppn_stage_engine *e, const struct ppn_stage_address *a,
		      const void *data, size_t dl, const void *meta, size_t ml, u64 timeout, struct ppn_stage_result *r)
{
	struct operation p;
	u32 row;
	u64 left;
	unsigned int d, m, i;
	int error, cleanup;
	bool live_d = false, live_m = false, reconcile = false;

	error = begin(e, timeout, r, &p);
	if (error)
		return error;
	if (!data || !meta || dl != PPN_STAGE_DATA || ml != PPN_STAGE_META ||
	    ppn_stage_encode(&e->g, a, &row))
		return -EINVAL;
	e->busy = true;
	d = 5 + 2 * e->g.bus;
	m = d + 1;
	/* This private path expects the proven 4KiB+16B chunk envelope, one whole page. */
	error = wr(&p, PPN_S_CONTROL, 6);
	if (error)
		goto out;
	error = wr(&p, PPN_S_SEQ_TICKS, e->g.seq_ticks);
	if (error)
		goto out;
	error = wr(&p, PPN_S_CONTROL, 0x100);
	if (error)
		goto out;
	error = wr(&p, PPN_S_EVENT, ~(u32)0);
	if (error)
		goto out;
	error = wr(&p, PPN_S_CONFIG, 5);
	if (error)
		goto out;
	error = wr(&p, PPN_S_PIO, 0x20840004);
	if (error)
		goto out;
	error = wr(&p, PPN_S_ADDRNUM, e->g.addrbytes - 1);
	if (error)
		goto out;
	error = wr(&p, PPN_S_CONTROL, 0x1104);
	if (error)
		goto out;
	e->loaned_data = data;
	e->loaned_meta = meta;
	error = checked(e->ops->dma_start(e->ctx, d, PPN_S_DATA_FIFO, data, dl, 4, 8, &e->data_lease));
	live_d = !!e->data_lease;
	if (error)
		goto out;
	if (!live_d) {
		error = -EPROTO;
		goto out;
	}
	error = remaining(&p, &left);
	if (error)
		goto out;
	error = checked(e->ops->dma_start(e->ctx, m, PPN_S_META_FIFO, meta, ml, 1, 1, &e->meta_lease));
	live_m = !!e->meta_lease;
	if (error)
		goto out;
	if (!live_m) {
		error = -EPROTO;
		goto out;
	}
	error = remaining(&p, &left);
	if (error)
		goto out;
	error = fifo(&p, PPN_S_COMMAND, 0x09000000 | (1u << a->physical_ce));
	if (error)
		goto out;
	error = fifo(&p, PPN_S_PARAMETER, row);
	if (error)
		goto out;
	error = fifo(&p, PPN_S_PARAMETER, 0x8a);
	if (error)
		goto out;
	error = fifo(&p, PPN_S_COMMAND, 0x11001709);
	if (error)
		goto out;
	for (i = 0; i < 4; i++) {
		error = fifo(&p, PPN_S_COMMAND, 0x0b000000);
		if (error)
			goto out;
	}
	error = fifo(&p, PPN_S_COMMAND, 0x08000017);
	if (error)
		goto out;
	error = drain(&p, a, &r->raw_status);
	if (error)
		goto out;
	error = fifo(&p, PPN_S_COMMAND, 0x09000000);
	if (error)
		goto out;
	if (r->raw_status != 0x40) {
		reconcile = !!(r->raw_status & 1);
		error = -EIO;
		goto out;
	}
	error = remaining(&p, &left);
	if (error)
		goto out;
	error = checked(e->ops->dma_wait(e->ctx, e->data_lease, left));
	if (error)
		goto out;
	live_d = false;
	error = remaining(&p, &left);
	if (error)
		goto out;
	error = remaining(&p, &left);
	if (error)
		goto out;
	error = checked(e->ops->dma_wait(e->ctx, e->meta_lease, left));
	if (error)
		goto out;
	live_m = false;
	error = remaining(&p, &left);
	if (error)
		goto out;
	/* Do not inherit Apple's waitfailure→Cancel→success behavior. */
	e->ops->dma_release(e->ctx, e->meta_lease);
	e->meta_lease = NULL;
	e->ops->dma_release(e->ctx, e->data_lease);
	e->data_lease = NULL;
	e->loaned_data = NULL;
	e->loaned_meta = NULL;
	error = remaining(&p, &left);
	return done(e, error, r);
out:
	/*
	 * Only cancel leases still in flight. A completed lease cannot be retargeted
	 * to an unrelated transfer; no channel-wide control callback exists.
	 */
	if (e->meta_lease) {
		cleanup = live_m ? cancel_own(e, e->meta_lease) : 0;
		if (cleanup) {
			if (!r->cleanup_error)
				r->cleanup_error = cleanup;
		} else {
			e->ops->dma_release(e->ctx, e->meta_lease);
			e->meta_lease = NULL;
		}
	}
	if (e->data_lease) {
		cleanup = live_d ? cancel_own(e, e->data_lease) : 0;
		if (cleanup) {
			if (!r->cleanup_error)
				r->cleanup_error = cleanup;
		} else {
			e->ops->dma_release(e->ctx, e->data_lease);
			e->data_lease = NULL;
		}
	}
	r->unquiesced_dma = !!r->cleanup_error;
	if (!r->unquiesced_dma) {
		e->loaned_data = NULL;
		e->loaned_meta = NULL;
		if (reconcile)
			error = collect_lists(&p, a, row, r);
	}
	return done(e, error, r);
}

int ppn_stage_erase(struct ppn_stage_engine *e, const struct ppn_stage_address *a,
		    u64 timeout, struct ppn_stage_result *r)
{
	struct operation p;
	u32 row, ce;
	u64 left;
	u8 phase = 0;
	int error, ending;
	bool started = false;

	error = begin(e, timeout, r, &p);
	if (error)
		return error;
	if (!a || a->page || ppn_stage_encode(&e->g, a, &row))
		return -EINVAL;
	e->busy = true;
	error = wr(&p, PPN_S_CE, 1u << a->physical_ce);
	if (error)
		goto erase_out;
	started = true;
	error = checked(e->ops->nand_command(e->ctx, 0x6a, 0xd7, row, e->g.addrbytes));
	if (!error)
		error = remaining(&p, &left);
	if (!error) {
		error = remaining(&p, &left);
		if (!error)
			error = checked(e->ops->nand_phase_status(e->ctx, &phase, left));
	}
	if (!error)
		error = remaining(&p, &left);
	if (!error && phase != 0x40)
		error = -EIO;
	if (!error) {
		error = remaining(&p, &left);
		if (!error)
			error = checked(e->ops->nand_entry_status(e->ctx, &r->raw_status, left));
	}
	if (!error)
		error = remaining(&p, &left);
	if (!error && r->raw_status != 0x40)
		error = -EIO;
	if (!error) {
		error = readreg(&p, PPN_S_CE, &ce);
		if (!error && ce != (1u << a->physical_ce))
			error = -ESTALE;
	}
	/* Bounded end of OWN transaction only; its failure prevents success. */
	ending = started ? remaining(&p, &left) : 0;
	if (started && !ending)
		ending = checked(e->ops->nand_end(e->ctx, 0x77, left));
	if (!ending && started)
		ending = remaining(&p, &left);
	if (ending) {
		r->cleanup_error = ending;
		if (!error)
			error = ending;
	}
erase_out:
	return done(e, error, r);
}
