/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef PPN_SEQ_PLAN_H
#define PPN_SEQ_PLAN_H
#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
typedef uint32_t u32;
typedef uint64_t u64;
#endif
#define PPN_SEQ_WORDS 44u
struct ppn_seq_request {
	u32 bus;
	u32 clock_factor; /* hclk /1000, in kHz for this pinned source. */
	u32 timing_weights[2];
	u32 count;
	const u32 *unpatched_words;
};

struct ppn_seq_plan {
	u32 bus;
	u32 clock_factor;
	u32 words[PPN_SEQ_WORDS];
	u32 offsets[PPN_SEQ_WORDS];
};

/* Pure per-controller preparation; no Apple microcode table or upload function.
 * Requires immutable, authenticated target program supplied by caller. Reserved
 * token meanings/timing units/controller applicability remain outside this API.
 * Recompute from the ORIGINAL unpatched source for each controller clock, not
 * from a previously transformed table. Count is exactly44. No output on error.
 * Request/unpatched input and output must not alias. No MMIO or execution occurs.
 */
/* Authenticated Linux HCLK in Hz; target conversion only, not clock discovery.
 * Caller excludes rate changes. Reject zero kHz/truncation; output unchanged
 * on error. No input/output alias or concurrent mutation is permitted.
 */
int ppn_seq_hclk_khz(u64 hclk_hz, u32 *factor);
int ppn_seq_prepare(const struct ppn_seq_request *request,
		    struct ppn_seq_plan *plan);
#endif
