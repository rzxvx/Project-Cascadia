// SPDX-License-Identifier: GPL-2.0-only
/*
 * Swiss FTL mount: VFL context, context superblock, tree, replay.
 * All of this is read-only; the writer is in sftl_write.c.
 */
#include "sftl.h"
#include "sftl_port.h"

#define VFL_BLOCK_A		1062
#define VFL_BLOCK_B		1063
#define VFL_HDR_SECTION		0xffff
#define VFL_MAX_LOG		256
#define CTX_SECTIONS		6

static const struct {
	u8 flag;
	u8 lbas;
} ctx_layout[CTX_SECTIONS] = {
	{ 1, 4 }, { 2, 4 }, { 3, 4 }, { 5, 8 }, { 6, 4 }, { 4, 32 },
};

int sftl_init(struct sftl *fs, const struct sftl_ops *ops, void *hw)
{
	unsigned int i;

	memset(fs, 0, sizeof(*fs));
	fs->ops = ops;
	fs->hw = hw;
	for (i = 0; i < SFTL_BANKS; i++) {
		fs->usage[i] = sftl_alloc(SFTL_BLOCKS * sizeof(u16));
		if (!fs->usage[i])
			return -ENOMEM;
	}
	fs->rec = sftl_alloc(SFTL_NUM_SB * sizeof(*fs->rec));
	fs->useq = sftl_alloc(SFTL_NUM_SB * sizeof(*fs->useq));
	fs->reads = sftl_alloc(SFTL_NUM_SB * sizeof(*fs->reads));
	fs->stats = sftl_alloc(SFTL_PAGE_SIZE);
	fs->io_data = sftl_alloc(SFTL_PAGE_SIZE);
	fs->io_meta = sftl_alloc(SFTL_META_PAGE);
	fs->cache_data = sftl_alloc(SFTL_PAGE_SIZE);
	fs->aux_data = sftl_alloc(SFTL_PAGE_SIZE);
	if (!fs->aux_data)
		return -ENOMEM;
	if (!fs->rec || !fs->useq || !fs->reads || !fs->stats || !fs->io_data ||
	    !fs->io_meta || !fs->cache_data)
		return -ENOMEM;
	return 0;
}

void sftl_exit(struct sftl *fs)
{
	unsigned int i;

	for (i = 0; i < SFTL_BANKS; i++)
		sftl_free(fs->usage[i]);
	sftl_free(fs->rec);
	sftl_free(fs->useq);
	sftl_free(fs->reads);
	sftl_free(fs->stats);
	sftl_free(fs->io_data);
	sftl_free(fs->io_meta);
	sftl_free(fs->cache_data);
	sftl_free(fs->aux_data);
	sftl_map_free(&fs->map);
}

static bool status_ok(u8 st)
{
	return st == SFTL_ST_OK || st == SFTL_ST_REFRESH;
}

static int read_page(struct sftl *fs, u32 bank, u32 block, u32 page, bool slc,
		     u8 *data, u8 *meta, u8 *st)
{
	return fs->ops->read_page(fs->hw, bank, block, page, slc, data, meta, st);
}

/* The VFL log of one bank: position -> (block, page). */
struct vfl_entry {
	bool present;
	u16 section;
	u8 block_b;
	u8 page;
};

static int vfl_scan_log(struct sftl *fs, u32 bank, struct vfl_entry *log,
			u32 *max_hdr, u32 *pages_b)
{
	int have_header = 0;
	u32 k;
	u32 blk;

	*max_hdr = 0;
	*pages_b = 0;
	for (blk = 0; blk < 2; blk++) {
		u32 block = blk ? VFL_BLOCK_B : VFL_BLOCK_A;

		for (k = 0; k < SFTL_SLC_PAGES; k++) {
			u8 st;
			int ret = read_page(fs, bank, block, k, true,
					    fs->io_data, fs->io_meta, &st);
			u32 pos, section;

			if (ret)
				return ret;
			if (st == SFTL_ST_ERASED)
				break;
			if (!status_ok(st) || fs->io_meta[0] != SFTL_MT_VFL)
				return -EIO;
			pos = ~sftl_get32(fs->io_meta + 4);
			section = sftl_get16(fs->io_meta + 1);
			if (pos >= VFL_MAX_LOG)
				return -EIO;
			log[pos].present = true;
			log[pos].section = (u16)section;
			log[pos].block_b = (u8)blk;
			log[pos].page = (u8)k;
			if (section == VFL_HDR_SECTION &&
			    (!have_header || pos > *max_hdr)) {
				*max_hdr = pos;
				have_header = 1;
			}
			if (blk)
				*pages_b = k + 1;
		}
	}
	return have_header ? 0 : -ENOENT;
}

static int vfl_read_tables(struct sftl *fs, u32 bank, struct vfl_entry *log,
			   u32 hdr_pos)
{
	u32 positions[SFTL_BANKS];
	u32 n;
	u8 st;
	int ret;

	ret = read_page(fs, bank,
			log[hdr_pos].block_b ? VFL_BLOCK_B : VFL_BLOCK_A,
			log[hdr_pos].page, true, fs->io_data, fs->io_meta, &st);
	if (ret)
		return ret;
	if (!status_ok(st))
		return -EIO;
	if (sftl_get32(fs->io_data) != 0xffffffffu ||
	    sftl_get16(fs->io_data + 12) != SFTL_NUM_SB)
		return -EIO;
	fs->reported_lbas = sftl_get32(fs->io_data + 4);
	for (n = 0; n < SFTL_BANKS; n++)
		positions[n] = sftl_get32(fs->io_data + 0x200 + 32 * n);

	for (n = 0; n < SFTL_BANKS; n++) {
		u32 pos = positions[n];
		u32 b;

		if (pos >= VFL_MAX_LOG || !log[pos].present ||
		    log[pos].section != n)
			return -EIO;
		ret = read_page(fs, bank,
				log[pos].block_b ? VFL_BLOCK_B : VFL_BLOCK_A,
				log[pos].page, true, fs->io_data, fs->io_meta,
				&st);
		if (ret)
			return ret;
		if (!status_ok(st))
			return -EIO;
		for (b = 0; b < SFTL_BLOCKS; b++)
			fs->usage[n][b] = sftl_get16(fs->io_data + 2 * b);
	}
	return 0;
}

int sftl_load_vfl(struct sftl *fs)
{
	struct vfl_entry *log;
	u32 bank, n, sb;
	int ret = -ENOENT;

	log = sftl_alloc(VFL_MAX_LOG * sizeof(*log));
	if (!log)
		return -ENOMEM;
	for (bank = 0; bank < SFTL_BANKS; bank++) {
		u32 hdr, pages_b;

		memset(log, 0, VFL_MAX_LOG * sizeof(*log));
		ret = vfl_scan_log(fs, bank, log, &hdr, &pages_b);
		if (ret)
			continue;
		ret = vfl_read_tables(fs, bank, log, hdr);
		if (ret)
			continue;
		fs->vfl_block = pages_b ? VFL_BLOCK_B : VFL_BLOCK_A;
		fs->vfl_log_pages = pages_b;
		break;
	}
	sftl_free(log);
	if (ret)
		return ret;

	fs->sig_bank = SFTL_BANKS;
	for (n = 0; n < SFTL_BANKS; n++) {
		u32 b;

		for (b = 0; b < SFTL_BLOCKS; b++) {
			if (fs->usage[n][b] == 0xc101) {
				fs->sig_bank = n;
				fs->sig_block = b;
			}
		}
	}
	for (sb = 0; sb < SFTL_NUM_SB; sb++) {
		struct sftl_sb_shape *sh = &fs->shape[sb];

		sh->width = 0;
		for (n = 0; n < SFTL_BANKS; n++)
			if (fs->usage[n][sb] == 0)
				sh->bank[sh->width++] = (u8)n;
	}
	return 0;
}

/* First VBA of every superblock; collects the context candidates. */
int sftl_scan(struct sftl *fs)
{
	u32 sb;

	fs->n_ctx = 0;
	for (sb = 0; sb < SFTL_NUM_SB; sb++) {
		struct sftl_sb_probe *pr = &fs->probe[sb];
		u8 st;
		int ret;

		pr->first_type = SFTL_MT_ERASED;
		pr->first_weave = 0;
		pr->flags = 0;
		if (!fs->shape[sb].width)
			continue;
		ret = read_page(fs, fs->shape[sb].bank[0], sb, 0, false,
				fs->io_data, fs->io_meta, &st);
		if (ret)
			return ret;
		if (st == SFTL_ST_ERASED)
			continue;
		if (!status_ok(st)) {
			sftl_warn("sb %u first page status %#x", sb, st);
			pr->first_type = 0;
			continue;
		}
		pr->first_type = fs->io_meta[0];
		pr->flags = fs->io_meta[1];
		pr->first_weave = sftl_get48(fs->io_meta + 2);
		if (pr->first_type == SFTL_MT_CONTEXT && pr->flags == 1 &&
		    fs->n_ctx < SFTL_MAX_CTX)
			fs->ctx_list[fs->n_ctx++] = sb;
	}
	return 0;
}

static int tree_ingest(struct sftl *fs, const u8 *lba_img, u32 *cursor,
		       u32 *runs)
{
	u32 pos = sftl_get32(lba_img);
	u32 i;

	if (sftl_get32(lba_img + 4) != 0xfffffff0u || pos != *cursor)
		return -EIO;
	for (i = 0; i < 511; i++) {
		u32 vba = sftl_get32(lba_img + 8 + 8 * i);
		u32 span = sftl_get32(lba_img + 12 + 8 * i);
		u32 j;

		if (vba == 0xffffffffu)
			break;
		if (span == 0 || *cursor + span > fs->total_lbas)
			return -EIO;
		for (j = 0; j < span; j++) {
			int ret = sftl_map_set(&fs->map, *cursor + j,
					       vba == SFTL_UNMAPPED ?
					       SFTL_UNMAPPED : vba + j);
			if (ret)
				return ret;
		}
		*cursor += span;
		(*runs)++;
	}
	return 0;
}

/* Read one context copy; on success its map and SB tables are loaded. */
static int context_try(struct sftl *fs, u32 sb)
{
	u32 cursor = 0, runs = 0, used = 0;
	u32 i;
	u64 base = fs->probe[sb].first_weave;
	u32 sec = 0, sec_idx = 0;
	u8 *pagebuf;
	int ret;

	pagebuf = sftl_alloc(SFTL_CTX_PAGES * SFTL_PAGE_SIZE +
			     SFTL_CTX_PAGES * SFTL_META_PAGE);
	if (!pagebuf)
		return -ENOMEM;

	for (i = 0; i < SFTL_CTX_PAGES; i++) {
		u32 row = i;
		u32 w = fs->shape[sb].width;
		u8 st;

		ret = read_page(fs, fs->shape[sb].bank[row % w], sb, row / w,
				false, pagebuf + i * SFTL_PAGE_SIZE,
				pagebuf + SFTL_CTX_PAGES * SFTL_PAGE_SIZE +
				i * SFTL_META_PAGE, &st);
		if (ret)
			goto out;
		if (!status_ok(st)) {
			ret = -EIO;
			goto out;
		}
	}

	for (i = 0; i < SFTL_CTX_LBAS; i++) {
		const u8 *meta = pagebuf + SFTL_CTX_PAGES * SFTL_PAGE_SIZE +
				 (i / 4) * SFTL_META_PAGE + (i % 4) * SFTL_META_LBA;

		if (sec_idx == ctx_layout[sec].lbas) {
			sec++;
			sec_idx = 0;
		}
		if (meta[0] != SFTL_MT_CONTEXT || meta[1] != ctx_layout[sec].flag ||
		    sftl_get48(meta + 2) != base + i) {
			ret = -EIO;
			goto out;
		}
		sec_idx++;
	}

	/* Stats section is LBAs 4..7 = page 1. */
	memcpy(fs->stats, pagebuf + 1 * SFTL_PAGE_SIZE, SFTL_PAGE_SIZE);

	/* SB array, page 2. */
	{
		const u8 *p = pagebuf + 2 * SFTL_PAGE_SIZE;

		for (i = 0; i < SFTL_NUM_SB; i++) {
			const u8 *e = p + 8 * i;

			fs->rec[i].type = e[0];
			fs->rec[i].btoc_flags = e[1];
			fs->rec[i].valid = sftl_get16(e + 2);
			fs->rec[i].reserved = sftl_get16(e + 4);
			fs->rec[i].erases = sftl_get16(e + 6);
		}
		/* User sequences, pages 3-4. */
		p = pagebuf + 3 * SFTL_PAGE_SIZE;
		for (i = 0; i < SFTL_NUM_SB; i++) {
			fs->useq[i].min = sftl_get32(p + 16 * i);
			fs->useq[i].max = sftl_get32(p + 16 * i + 4);
			fs->useq[i].avg = sftl_get32(p + 16 * i + 8);
			fs->useq[i].count = sftl_get32(p + 16 * i + 12);
		}
		/* Read counts, page 5. */
		p = pagebuf + 5 * SFTL_PAGE_SIZE;
		for (i = 0; i < SFTL_NUM_SB; i++)
			fs->reads[i] = sftl_get32(p + 4 * i);
	}

	/*
	 * Tree: from page 6, 32 LBAs in the contexts iOS writes, and for as long
	 * as the metadata continues the section in contexts with a longer one.
	 */
	sftl_map_free(&fs->map);
	fs->total_lbas = SFTL_TOTAL_LBAS;
	ret = sftl_map_init(&fs->map, fs->total_lbas);
	if (ret)
		goto out;
	{
		u32 t, limit = sftl_capacity(fs, sb) - SFTL_CTX_TREE_VBA;
		bool ended = false;

		for (t = 0; t < limit; t++) {
			const u8 *img;

			if (t < SFTL_TREE_LBAS) {
				img = pagebuf + 6 * SFTL_PAGE_SIZE + t * SFTL_LBA_SIZE;
			} else {
				u32 o = 24 + t, bank, page, slot;
				u8 st;
				const u8 *m;

				if (!sftl_locate(fs, sftl_vba(sb, o), &bank, &page, &slot))
					break;
				if (slot == 0) {
					ret = read_page(fs, bank, sb, page, false, fs->io_data,
							fs->io_meta, &st);
					if (ret)
						goto out;
					if (!status_ok(st))
						break;
				}
				m = fs->io_meta + slot * SFTL_META_LBA;
				if (m[0] != SFTL_MT_CONTEXT || m[1] != 4 ||
				    sftl_get48(m + 2) != base + o)
					break;
				img = fs->io_data + slot * SFTL_LBA_SIZE;
			}
			/* Empty tree LBAs end the tree but still belong to the context. */
			if (sftl_get32(img) == 0xffffffffu &&
			    sftl_get32(img + 4) == 0xffffffffu)
				ended = true;
			if (ended)
				continue;
			ret = tree_ingest(fs, img, &cursor, &runs);
			if (ret)
				goto out;
			used++;
		}
		fs->ctx_lbas = SFTL_CTX_TREE_VBA + t;
	}
	if (used > SFTL_TREE_LBAS)
		sftl_warn("context sb %u: tree of %u LBAs, longer than the usual %u",
			  sb, used, SFTL_TREE_LBAS);
	if (cursor != fs->total_lbas)
		sftl_info("context sb %u: tree covers %u LBAs of %u", sb, cursor, fs->total_lbas);

	fs->ctx_sb = sb;
	fs->ctx_weave = base;
	fs->tree_runs = runs;
	fs->tree_used = used;
	fs->mapped_before = sftl_map_count_mapped(&fs->map, fs->total_lbas);
	ret = 0;
out:
	sftl_free(pagebuf);
	return ret;
}

int sftl_load_context(struct sftl *fs)
{
	u32 order[SFTL_MAX_CTX];
	unsigned int i, j, n = fs->n_ctx;

	for (i = 0; i < n; i++)
		order[i] = fs->ctx_list[i];
	/* Newest base weave first. */
	for (i = 0; i < n; i++)
		for (j = i + 1; j < n; j++)
			if (fs->probe[order[j]].first_weave >
			    fs->probe[order[i]].first_weave) {
				u32 t = order[i];

				order[i] = order[j];
				order[j] = t;
			}
	for (i = 0; i < n; i++) {
		int ret = context_try(fs, order[i]);

		if (!ret)
			return 0;
		sftl_warn("context sb %u unusable (%d), trying older", order[i],
			  ret);
	}
	return -ENOENT;
}

bool sftl_sb_closed(struct sftl *fs, u32 sb, u8 *last_meta)
{
	u32 cap = sftl_capacity(fs, sb);
	u32 bank, page, slot;
	u8 st;

	if (!cap || !sftl_locate(fs, sftl_vba(sb, cap - 1), &bank, &page, &slot))
		return false;
	if (read_page(fs, bank, sb, page, false, fs->io_data, fs->io_meta, &st))
		return false;
	if (!status_ok(st) || fs->io_meta[slot * SFTL_META_LBA] != SFTL_MT_BTOC)
		return false;
	if (last_meta)
		memcpy(last_meta, fs->io_meta + slot * SFTL_META_LBA, SFTL_META_LBA);
	return true;
}

/* Read a BTOC image of nb LBAs into img (at most 2 pages). */
int sftl_read_btoc(struct sftl *fs, u32 sb, u32 *nb, u8 *img)
{
	u8 meta[SFTL_META_LBA];
	u32 cap = sftl_capacity(fs, sb);
	u32 i;

	if (!sftl_sb_closed(fs, sb, meta))
		return -ENOENT;
	*nb = sftl_get16(meta + 12);
	if (*nb != 4 && *nb != 8)
		return -EIO;
	for (i = 0; i < *nb; i += SFTL_LBAS_PER_PAGE) {
		u32 bank, page, slot;
		u8 st;
		int ret;

		if (!sftl_locate(fs, sftl_vba(sb, cap - *nb + i), &bank, &page, &slot))
			return -EIO;
		ret = read_page(fs, bank, sb, page, false,
				img + i * SFTL_LBA_SIZE, fs->io_meta, &st);
		if (ret)
			return ret;
		if (!status_ok(st))
			return -EIO;
	}
	return 0;
}

void sftl_summarize_useq(const u8 *btoc, u32 entries, struct sftl_useq *out)
{
	u64 sum = 0;
	u32 i, mn = 0xffffffffu, mx = 0;

	for (i = 0; i < entries; i++) {
		u32 u = sftl_get32(btoc + 16 * i + 4);

		if (u < mn)
			mn = u;
		if (u > mx)
			mx = u;
		sum += u;
	}
	if (entries == 0) {
		out->min = out->max = out->avg = 0;
	} else {
		u64 avg = sum;

#ifdef __KERNEL__
		do_div(avg, entries);
#else
		avg /= entries;
#endif
		out->min = mn;
		out->max = mx;
		out->avg = (u32)avg;
	}
	out->count = entries;
}

/*
 * Replay of a single mapping.  Replay is applied in weave order per LBA via
 * rp_weave[]: a later (higher-weave) write of an LBA wins whatever the order
 * in which superblocks are walked.
 */
struct replay_state {
	u64 min_weave;		/* apply only weaves above this */
	struct sftl_map guard;	/* lba -> weave - min_weave (0 = none) */
};

static bool replay_newer(struct replay_state *rs, u32 lba, u64 weave)
{
	u32 cur = sftl_map_get(&rs->guard, lba);
	u32 off;

	if (weave <= rs->min_weave)
		return false;
	off = (u32)(weave - rs->min_weave);
	if (cur != SFTL_UNMAPPED && cur >= off)
		return false;
	return sftl_map_set(&rs->guard, lba, off) == 0;
}

static void note_weave(struct sftl *fs, u64 weave)
{
	if (weave >= fs->next_weave)
		fs->next_weave = weave + 1;
}

static int apply_data(struct sftl *fs, struct replay_state *rs, u32 lba,
		      u64 weave, u32 vba)
{
	if (lba >= fs->total_lbas)
		return -EIO;
	note_weave(fs, weave);
	if (!replay_newer(rs, lba, weave))
		return 0;
	return sftl_map_set(&fs->map, lba, vba);
}

/* A DELETE token's payload: u32 n, then n (lba, count). */
static int apply_delete(struct sftl *fs, struct replay_state *rs, u32 vba,
			u64 weave)
{
	u32 bank, page, slot;
	u8 st;
	int ret;
	u8 *payload;
	u32 i, n;

	if (!sftl_locate(fs, vba, &bank, &page, &slot))
		return -EIO;
	ret = read_page(fs, bank, sftl_sb_of(vba), page, false,
			fs->aux_data, fs->aux_meta, &st);
	if (ret)
		return ret;
	if (!status_ok(st))
		return -EIO;
	payload = fs->aux_data + slot * SFTL_LBA_SIZE;
	n = sftl_get32(payload);
	if (n > (SFTL_LBA_SIZE - 4) / 8)
		return -EIO;
	note_weave(fs, weave);
	for (i = 0; i < n; i++) {
		u32 lba = sftl_get32(payload + 4 + 8 * i);
		u32 cnt = sftl_get32(payload + 8 + 8 * i);
		u32 j;

		if (lba >= fs->total_lbas || cnt > fs->total_lbas - lba)
			return -EIO;
		for (j = 0; j < cnt; j++) {
			if (!replay_newer(rs, lba + j, weave))
				continue;
			ret = sftl_map_set(&fs->map, lba + j, SFTL_UNMAPPED);
			if (ret)
				return ret;
		}
	}
	return 0;
}

static int replay_sb_btoc(struct sftl *fs, struct replay_state *rs, u32 sb,
			  u32 *applied)
{
	u8 *btoc;
	u32 nb, i, pos = 0;
	u64 first = fs->probe[sb].first_weave;
	int ret;

	btoc = sftl_alloc(2 * SFTL_PAGE_SIZE);
	if (!btoc)
		return -ENOMEM;
	ret = sftl_read_btoc(fs, sb, &nb, btoc);
	if (ret)
		goto out;
	{
		u8 meta[SFTL_META_LBA];

		/* max_weave_add may include the BTOC itself (iOS's "+nb"). */
		if (sftl_sb_closed(fs, sb, meta))
			note_weave(fs, first + sftl_get32(meta + 8) - 1);
	}
	for (i = 0; i < nb * SFTL_LBA_SIZE / 16; i++) {
		const u8 *e = btoc + 16 * i;
		u32 woff = sftl_get32(e);
		u32 lba = sftl_get32(e + 8);
		u32 cnt = sftl_get32(e + 12);
		u32 j;

		if (cnt == 0)
			break;
		if (lba >= SFTL_TOKEN_MIN) {
			if (lba == SFTL_TOKEN_DELETE) {
				if (cnt != 1) {
					ret = -EIO;
					goto out;
				}
				ret = apply_delete(fs, rs, sftl_vba(sb, pos),
						   first + woff);
				if (ret)
					goto out;
			} else {
				/* PAD: a run of VBAs that carry nothing. */
				note_weave(fs, first + woff + cnt - 1);
			}
			pos += cnt;
			continue;
		}
		for (j = 0; j < cnt; j++) {
			ret = apply_data(fs, rs, lba + j, first + woff + j,
					 sftl_vba(sb, pos + j));
			if (ret)
				goto out;
		}
		pos += cnt;
		(*applied) += cnt;
	}
	if (pos > sftl_capacity(fs, sb) - nb)
		ret = -EIO;
out:
	sftl_free(btoc);
	return ret;
}

static int replay_sb_walk(struct sftl *fs, struct replay_state *rs, u32 sb,
			  u32 *applied)
{
	u32 cap = sftl_capacity(fs, sb);
	u32 o;

	for (o = 0; o < cap; o += SFTL_LBAS_PER_PAGE) {
		u32 bank, page, slot, i;
		u8 st;
		int ret;

		if (!sftl_locate(fs, sftl_vba(sb, o), &bank, &page, &slot))
			return -EIO;
		ret = read_page(fs, bank, sb, page, false, fs->io_data,
				fs->io_meta, &st);
		if (ret)
			return ret;
		if (st == SFTL_ST_ERASED)
			return 0;
		if (!status_ok(st)) {
			/*
			 * Spec 4.9: a program that failed leaves a page that is
			 * not taken; what it held was written again elsewhere
			 * with a higher weave, or never acknowledged.
			 */
			sftl_warn("replay walk sb %u offset %u: status %#x, page skipped",
				  sb, o, st);
			fs->torn[sb / 8] |= 1 << (sb % 8);
			continue;
		}
		if (fs->io_meta[0] == SFTL_MT_BTOC)
			return 0;
		for (i = 0; i < SFTL_LBAS_PER_PAGE; i++) {
			const u8 *m = fs->io_meta + i * SFTL_META_LBA;
			u64 weave = sftl_get48(m + 2);
			u32 lba = sftl_get32(m + 8);

			if (m[0] != SFTL_MT_USER && m[0] != SFTL_MT_INTERNAL)
				return 0;
			note_weave(fs, weave);
			if (weave <= rs->min_weave)
				continue;
			(*applied)++;
			if (lba == SFTL_TOKEN_PAD)
				continue;
			if (lba == SFTL_TOKEN_DELETE) {
				ret = apply_delete(fs, rs, sftl_vba(sb, o + i), weave);
				if (ret)
					return ret;
				continue;
			}
			if (lba >= fs->total_lbas) {
				sftl_err("replay walk sb %u offset %u: lba %#x type %#x weave %llu",
					 sb, o + i, lba, m[0], (unsigned long long)weave);
				return -EIO;
			}
			ret = apply_data(fs, rs, lba, weave, sftl_vba(sb, o + i));
			if (ret)
				return ret;
		}
	}
	return 0;
}

int sftl_replay(struct sftl *fs)
{
	u32 *set;
	u64 *weaves;
	struct replay_state rs;
	u32 sb, n = 0, i, j;
	int ret;

	rs.min_weave = fs->ctx_weave;
	memset(fs->torn, 0, sizeof(fs->torn));
	ret = sftl_map_init(&rs.guard, fs->total_lbas);
	if (ret)
		return ret;
	set = sftl_alloc(SFTL_NUM_SB * sizeof(*set));
	weaves = sftl_alloc(SFTL_NUM_SB * sizeof(*weaves));
	if (!set || !weaves) {
		ret = -ENOMEM;
		goto out;
	}

	note_weave(fs, fs->ctx_weave + fs->ctx_lbas - 1);
	for (sb = 0; sb < SFTL_NUM_SB; sb++) {
		const struct sftl_sb_probe *pr = &fs->probe[sb];
		u8 t = fs->rec[sb].type;
		bool fresh = pr->first_type != SFTL_MT_ERASED &&
			     pr->first_type != SFTL_MT_CONTEXT &&
			     pr->first_type != 0 &&
			     pr->first_weave > fs->ctx_weave;
		bool open_rec = (t == SFTL_SB_OPEN || t == SFTL_SB_OPEN_ALT) &&
				pr->first_type != SFTL_MT_ERASED &&
				pr->first_type != SFTL_MT_CONTEXT;

		note_weave(fs, pr->first_weave);
		if (fresh || open_rec) {
			set[n] = sb;
			weaves[n] = pr->first_weave;
			n++;
		}
	}

	/* Ascending first weave. */
	for (i = 1; i < n; i++) {
		for (j = i; j > 0 && weaves[j - 1] > weaves[j]; j--) {
			u64 tw = weaves[j];
			u32 ts = set[j];

			weaves[j] = weaves[j - 1];
			set[j] = set[j - 1];
			weaves[j - 1] = tw;
			set[j - 1] = ts;
		}
	}

	fs->n_replay = 0;
	fs->replay_applied = 0;
	for (i = 0; i < n; i++) {
		u32 s = set[i];
		const struct sftl_sb_probe *pr = &fs->probe[s];
		bool fresh = pr->first_weave > fs->ctx_weave;
		bool closed = sftl_sb_closed(fs, s, NULL);
		u32 applied = 0;

		if (closed && fresh)
			ret = replay_sb_btoc(fs, &rs, s, &applied);
		else
			ret = replay_sb_walk(fs, &rs, s, &applied);
		if (ret) {
			sftl_err("replay of sb %u failed: %d", s, ret);
			goto out;
		}
		if (fs->n_replay < SFTL_REPLAY_LOG) {
			fs->replay_log[fs->n_replay].sb = s;
			fs->replay_log[fs->n_replay].closed = closed;
			fs->replay_log[fs->n_replay].applied = applied;
			fs->n_replay++;
		}
		fs->replay_applied += applied;
		if (applied)
			fs->replayed = true;
	}
	ret = 0;
out:
	sftl_map_free(&rs.guard);
	sftl_free(set);
	sftl_free(weaves);
	return ret;
}

int sftl_mount(struct sftl *fs)
{
	int ret;

	ret = sftl_load_vfl(fs);
	if (ret) {
		sftl_err("no usable VFL context (%d)", ret);
		return ret;
	}
	ret = sftl_scan(fs);
	if (ret)
		return ret;
	ret = sftl_load_context(fs);
	if (ret) {
		sftl_err("no usable FTL context (%d)", ret);
		return ret;
	}
	return sftl_replay(fs);
}

/* Read-through a one-page cache keyed by physical address. */
int sftl_read_page_cached(struct sftl *fs, u32 bank, u32 block, u32 page,
			  u8 *data, u8 *meta)
{
	u8 st;
	int ret;

	if (!(fs->cache_valid && fs->cache_bank == bank &&
	      fs->cache_block == block && fs->cache_page == page)) {
		ret = read_page(fs, bank, block, page, false, fs->cache_data,
				fs->cache_meta, &st);
		if (ret)
			return ret;
		if (!status_ok(st)) {
			fs->cache_valid = false;
			return -EIO;
		}
		fs->cache_valid = true;
		fs->cache_bank = (u8)bank;
		fs->cache_block = (u16)block;
		fs->cache_page = (u16)page;
	}
	if (data)
		memcpy(data, fs->cache_data, SFTL_PAGE_SIZE);
	if (meta)
		memcpy(meta, fs->cache_meta, SFTL_META_PAGE);
	return 0;
}

/* Read one VBA's data (optional) and metadata (optional). */
int sftl_read_vba(struct sftl *fs, u32 vba, u8 *data, u8 *meta16)
{
	u32 bank, page, slot;
	int ret;

	if (!sftl_locate(fs, vba, &bank, &page, &slot))
		return -EIO;
	ret = sftl_read_page_cached(fs, bank, sftl_sb_of(vba), page, NULL, NULL);
	if (ret)
		return ret;
	if (data)
		memcpy(data, fs->cache_data + slot * SFTL_LBA_SIZE, SFTL_LBA_SIZE);
	if (meta16)
		memcpy(meta16, fs->cache_meta + slot * SFTL_META_LBA, SFTL_META_LBA);
	return 0;
}

/* Read one mapped LBA, checking that the media agrees it is that LBA. */
int sftl_read_lba(struct sftl *fs, u32 lba, u8 *out, u8 *meta16)
{
	u32 vba;
	u8 meta[SFTL_META_LBA];
	int ret;

	if (lba >= fs->total_lbas)
		return -EINVAL;
	vba = sftl_map_get(&fs->map, lba);
	if (vba == SFTL_UNMAPPED) {
		memset(out, 0, SFTL_LBA_SIZE);
		if (meta16)
			memset(meta16, 0, SFTL_META_LBA);
		return 0;
	}
	ret = sftl_read_vba(fs, vba, out, meta);
	if (ret)
		return ret;
	if (sftl_get32(meta + 8) != lba) {
		sftl_err("lba %u at vba %#x: media says lba %u", lba, vba,
			 sftl_get32(meta + 8));
		return -EIO;
	}
	if (meta16)
		memcpy(meta16, meta, SFTL_META_LBA);
	return 0;
}

/* Recompute the valid counters from the map (used by the writer at start). */
int sftl_recount_valid(struct sftl *fs)
{
	u32 lba, sb;

	for (sb = 0; sb < SFTL_NUM_SB; sb++)
		fs->rec[sb].valid = 0;
	for (lba = 0; lba < fs->total_lbas; lba++) {
		u32 vba = sftl_map_get(&fs->map, lba);

		if (vba == SFTL_UNMAPPED)
			continue;
		if (sftl_sb_of(vba) >= SFTL_NUM_SB)
			return -EIO;
		fs->rec[sftl_sb_of(vba)].valid++;
	}
	return 0;
}
