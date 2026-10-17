// SPDX-License-Identifier: GPL-2.0-only
#include "ppn_cdma_plan.h"
#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#else
#include <errno.h>
#include <string.h>
#endif
static int ppn_cdma_setting(u32 value, u32 maximum, u32 *encoded)
{
	u32 power = 0;

	if (!value || value > maximum || (value & (value - 1)))
		return -EINVAL;
	while (value > 1) {
		value >>= 1;
		power++;
	}
	*encoded = power;
	return 0;
}

int ppn_cdma_build_plan(const struct ppn_cdma_request *r,
			struct ppn_cdma_plan *p)
{
	u32 i, width, burst, fifo, off, flags;
	u64 total = 0, end, needed;

	if (!r || !p || !r->sg || r->bus > 1 || r->metadata > 1 ||
	    r->direction > 1 || r->selector > 15 || r->start_selector > 15 || !r->count ||
	    r->count > PPN_CDMA_MAX_SG || !r->size || r->size > 0x7fffffff ||
	    (r->chain_dma & 31) ||
	    ppn_cdma_setting(r->word, 4, &width) ||
	    ppn_cdma_setting(r->burst, 32, &burst))
		return -EINVAL;
	needed = ((u64)r->count + 2) * 32;
	end = (u64)r->chain_dma + r->chain_bytes;
	if (r->chain_bytes < needed || end > ((u64)1 << 32))
		return -ERANGE;
	for (i = 0; i < r->count; i++) {
		if (!r->sg[i].length || r->sg[i].length > 0x1000000)
			return -EINVAL;
		end = (u64)r->sg[i].dma_address + r->sg[i].length;
		if (end > ((u64)1 << 32))
			return -ERANGE;
		total += r->sg[i].length;
	}
	if (total != r->size)
		return -EMSGSIZE;
	memset(p, 0, sizeof(*p));
	p->channel = 5 + r->bus * 2 + r->metadata;
	p->cache_bytes = (r->count + 1) * 32;
	flags = 3 | (r->selector << 24);
	for (i = 0; i < r->count; i++) {
		p->chain[i * 8] = r->chain_dma + (i + 1) * 32;
		p->chain[i * 8 + 1] = flags | (i + 1 == r->count ? 0x100 : 0);
		p->chain[i * 8 + 2] = r->sg[i].dma_address;
		p->chain[i * 8 + 3] = r->sg[i].length;
	}
	p->chain[r->count * 8] = r->chain_dma + (r->count + 1) * 32;
	off = p->channel << 12;
	p->register_offsets[0] = off;
	p->register_offsets[1] = off + 4;
	p->register_offsets[2] = off + 8;
	p->register_offsets[3] = off + 12;
	p->register_offsets[4] = off + 20;
	p->register_offsets[5] = off;
	fifo = 0x31200014 + r->bus * 0x100000 + r->metadata * 4;
	p->register_values[0] = 2;
	p->register_values[1] = ((r->bus * 2 + r->metadata) << 16) |
		(width << 2) | (burst << 4) | (r->direction << 1);
	p->register_values[2] = fifo;
	p->register_values[3] = r->size;
	p->register_values[4] = r->chain_dma;
	p->register_values[5] = 0x1c0009 | (r->start_selector << 24);
	return 0;
}
