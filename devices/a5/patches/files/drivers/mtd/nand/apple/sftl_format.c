// SPDX-License-Identifier: GPL-2.0-only
/* Serialising on-media structures: BTOC images, the tree, the context. */
#include "sftl.h"
#include "sftl_port.h"

void sftl_btoc_append(struct sftl_btoc_entry *ent, u32 *n, u64 first_weave,
		      u32 lba, u64 weave, u32 user)
{
	u32 off = (u32)(weave - first_weave);

	if (*n && lba != SFTL_TOKEN_DELETE) {
		struct sftl_btoc_entry *last = &ent[*n - 1];
		bool same_run = last->weave_offset + last->count == off;

		/* Data runs continue in LBA order; PAD runs are one entry. */
		if (same_run && lba < SFTL_TOKEN_MIN && last->lba < SFTL_TOKEN_MIN &&
		    last->lba + last->count == lba) {
			last->count++;
			return;
		}
		if (same_run && lba == SFTL_TOKEN_PAD && last->lba == SFTL_TOKEN_PAD &&
		    last->count < SFTL_PAD_RUN_MAX) {
			last->count++;
			return;
		}
	}
	ent[*n].weave_offset = off;
	ent[*n].user = user;
	ent[*n].lba = lba;
	ent[*n].count = 1;
	(*n)++;
}

/*
 * img must hold 2 pages.  force_nb, when non-zero, overrides the choice of 4
 * or 8.  Returns the bytes used (nb LBAs).
 */
u32 sftl_btoc_build(const struct sftl_btoc_entry *ent, u32 n, u8 *img, u32 *nb_out,
		    u32 force_nb)
{
	u32 nb = force_nb ? force_nb : (n + 1 <= SFTL_PAGE_SIZE / 16 ? 4 : 8);
	u32 i;

	memset(img, 0, nb * SFTL_LBA_SIZE);
	for (i = 0; i < n; i++) {
		sftl_put32(img + 16 * i, ent[i].weave_offset);
		sftl_put32(img + 16 * i + 4, ent[i].user);
		sftl_put32(img + 16 * i + 8, ent[i].lba);
		sftl_put32(img + 16 * i + 12, ent[i].count);
	}
	*nb_out = nb;
	return nb * SFTL_LBA_SIZE;
}

/*
 * Serialise the map as tree LBAs into lbas (max_lbas of them, zeroed first):
 * each LBA is its first covered LBA, 0xfffffff0, then up to 511 (start, span)
 * runs, ended by 0xffffffff when not full.  -ENOSPC if max_lbas is too few.
 */
int sftl_tree_serialize(struct sftl *fs, u8 *lbas, u32 max_lbas, u32 *runs_out,
			u32 *used_out)
{
	u32 lba = 0, lidx = 0, total = 0;

	memset(lbas, 0, (size_t)max_lbas * SFTL_LBA_SIZE);
	while (lba < fs->total_lbas) {
		u8 *img;
		u32 r = 0;

		if (lidx >= max_lbas)
			return -ENOSPC;
		img = lbas + (size_t)lidx * SFTL_LBA_SIZE;
		sftl_put32(img, lba);
		sftl_put32(img + 4, 0xfffffff0u);
		while (r < 511 && lba < fs->total_lbas) {
			u32 start = sftl_map_get(&fs->map, lba);
			u32 span = 1;

			if (start == SFTL_UNMAPPED) {
				while (lba + span < fs->total_lbas &&
				       sftl_map_get(&fs->map, lba + span) == SFTL_UNMAPPED)
					span++;
			} else {
				while (lba + span < fs->total_lbas &&
				       sftl_map_get(&fs->map, lba + span) == start + span)
					span++;
			}
			sftl_put32(img + 8 + 8 * r, start);
			sftl_put32(img + 12 + 8 * r, span);
			lba += span;
			r++;
			total++;
		}
		if (r < 511)
			sftl_put32(img + 8 + 8 * r, 0xffffffffu);
		lidx++;
	}
	if (runs_out)
		*runs_out = total;
	if (used_out)
		*used_out = lidx;
	return 0;
}

/*
 * Tree LBAs a context carries for a tree of 'used' LBAs: at least
 * SFTL_TREE_LBAS (32 on the reference package, 16 on 030800P), else used
 * rounded up to 16, with no empty LBA when used is already a multiple.  iOS
 * contexts seen: 26 used in 32; 172, 173, 174 and 176 used, all in 176; on
 * 030800P, 8 used in 16.
 */
u32 sftl_ctx_tree_lbas(u32 used)
{
	u32 n = (used + 15) & ~15u;

	return n < SFTL_TREE_LBAS ? SFTL_TREE_LBAS : n;
}

static void put_ctx_meta(u8 *m, u8 section, u64 weave, u32 off, u32 len)
{
	memset(m, 0, SFTL_META_LBA);
	m[0] = SFTL_MT_CONTEXT;
	m[1] = section;
	sftl_put48(m + 2, weave);
	sftl_put16(m + 8, (u16)off);
	sftl_put16(m + 10, (u16)len);
	sftl_put32(m + 12, 0xffffffffu);
}

static u32 section_len(u32 total, u32 per_lba, u32 idx)
{
	u64 done = (u64)idx * per_lba;

	if (done >= total)
		return 0;
	return total - done < per_lba ? total - (u32)done : per_lba;
}

/*
 * Build a context copy at superblock sb with base weave 'weave': 'pages' holds
 * SFTL_CTX_MAX_PAGES * 16 KiB, 'metas' SFTL_CTX_MAX_PAGES * 64 bytes.  On
 * return *nlbas_out is the number of LBAs to program (a multiple of 4).
 * -ENOSPC when the tree needs more than SFTL_TREE_MAX_LBAS.
 */
int sftl_ctx_build(struct sftl *fs, u32 sb, u64 weave, u8 *pages, u8 *metas,
		   u32 *nlbas_out)
{
	u32 i, lba = 0, used = 0, ntree;
	u8 *tree = pages + (size_t)SFTL_CTX_TREE_VBA * SFTL_LBA_SIZE;
	const u32 *sec = sftl_geo.sec_lbas;
	u8 *sbarr = pages + (size_t)(sec[0] + sec[1]) * SFTL_LBA_SIZE;
	u8 *useq = sbarr + (size_t)sec[2] * SFTL_LBA_SIZE;
	u8 *reads = useq + (size_t)sec[3] * SFTL_LBA_SIZE;
	int ret;

	memset(pages, 0, SFTL_CTX_MAX_PAGES * SFTL_PAGE_SIZE);
	memset(metas, 0xff, SFTL_CTX_MAX_PAGES * SFTL_META_PAGE);
	/* Base. */
	sftl_put32(pages, 1);
	sftl_put32(pages + 4, sb);
	sftl_put32(pages + SFTL_PAGE_SIZE - 4, SFTL_NUM_SB);
	for (i = 0; i < 4; i++, lba++)
		put_ctx_meta(metas + lba * SFTL_META_LBA, 1, weave + lba, 0, 0);

	/* Stats. */
	memcpy(pages + SFTL_PAGE_SIZE, fs->stats, SFTL_PAGE_SIZE);
	for (i = 0; i < 4; i++, lba++)
		put_ctx_meta(metas + lba * SFTL_META_LBA, 2, weave + lba, 0, 0);

	/* SB array. */
	for (i = 0; i < SFTL_NUM_SB; i++) {
		u8 *e = sbarr + 8 * i;

		e[0] = fs->rec[i].type;
		e[1] = fs->rec[i].btoc_flags;
		sftl_put16(e + 2, fs->rec[i].valid);
		sftl_put16(e + 4, fs->rec[i].reserved);
		sftl_put16(e + 6, fs->rec[i].erases);
	}
	for (i = 0; i < sec[2]; i++, lba++)
		put_ctx_meta(metas + lba * SFTL_META_LBA, 3, weave + lba,
			     i * 512, section_len(SFTL_NUM_SB, 512, i));

	/* User sequences. */
	for (i = 0; i < SFTL_NUM_SB; i++) {
		u8 *e = useq + 16 * i;

		sftl_put32(e, fs->useq[i].min);
		sftl_put32(e + 4, fs->useq[i].max);
		sftl_put32(e + 8, fs->useq[i].avg);
		sftl_put32(e + 12, fs->useq[i].count);
	}
	for (i = 0; i < sec[3]; i++, lba++)
		put_ctx_meta(metas + lba * SFTL_META_LBA, 5, weave + lba,
			     i * 256, section_len(SFTL_NUM_SB, 256, i));

	/* Read counts. */
	for (i = 0; i < SFTL_NUM_SB; i++)
		sftl_put32(reads + 4 * i, fs->reads[i]);
	for (i = 0; i < sec[4]; i++, lba++)
		put_ctx_meta(metas + lba * SFTL_META_LBA, 6, weave + lba,
			     i * 1024, section_len(SFTL_NUM_SB, 1024, i));

	/* Tree, then empty tree LBAs up to the count the layout asks for. */
	ret = sftl_tree_serialize(fs, tree, SFTL_TREE_MAX_LBAS, NULL, &used);
	if (ret)
		return ret;
	ntree = sftl_ctx_tree_lbas(used);
	for (i = used; i < ntree; i++) {
		u8 *img = tree + (size_t)i * SFTL_LBA_SIZE;

		sftl_put32(img, 0xffffffffu);
		sftl_put32(img + 4, 0xffffffffu);
	}
	for (i = 0; i < ntree; i++, lba++)
		put_ctx_meta(metas + lba * SFTL_META_LBA, 4, weave + lba, 0, 0);
	*nlbas_out = lba;
	return 0;
}
