/* SPDX-License-Identifier: GPL-2.0-only */
/* Linux-owned EXPERIMENTAL byte schema. No Apple format, I/O or proof flags. */
#ifndef PPN_BBT_CODEC_H
#define PPN_BBT_CODEC_H
#include "ppn_bbt_tx.h"
#ifndef __KERNEL__
#include <stddef.h>
#endif
#define PPN_BBT_CODEC_PAGE 16384u
#define PPN_BBT_CODEC_HEADER 128u
#define PPN_BBT_CODEC_BODY_USED (PPN_BBT_CODEC_HEADER + PPN_BBT_BITMAP_BYTES)
struct ppn_bbt_body_view {
	struct ppn_bbt_snapshot snapshot;
	u32 body_crc;
};

struct ppn_bbt_decoded {
	struct ppn_bbt_snapshot snapshot;
	u32 body_crc;
	u32 manifest_crc;
};

/* All input objects are immutable, completely readable ordinary RAM and cannot
 * overlap each other or output. Calls are caller-serialized. Exact16KiB buffers
 * only; output is unchanged on errors. Workspace/output structs belong in heap.
 * Encoders return byte PLANS, never permission to submit/commit them. The v3
 * transaction machine and EXTERNAL durable/quiesced/blank/ownership witnesses
 * remain mandatory; no codec function creates or modifies those proof flags.
 * Config binds expected media/owner/geometry/physical slot/alias, not authority.
 * Encode requires the current lease epoch; decode accepts historical epochs.
 * Body-only view is explicitly uncommitted. Pair decode binds every duplicate
 * header field and normalized v3 fingerprints, but still proves no durability.
 */
int ppn_bbt_codec_encode_body(const struct ppn_bbt_config *config, u32 slot,
			      const struct ppn_bbt_snapshot *snapshot,
			      u8 *page, size_t page_length);
int ppn_bbt_codec_encode_manifest(const struct ppn_bbt_config *config, u32 slot,
				  const struct ppn_bbt_snapshot *snapshot,
				  u32 body_crc, u32 manifest_crc,
				  u8 *page, size_t page_length);
int ppn_bbt_codec_decode_body(const struct ppn_bbt_config *config, u32 slot,
			      const u8 *page, size_t page_length,
			      struct ppn_bbt_body_view *view);
int ppn_bbt_codec_decode_pair(const struct ppn_bbt_config *config, u32 slot,
			      const u8 *body, size_t body_length,
			      const u8 *manifest, size_t manifest_length,
			      struct ppn_bbt_decoded *decoded);
#endif
