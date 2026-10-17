/* SPDX-License-Identifier: GPL-2.0-only */
/* Private clean-room PPN orchestration foundation. No hardware binding.
 * A zero return means TRANSPORT completion only, never durable storage.
 * This is not an MTD writer; integrating it requires independently established
 * SLC block exclusivity, protected regions, retirement and crash guarantees.
 */
#ifndef PPN_STAGE_ENGINE_H
#define PPN_STAGE_ENGINE_H
#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
#endif
#define PPN_STAGE_DATA 16384u
#define PPN_STAGE_META 64u
#define PPN_STAGE_LIST 512u
#define PPN_STAGE_LIST_MAX 127u
#define PPN_STAGE_TIMEOUT_MAX_US 60000000u
#define PPN_STAGE_CLEANUP_TIMEOUT_US 2000000u
/* Controller-register offsets, not pointers or actual mapped addresses. */
enum ppn_stage_reg {
	PPN_S_CONFIG = 0, PPN_S_CONTROL = 4, PPN_S_PIO = 0x34,
	PPN_S_CE = 0x4000c, PPN_S_ADDRNUM = 0x40020, PPN_S_NAND_STATUS = 0x40048,
	PPN_S_COMMAND = 0xc0000, PPN_S_COMMAND_DEPTH = 0xc0004,
	PPN_S_PARAMETER = 0xc0014, PPN_S_PARAMETER_DEPTH = 0xc0018,
	PPN_S_EVENT = 0xc0020, PPN_S_SEQ_TICKS = 0xc002c,
};
enum ppn_stage_fifo { PPN_S_DATA_FIFO, PPN_S_META_FIFO };
/* Register/command callbacks must be bounded synchronous accesses. Status/wait/
 * cancel callbacks must honor their budget; engine also checks elapsed time AFTER
 * success. Software cannot preempt a callback violating this contract. now_us
 * must be monotonic microseconds and delay_us(1) must advance that clock. Every
 * int callback returns0 or negativeerrno; positive replies are rejected.
 * DMA leases must identify the OWN submitted transfer, never merely a channel.
 * Failed start with NULLlease guarantees noDMAwasaccepted; a partial start must
 * return its own lease even on failure. Wait/cancel never release ownership;
 * release is called only after proven completion/cancel. Caller holds resource
 * reservations and the controller lock for the entire operation.
 */
struct ppn_stage_ops {
	int (*read_reg)(void *ctx, enum ppn_stage_reg reg, u32 *value);
	int (*write_reg)(void *ctx, enum ppn_stage_reg reg, u32 value);
	/* CPU buffers remain owned by caller; successful cancel MUST prove that no
	 * DMA can access them afterwards. Mapping/AES/descriptor code is absent.
	 */
	int (*dma_start)(void *ctx, unsigned int channel, enum ppn_stage_fifo fifo,
			 const void *buffer, size_t length, unsigned int word, unsigned int block, void **lease_out);
	int (*dma_wait)(void *ctx, void *lease, u64 remaining_us);
	int (*dma_cancel)(void *ctx, void *lease, u64 cleanup_timeout_us);
	void (*dma_release)(void *ctx, void *lease);
	/* Narrow whole-row PPN phase operations; these callbacks are never bound to
	 * a controller here. Status callbacks return raw status without interpreting it.
	 */
	int (*nand_command)(void *ctx, u8 first, u8 second, u32 row, unsigned int addrbytes);
	int (*nand_phase_status)(void *ctx, u8 *raw, u64 remaining_us);
	int (*nand_entry_status)(void *ctx, u8 *raw, u64 remaining_us);
	int (*nand_end)(void *ctx, u8 command, u64 remaining_us);
	/* Copy exactly512 bytes into owned output. Three error-list EE queries only;
	 * no EF3380 callback exists because its acknowledgement meaning is unknown.
	 */
	int (*feature_copy)(void *ctx, u8 physical_ce, unsigned int feature,
			    u8 out[PPN_STAGE_LIST], u64 remaining_us);
	u64 (*now_us)(void *ctx);
	int (*delay_us)(void *ctx, unsigned int duration_us);
};
struct ppn_stage_geometry {
	u8 bus, ce_mask, caus, page_bits, block_bits, cau_bits, addrbytes;
	u32 blocks_per_cau, slc_pages, owned_first_block, owned_blocks;
	u32 seq_ticks; /* Explicit prevalidated controller ticks; no invented unit. */
};
struct ppn_stage_address {
	u8 physical_ce, cau;
	u32 block, page;
	bool slc;
};
struct ppn_stage_result {
	bool transport_complete; /* Does NOT imply persistent completion. */
	bool durability_unknown; /* Always true in this staged engine. */
	bool unquiesced_dma; /* Caller MUST retain buffers while true, even on a rejected retry. */
	int cleanup_error;
	u8 raw_status;
	bool list_valid[3];
	u8 list_snapshot[3][PPN_STAGE_LIST];
};
struct ppn_stage_engine {
	const struct ppn_stage_ops *ops;
	void *ctx;
	struct ppn_stage_geometry g;
	bool bound, busy, blocked;
	const void *loaned_data, *loaned_meta;
	void *data_lease, *meta_lease;
};
/* Caller initializes engine to zero exactly once and holds one controller lock.
 * Rebinding an initialized engine is refused. There is no clear-blocked/resetAPI.
 * A rejected call reports previously held DMA leases in unquiesced_dma. Its
 * cleanup_error describes this call only; zero does not clear prior ownership.
 * Inputs, outputs, engine and borrowed buffers must be nonaliasing regular memory
 * and immutable while in use; callbacks must not reenter or mutate the engine.
 */
int ppn_stage_bind(struct ppn_stage_engine *engine, const struct ppn_stage_ops *ops,
		   void *ctx, const struct ppn_stage_geometry *geometry);
int ppn_stage_encode(const struct ppn_stage_geometry *geometry,
		     const struct ppn_stage_address *address, u32 *row);
int ppn_stage_program(struct ppn_stage_engine *engine,
		      const struct ppn_stage_address *address,
		      const void *data, size_t data_len, const void *meta, size_t meta_len,
		      u64 timeout_us, struct ppn_stage_result *result);
int ppn_stage_erase(struct ppn_stage_engine *engine,
		    const struct ppn_stage_address *address,
		    u64 timeout_us, struct ppn_stage_result *result);
#endif
