// SPDX-License-Identifier: GPL-2.0-only
#include "ppn_seq_plan.h"
#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/math64.h>
#else
#include <errno.h>
#endif
/* Original AP stores hclk/1000, low32. Refuse zero/truncating factors. */
int ppn_seq_hclk_khz(u64 hclk_hz, u32 *factor)
{
	u64 khz;

	if (!factor || hclk_hz < 1000)
		return -EINVAL;
#ifdef __KERNEL__
	khz = div_u64(hclk_hz, 1000);
#else
	khz = hclk_hz / 1000;
#endif
	if (khz > 0xffffffff)
		return -ERANGE;
	*factor = (u32)khz;
	return 0;
}

int ppn_seq_prepare(const struct ppn_seq_request *r, struct ppn_seq_plan *p)
{
	u64 products[2];
	u32 i, word;

	if (!r || !p || !r->unpatched_words || r->count != PPN_SEQ_WORDS ||
	    r->bus > 1 || !r->clock_factor)
		return -EINVAL;
	products[0] = (u64)r->clock_factor * r->timing_weights[0];
	products[1] = (u64)r->clock_factor * r->timing_weights[1];
	/* Original MUL wraps32 before scaling. Reject that unproven timing instead. */
	if (products[0] > 0xffffffff || products[1] > 0xffffffff)
		return -ERANGE;
	p->bus = r->bus;
	p->clock_factor = r->clock_factor;
	for (i = 0; i < PPN_SEQ_WORDS; i++) {
		word = r->unpatched_words[i];
		if (word == 0x0e000000 || word == 0x0e000001)
			word = 0x0e000000 |
			       (u32)((products[word & 1] * 0x431bde83) >> 50);
		p->words[i] = word;
		p->offsets[i] = 0xc1000 + i * 4;
	}
	return 0;
}
