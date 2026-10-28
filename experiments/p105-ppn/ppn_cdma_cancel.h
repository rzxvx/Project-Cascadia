/* SPDX-License-Identifier: GPL-2.0-only */
/* Unbound checked cancellation transport. No hardware/IRQ/NAND binding. */
#ifndef PPN_CDMA_CANCEL_H
#define PPN_CDMA_CANCEL_H
#include "ppn_cdma_completion.h"

#define PPN_CDMA_CANCEL_MAX_US 2000000u
#define PPN_CDMA_CANCEL_BUSY_MASK 0x30000u
#define PPN_CDMA_CANCEL_BUSY 0x10000u

enum ppn_cdma_cancel_phase {
	PPN_CDMA_CANCEL_UNINITIALIZED, PPN_CDMA_CANCEL_READY,
	PPN_CDMA_CANCEL_RUNNING, PPN_CDMA_CANCEL_WAIT_FENCE,
	PPN_CDMA_CANCEL_HOLD,
};
struct ppn_cdma_cancel_adoption {
	struct ppn_cdma_identity identity;
	bool own_submitted_lease, exclusive, initial_handoff_fenced;
};
/* All callbacks are bounded synchronous operations, return0/negative errno,
 * and may not mutate state/config/identity. validate_lease must establish the
 * exact CURRENT exclusively held lease, not channel-only ownership; reservation
 * and serialization are continuous, not reconstructed from a register sample.
 * Every I/O receives the exact identity and remaining CLEANUP budget.
 * now_us is monotonic; delay_us(1) must advance it. No clock/elapsed wrapper here
 * can preempt an external callback that violates its synchronous time budget.
 */
struct ppn_cdma_cancel_ops {
	int (*validate_lease)(void *ctx, const struct ppn_cdma_identity *identity);
	int (*read)(void *ctx, const struct ppn_cdma_identity *identity,
		    u32 offset, u32 *value, u64 remaining_us);
	int (*write)(void *ctx, const struct ppn_cdma_identity *identity,
		     u32 offset, u32 value, u64 remaining_us);
	u64 (*now_us)(void *ctx);
	int (*delay_us)(void *ctx, unsigned int us, u64 remaining_us);
};
struct ppn_cdma_cancel {
	enum ppn_cdma_cancel_phase phase;
	struct ppn_cdma_identity identity;
	const struct ppn_cdma_cancel_ops *ops;
	void *ctx;
	u64 start_us, last_us, timeout_us;
	u32 last_control, polls;
	int error;
};
/* Fresh zero state exactly once, no reset/rebind. Serialized calls and immutable,
 * nonaliasing regular-memory input/output/state required. Unaccepted/prepared
 * DMA is not an own submitted lease. Adoption requires real external evidence,
 * absent in this experiment. IDs cannot be reused/wrapped while old events can
 * exist. Buffers/descriptors stay owned by the caller throughout all phases.
 */
int ppn_cdma_cancel_begin(struct ppn_cdma_cancel *state,
			  const struct ppn_cdma_cancel_adoption *adoption,
			  const struct ppn_cdma_cancel_ops *ops, void *ctx);
/* timeout_us starts a FRESH cleanup deadline, independent of the original failed
 * operation deadline. Actual source-backed SET/CONTROL2/poll/CLR order only.
 * Raw control states other than10000 are not assigned electrical meanings.
 * Success returns -EINPROGRESS / WAIT_FENCE, NEVER dma_cancel0 or release.
 * Still requires separate true no-future-access/IRQ-drained/barrier witnesses.
 * Errors latch HOLD and retain lease/buffers, with no blind retry/fallback/reset.
 * Post-I/O ownership/clock checks can reject even a successful transport write.
 */
int ppn_cdma_cancel_run(struct ppn_cdma_cancel *state,
			const struct ppn_cdma_identity *request_key, u64 timeout_us);
int ppn_cdma_cancel_wait(const struct ppn_cdma_cancel *state,
			 const struct ppn_cdma_identity *request_key);
#endif
