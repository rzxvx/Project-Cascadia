/* SPDX-License-Identifier: GPL-2.0-only */
/* Private Linux-owned journal transaction model, no device or Apple format. */
#ifndef PPN_BBT_TX_H
#define PPN_BBT_TX_H
#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdbool.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
#endif
#define PPN_BBT_MAX_BLOCKS 65536u
#define PPN_BBT_BITMAP_BYTES (PPN_BBT_MAX_BLOCKS / 8u)
#define PPN_BBT_NO_BLOCK 0xffffffffu
enum ppn_bbt_phase {
	PPN_BBT_UNINITIALIZED, PPN_BBT_GENESIS, PPN_BBT_READY,
	PPN_BBT_WAIT_BODY, PPN_BBT_WAIT_MANIFEST, PPN_BBT_ACTIVE,
	PPN_BBT_READONLY
};
enum ppn_bbt_operation { PPN_BBT_NONE, PPN_BBT_PROGRAM, PPN_BBT_ERASE };
enum ppn_bbt_evidence { PPN_BBT_SLOT_ERASED, PPN_BBT_BODY_DURABLE, PPN_BBT_MANIFEST_DURABLE };
struct ppn_bbt_slot {
	u64 physical_id;
	u64 alias_domain;
};

struct ppn_bbt_config {
	u64 media_id;
	u64 owner_id;
	u64 lease_epoch;
	u32 blocks;
	struct ppn_bbt_slot slots[2];
	bool ownership_proven;
};

/* Normalized snapshot, NOT an on-flash C ABI. A future codec must serialize
 * every field explicitly and use separate whole16KiB body/manifest pages.
 * Unused bitmap bytes/bits must be zero; generation never wraps.
 */
struct ppn_bbt_snapshot {
	u64 media_id;
	u64 owner_id;
	u64 record_epoch;
	u64 generation;
	u64 physical_slot;
	u64 alias_domain;
	u32 blocks;
	u32 operation;
	u32 pending_block;
	u8 bad[PPN_BBT_BITMAP_BYTES];
};

/* An external source-of-truth supplies these fields; neither an engine0 return
 * nor this struct's existence proves NAND durability. The current hardware
 * backend cannot provide them. No callback is invoked by this module.
 */
struct ppn_bbt_receipt {
	u64 media_id;
	u64 owner_id;
	u64 lease_epoch;
	u64 generation;
	u64 physical_slot;
	u64 alias_domain;
	u32 body_crc;
	u32 manifest_crc;
	u32 stage;
	bool durable;
	bool quiesced;
};

struct ppn_bbt_copy {
	struct ppn_bbt_snapshot snapshot;
	u32 body_crc;
	u32 manifest_crc;
	bool body_present;
	bool manifest_present;
	/* Full-page read completed; absence additionally needs erased-page proof. */
	bool complete_read;
	bool absent_manifest_erased;
	bool empty_slot_erased;
};

/* Allocate fresh ZEROED workspace in owned heap, never kernel stack (~16KiB).
 * create_empty/recover are one-shot initializers; any nonzero/reused workspace
 * returns-EALREADY without changing one byte, including readonly failure state.
 * Config/copies/receipts remain immutable and may not alias the workspace or
 * outputs. Calls are serialized under the caller's exclusive controller lock.
 * There is no reset API; retaining an unresolved intent cannot be bypassed by
 * reinitialization. A new workspace requires independent recovery/ownership.
 */
struct ppn_bbt_tx {
	struct ppn_bbt_config config;
	struct ppn_bbt_snapshot committed;
	struct ppn_bbt_snapshot prepared;
	u32 committed_crc;
	u32 prepared_crc;
	u32 active_slot;
	u32 target_slot;
	u32 phase;
	u32 after_commit;
};

u32 ppn_bbt_snapshot_crc(const struct ppn_bbt_snapshot *snapshot);
u32 ppn_bbt_manifest_crc(const struct ppn_bbt_snapshot *snapshot, u32 body_crc);
/* Genesis additionally requires TWO whole-slot erased, durable and quiesced
 * receipts bound to current owner/media/lease/physical/alias identity, generation0.
 * Ownership alone never establishes blank journal media.
 */
int ppn_bbt_create_empty(struct ppn_bbt_tx *tx, const struct ppn_bbt_config *config,
			 const struct ppn_bbt_receipt erased_slots[2]);
int ppn_bbt_recover(struct ppn_bbt_tx *tx, const struct ppn_bbt_config *config,
		    const struct ppn_bbt_copy copies[2]);
int ppn_bbt_checkpoint(struct ppn_bbt_tx *tx, const struct ppn_bbt_receipt *erased);
int ppn_bbt_begin(struct ppn_bbt_tx *tx, enum ppn_bbt_operation operation,
		  u32 block, const struct ppn_bbt_receipt *erased);
/* confirmed_retirement is a distinct physical-policy witness, not a generic
 * error result. Unknown/failed quiescence leaves persisted intent and readonly.
 * Return0 means a completion journal PLAN is prepared, not I/O success.
 */
int ppn_bbt_finish(struct ppn_bbt_tx *tx, bool operation_succeeded,
		   bool flash_durable, bool dma_quiesced, bool confirmed_retirement,
		   const struct ppn_bbt_receipt *erased);
int ppn_bbt_get_manifest(struct ppn_bbt_tx *tx, u32 *manifest_crc);
int ppn_bbt_confirm(struct ppn_bbt_tx *tx, const struct ppn_bbt_receipt *receipt);
#endif
