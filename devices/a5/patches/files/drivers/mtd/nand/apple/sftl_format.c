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

/* Write the tree: returns 0 on success, -ENOSPC if it needs >32 LBAs. */
u32 sftl_tree_serialize(struct sftl *fs, u8 *lbas, u32 *runs_out, u32 *used_out)
{
	u32 lba = 0, lidx = 0, nruns = 0;
	u32 total = 0;

	memset(lbas, 0, SFTL_TREE_LBAS * SFTL_LBA_SIZE);
	while (lba < fs->total_lbas) {
		u8 *img;
		u32 r = 0;

		if (lidx >= SFTL_TREE_LBAS)
			return (u32)-ENOSPC;
		img = lbas + lidx * SFTL_LBA_SIZE;
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
		nruns = r;
	}
	for (; lidx < SFTL_TREE_LBAS; lidx++) {
		u8 *img = lbas + lidx * SFTL_LBA_SIZE;

		if (!total) /* empty map: never happens */
			break;
		sftl_put32(img, 0xffffffffu);
		sftl_put32(img + 4, 0xffffffffu);
		*(u32 *)(img + 8) = 0;
		(void)nruns;
	}
	if (runs_out)
		*runs_out = total;
	if (used_out)
		*used_out = (total + 510) / 511;
	return 0;
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
 * Build the 14 pages of a context copy at superblock sb with base weave
 * weave: 'pages' is 14 * 16 KiB, 'metas' 14 * 64 bytes.  Returns 0 or the
 * tree overflow -ENOSPC.
 */
int sftl_ctx_build(struct sftl *fs, u32 sb, u64 weave, u8 *pages, u8 *metas)
{
	u32 i, lba = 0;
	u32 ret;

	memset(pages, 0, SFTL_CTX_PAGES * SFTL_PAGE_SIZE);
	memset(metas, 0xff, SFTL_CTX_PAGES * SFTL_META_PAGE);

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
		u8 *e = pages + 2 * SFTL_PAGE_SIZE + 8 * i;

		e[0] = fs->rec[i].type;
		e[1] = fs->rec[i].btoc_flags;
		sftl_put16(e + 2, fs->rec[i].valid);
		sftl_put16(e + 4, fs->rec[i].reserved);
		sftl_put16(e + 6, fs->rec[i].erases);
	}
	for (i = 0; i < 4; i++, lba++)
		put_ctx_meta(metas + lba * SFTL_META_LBA, 3, weave + lba,
			     i * 512, section_len(SFTL_NUM_SB, 512, i));

	/* User sequences. */
	for (i = 0; i < SFTL_NUM_SB; i++) {
		u8 *e = pages + 3 * SFTL_PAGE_SIZE + 16 * i;

		sftl_put32(e, fs->useq[i].min);
		sftl_put32(e + 4, fs->useq[i].max);
		sftl_put32(e + 8, fs->useq[i].avg);
		sftl_put32(e + 12, fs->useq[i].count);
	}
	for (i = 0; i < 8; i++, lba++)
		put_ctx_meta(metas + lba * SFTL_META_LBA, 5, weave + lba,
			     i * 256, section_len(SFTL_NUM_SB, 256, i));

	/* Read counts. */
	for (i = 0; i < SFTL_NUM_SB; i++)
		sftl_put32(pages + 5 * SFTL_PAGE_SIZE + 4 * i, fs->reads[i]);
	for (i = 0; i < 4; i++, lba++)
		put_ctx_meta(metas + lba * SFTL_META_LBA, 6, weave + lba,
			     i * 1024, section_len(SFTL_NUM_SB, 1024, i));

	/* Tree. */
	ret = sftl_tree_serialize(fs, pages + 6 * SFTL_PAGE_SIZE, NULL, NULL);
	if (ret)
		return -ENOSPC;
	for (i = 0; i < SFTL_TREE_LBAS; i++, lba++)
		put_ctx_meta(metas + lba * SFTL_META_LBA, 4, weave + lba, 0, 0);
	return 0;
}
