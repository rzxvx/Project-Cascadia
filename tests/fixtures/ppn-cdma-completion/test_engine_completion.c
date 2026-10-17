// SPDX-License-Identifier: GPL-2.0-only
/* Reuse frozen strict process-local engine fakes, not their success-only DMA API. */
#define main frozen_stage_fixture_main
#include "test_ppn_stage.c"
#undef main
#include "ppn_cdma_completion.h"

static struct ppn_cdma_completion completion[2];
static unsigned int inject_mode, inject_fifo, completions, held;
static struct ppn_cdma_identity ids[2];
static int start_completion(void *v, unsigned int channel, enum ppn_stage_fifo fifo,
			    const void *buffer, size_t length, unsigned int word,
			    unsigned int block, void **lease)
{
	struct ppn_cdma_sg sg = {0x40000000 + fifo * 0x100000, (u32)length};
	struct ppn_cdma_request r = {geometry.bus, fifo, 1, 0, 0,
		0x50000000 + fifo * 0x100000, 96, (u32)length, word, block, 1, &sg};
	int error = start(v, channel, fifo, buffer, length, word, block, lease);
	struct ppn_cdma_reservation owned;

	if (error)
		return error;
	assert(*lease);
	ids[fifo] = (struct ppn_cdma_identity){1, 2, 3 + fifo,
		(u64)(uintptr_t)*lease, channel};
	owned = (struct ppn_cdma_reservation){ids[fifo], true, true, true};
	return ppn_cdma_completion_begin(&completion[fifo], &r, &owned);
}
static int wait_completion(void *v, void *lease, u64 budget)
{
	struct fake *f = v;
	struct lease *l = owned(f, lease);
	unsigned int fifo = l == &f->leases[1];
	struct ppn_cdma_completion *s = &completion[fifo];
	struct ppn_cdma_observation o = {ids[fifo], 20 + fifo, 0x80000, 0, 0, true};
	struct ppn_cdma_action action;
	struct ppn_cdma_action_receipt receipt;
	struct ppn_cdma_fence_receipt fence = {ids[fifo], 20 + fifo, 15, 0};
	int error;

	assert(budget > 0 && l->active);
	if (fifo == inject_fifo) {
		if (inject_mode == 1) o.control = 0;
		if (inject_mode == 2) o.control = 0x180000;
		if (inject_mode == 3) o.control = 0xc0000;
		if (inject_mode == 4) o.pending = 64;
		if (inject_mode == 5) o.remaining = 0x80000000;
		if (inject_mode == 6) o.identity.epoch--;
		if (inject_mode == 7) fence.flags = 7;
		if (inject_mode == 8) fence.identity.lease_cookie++;
	}
	error = ppn_cdma_completion_observe(s, &ids[fifo], &o, &action);
	if (error)
		return error;
	receipt = (struct ppn_cdma_action_receipt){action, action.mask, 0, 0, true};
	if (fifo == inject_fifo && inject_mode == 9) {
		receipt.attempted = 1;
		receipt.ack_error = -ETIMEDOUT;
	}
	if (fifo == inject_fifo && inject_mode == 10)
		receipt.disable_error = -EIO;
	error = ppn_cdma_completion_actions(s, &ids[fifo], &receipt);
	if (error)
		return error;
	error = ppn_cdma_completion_fence(s, &ids[fifo], &fence);
	if (error)
		return error;
	error = ppn_cdma_completion_wait(s, &ids[fifo]);
	if (!error) {
		l->active = false;
		completions++;
	}
	return error;
}
static int no_unproven_cancel(void *v, void *lease, u64 budget)
{
	struct fake *f = v;

	assert(owned(f, lease)->active && budget == PPN_STAGE_CLEANUP_TIMEOUT_US);
	held++;
	return -EOPNOTSUPP;
}
static void release_completion(void *v, void *lease)
{
	struct fake *f = v;
	unsigned int fifo = owned(f, lease) == &f->leases[1];

	assert(!ppn_cdma_completion_wait(&completion[fifo], &ids[fifo]));
	release_dma(v, lease);
}
int main(void)
{
	struct ppn_stage_ops qualified = ops;
	struct ppn_stage_engine engine;
	struct ppn_stage_result result;
	struct fake f;
	unsigned int cases = 0;
	int error;

	qualified.dma_start = start_completion;
	qualified.dma_wait = wait_completion;
	qualified.dma_cancel = no_unproven_cancel;
	qualified.dma_release = release_completion;
	for (unsigned int bus = 0; bus < 2; bus++)
		for (unsigned int target = 0; target < 2; target++)
			for (unsigned int mode = 0; mode <= 10; mode++) {
				memset(&f, 0, sizeof(f));
				memset(&engine, 0, sizeof(engine));
				memset(completion, 0, sizeof(completion));
				geometry.bus = bus;
				f.ce = f.event = 1;
				f.status = 0x40;
				inject_mode = mode;
				inject_fifo = target;
				completions = held = 0;
				assert(!ppn_stage_bind(&engine, &qualified, &f, &geometry));
				error = program(&engine, &result, 1000000);
				if (!mode) {
					assert(!error && completions == 2 && f.release_count == 2);
					assert(result.transport_complete && result.durability_unknown);
					idle(&f, &engine);
				} else {
					assert(error < 0 && !result.transport_complete && result.durability_unknown);
					assert(result.unquiesced_dma && result.cleanup_error == -EOPNOTSUPP);
					assert(engine.blocked && engine.loaned_data == data && engine.loaned_meta == meta);
					assert(completions == target && f.release_count == target);
					assert(held == 2 - target);
					assert(engine.meta_lease == &f.leases[1]);
					assert(target ? !engine.data_lease : engine.data_lease == &f.leases[0]);
					assert(program(&engine, &result, 1000000) == -EIO);
				}
				cases++;
			}
	printf("PASS %u actual frozen-engine/lease-completion cases\n", cases);
	return 0;
}
