// SPDX-License-Identifier: GPL-2.0-only
/*
 * Read-only: print the layout of the live context superblock, VBA by VBA from
 * VBA 52 on: metadata type, section, weave offset from the base, the two
 * 16-bit fields and the 32-bit field of the metadata, and for tree LBAs the
 * first covered LBA, the second word and the number of runs.  Stops after 8
 * erased VBAs in a row.
 */
#include "sftl.h"
#include "sftl_port.h"
#include "ctxdump.h"

int ctxdump_run(const struct sftl_ops *ops, void *hw)
{
	struct sftl *fs = sftl_alloc(sizeof(*fs));
	u32 o, cap, sb, erased = 0, after = 0xffffffffu;
	u64 base, next_first = ~0ull;
	int ret;

	if (!fs)
		return -ENOMEM;
	ret = sftl_init(fs, ops, hw);
	if (!ret)
		ret = sftl_mount(fs);
	if (ret)
		goto out;
	for (o = 0; o < fs->n_ctx; o++)
		sftl_info("ctxdump: context candidate sb %u, first weave %llu, rec type %u",
			  fs->ctx_list[o], (unsigned long long)fs->probe[fs->ctx_list[o]].first_weave,
			  fs->rec[fs->ctx_list[o]].type);
	for (o = 0; o < SFTL_NUM_SB; o++)
		if (fs->rec[o].type == SFTL_SB_CONTEXT)
			sftl_info("ctxdump: SB array lists sb %u as a context", o);
	sb = fs->ctx_sb;
	base = fs->ctx_weave;
	cap = sftl_capacity(fs, sb);
	sftl_info("ctxdump: context sb %u, base weave %llu, width %u, capacity %u, tree %u LBAs, %u runs, rec type %u",
		  sb, (unsigned long long)base, fs->shape[sb].width, cap, fs->tree_used,
		  fs->tree_runs, fs->rec[sb].type);
	/* Base section (VBAs 0..3): every non-zero word. */
	for (o = 0; o < 4; o++) {
		u8 m[SFTL_META_LBA], *d = fs->aux_data;
		u32 k;

		if (sftl_read_vba(fs, sftl_vba(sb, o), d, m))
			continue;
		for (k = 0; k < SFTL_LBA_SIZE; k += 4)
			if (sftl_get32(d + k))
				sftl_info("ctxdump: base lba %u +%u: %08x", o, k, sftl_get32(d + k));
	}
	for (o = 52; o < cap; o++) {
		u8 m[SFTL_META_LBA], *d = fs->aux_data;
		u32 r = 0;

		ret = sftl_read_vba(fs, sftl_vba(sb, o), d, m);
		if (ret) {
			sftl_info("ctxdump: vba %u: read %d", o, ret);
			if (++erased >= 8)
				break;
			continue;
		}
		if (m[0] == 0xff) {
			if (after == 0xffffffffu)
				after = o;
			if (++erased >= 8)
				break;
			continue;
		}
		erased = 0;
		while (r < 511 && sftl_get32(d + 8 + 8 * r) != 0xffffffffu)
			r++;
		sftl_info("ctxdump: vba %u: type %02x sec %u w+%lld f8 %u f10 %u f12 %08x | %08x %08x runs %u",
			  o, m[0], m[1], (long long)(sftl_get48(m + 2) - base),
			  sftl_get16(m + 8), sftl_get16(m + 10), sftl_get32(m + 12),
			  sftl_get32(d), sftl_get32(d + 4), r);
	}
	sftl_info("ctxdump: first erased VBA %u", after);
	for (o = 0; o < SFTL_NUM_SB; o++)
		if (fs->probe[o].first_weave > base && fs->probe[o].first_weave < next_first &&
		    fs->probe[o].first_type != SFTL_MT_ERASED)
			next_first = fs->probe[o].first_weave;
	sftl_info("ctxdump: lowest first weave above the base: base + %lld",
		  (long long)(next_first - base));
	ret = 0;
out:
	sftl_writer_stop(fs);
	sftl_exit(fs);
	sftl_free(fs);
	return ret;
}
