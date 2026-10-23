/* SPDX-License-Identifier: GPL-2.0-only */
/* Unbound clean-room CDMA plan builder. No register access or DMA submission. */
#ifndef PPN_CDMA_PLAN_H
#define PPN_CDMA_PLAN_H
#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stddef.h>
#include <stdint.h>
typedef uint32_t u32;
typedef uint64_t u64;
#endif
#define PPN_CDMA_MAX_SG 127u
#define PPN_CDMA_CHAIN_WORDS ((PPN_CDMA_MAX_SG + 2u) * 8u)
struct ppn_cdma_sg {
	u32 dma_address;
	u32 length;
};

struct ppn_cdma_request {
	u32 bus;
	u32 metadata;
	u32 direction;
	u32 selector;
	u32 start_selector;
	u32 chain_dma;
	u32 chain_bytes;
	u32 size;
	u32 word;
	u32 burst;
	u32 count;
	const struct ppn_cdma_sg *sg;
};

struct ppn_cdma_plan {
	/* CPU-native words; backend must serialize little-endian descriptors. */
	u32 chain[PPN_CDMA_CHAIN_WORDS];
	u32 channel;
	u32 cache_bytes;
	u32 register_offsets[6];
	u32 register_values[6];
};

/* Complete one non-AES batch only. Request and output may not alias. Every DMA
 * address is a caller-owned mapping, never inferred from a CPU pointer.
 * Caller holds controller/channel reservations and maps/cleans OWN descriptors.
 * Returns only a plan; no resource lease or transport/durability claim is made.
 * Input errors leave output unchanged. Reserved descriptor words are zeroed.
 * Two spare entries make the original next-pointer/clean range stay in allocation.
 * Not a device backend: requires cache coherency, barriers, IRQ/quiescence,
 * initialization, endian/DMA-address applicability and channel ownership evidence.
 */
int ppn_cdma_build_plan(const struct ppn_cdma_request *request,
			struct ppn_cdma_plan *plan);
#endif
