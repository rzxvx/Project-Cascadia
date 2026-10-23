/* SPDX-License-Identifier: GPL-2.0-only */
/* Unbound Linux-owned lifecycle. No MMIO, IRQ routing, DMA or NAND binding. */
#ifndef PPN_CDMA_COMPLETION_H
#define PPN_CDMA_COMPLETION_H
#include "ppn_cdma_plan.h"
#ifndef __KERNEL__
#include <stdbool.h>
#endif

#define PPN_CDMA_IRQ_ERROR 0x40000u
#define PPN_CDMA_IRQ_DONE 0x80000u
#define PPN_CDMA_IRQ_SPURIOUS 0x100000u
#define PPN_CDMA_ACTION_ACK 1u
#define PPN_CDMA_ACTION_DISABLE 2u
#define PPN_CDMA_FENCE_EXCLUSIVE 1u
#define PPN_CDMA_FENCE_NO_ACCESS 2u
#define PPN_CDMA_FENCE_IRQ_DRAINED 4u
#define PPN_CDMA_FENCE_BARRIERS 8u
#define PPN_CDMA_FENCE_ALL 15u

enum ppn_cdma_completion_phase {
	PPN_CDMA_UNINITIALIZED, PPN_CDMA_ACTIVE, PPN_CDMA_WAIT_ACTION,
	PPN_CDMA_WAIT_FENCE, PPN_CDMA_FINISHED, PPN_CDMA_HOLD,
};
/* These are Linux allocator/lease identities, not tags decoded from Apple IRQs.
 * All IDs are nonzero and never reused/wrapped while an old DMA/IRQ can exist.
 * lease_cookie names an OWN submitted transfer (e.g. engine opaque lease), not
 * merely a channel or a reusable object address. The external allocator retains
 * the reservation, immutable buffers and controller lock through the final fence.
 */
struct ppn_cdma_identity {
	u64 controller, epoch, transfer, lease_cookie;
	u32 channel;
};
struct ppn_cdma_reservation {
	struct ppn_cdma_identity identity;
	bool exclusive, handoff_quiesced, prior_irq_drained;
};
struct ppn_cdma_observation {
	struct ppn_cdma_identity identity;
	u64 observation;
	u32 control, pending, remaining;
	bool exclusive_current;
};
struct ppn_cdma_action {
	struct ppn_cdma_identity identity;
	u64 observation;
	u32 mask, ack_offset, ack_value, disable_offset, disable_value;
};
struct ppn_cdma_action_receipt {
	struct ppn_cdma_action action;
	u32 attempted;
	int ack_error, disable_error;
	bool exclusive_current;
};
struct ppn_cdma_fence_receipt {
	struct ppn_cdma_identity identity;
	u64 observation;
	u32 flags;
	int error;
};
struct ppn_cdma_completion {
	enum ppn_cdma_completion_phase phase;
	struct ppn_cdma_identity identity;
	struct ppn_cdma_plan plan;
	struct ppn_cdma_action action;
	int terminal_error;
};

/* Fresh all-zero storage only; no reinit/reset API. All calls serialized. Config,
 * request/SG/receipt objects are immutable during a call; input/output/state must
 * not alias. Caller supplies valid regular-memory objects, not MMIO. In-memory
 * structs are not an on-wire format. Ownership/drain/barrier receipts require
 * genuine externally qualified backend evidence, absent in this experiment.
 * A successful begin produces a planner snapshot; it does NOT submit DMA.
 */
int ppn_cdma_completion_begin(struct ppn_cdma_completion *state,
			      const struct ppn_cdma_request *request,
			      const struct ppn_cdma_reservation *reservation);
/* request_key identifies the API object; a wrong key is an unchanged -EINVAL.
 * A stale/foreign ACTUAL observation is -ESTALE and HOLD, retaining the current
 * lease, never ACKing/retiring/releasing the newly owned transfer. No80000 or
 * spurious is -EAGAIN/no-action, not an invented hardware-DMA failure. Waiting
 * is separately bounded by the engine/caller. Only one eligible IRQ observation
 * is consumed. Refill/partial events may plan exact ACK but never completion.
 */
int ppn_cdma_completion_observe(struct ppn_cdma_completion *state,
				const struct ppn_cdma_identity *request_key,
				const struct ppn_cdma_observation *observation,
				struct ppn_cdma_action *action);
/* Receipts describe the exact planned W1C/disable operation, never all-mask ACK.
 * Backend must revalidate this exclusive lease BEFORE each write, ACK first,
 * abort on ACK failure, and disable only this channel after a successful ACK.
 * Reservation remains continuously held through the fence; a stale plan alone
 * never authorizes I/O. Nonzero/error/invalid receipts latch HOLD, without retry.
 * No actual callback/register access is executed by these functions.
 */
int ppn_cdma_completion_actions(struct ppn_cdma_completion *state,
				const struct ppn_cdma_identity *request_key,
				const struct ppn_cdma_action_receipt *receipt);
int ppn_cdma_completion_fence(struct ppn_cdma_completion *state,
			      const struct ppn_cdma_identity *request_key,
			      const struct ppn_cdma_fence_receipt *receipt);
/* Engine dma_wait may return0 only after FINISHED; neither wait nor finish
 * releases the lease. HOLD always retains it for a separately proven cancel /
 * quiesce recovery, which this module does not implement. A zero return is DMA
 * memory/FIFO completion with external fence only, never NAND durability.
 */
int ppn_cdma_completion_wait(const struct ppn_cdma_completion *state,
			     const struct ppn_cdma_identity *request_key);
#endif
