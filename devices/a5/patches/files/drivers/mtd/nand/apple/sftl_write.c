// SPDX-License-Identifier: GPL-2.0-only
/*
 * Swiss FTL writer: one frontier superblock, BTOC at close, context commits,
 * and reclaim.  See the spec sections 4.8 and 7.
 */
#include "sftl.h"
#include "sftl_port.h"

static int fail_wedge(struct sftl *fs, const char *why, int err)
{
	sftl_err("writer stopped: %s (%d)", why, err);
	fs->wr->wedged = true;
	return err ? err : -EIO;
}

/* Queued for erase after the next commit: never a spare before then. */
static bool erase_pending(const struct sftl *fs, u32 sb)
{
	const struct sftl_writer *w = fs->wr;
	unsigned int i;

	for (i = 0; i < w->n_pending; i++)
		if (w->pending_erase[i] == sb)
			return true;
	return false;
}

static bool sb_bad(const struct sftl *fs, u32 sb)
{
	return (fs->wr->bad_map[sb / 8] >> (sb % 8)) & 1;
}

/*
 * Give up a superblock for this session after a program or erase failure:
 * type 1 with nothing valid, as spec 4.9 asks.  The VFL is never touched;
 * if the block is really bad, iOS meets the failure and retires it itself.
 */
static void mark_bad(struct sftl *fs, u32 sb)
{
	fs->wr->bad_map[sb / 8] |= 1 << (sb % 8);
	fs->rec[sb].type = SFTL_SB_AWAIT_ERASE;
	fs->rec[sb].btoc_flags = 0;
	fs->wr->need_commit = true;
}

static u32 spare_count(struct sftl *fs, bool four_banks)
{
	u32 sb, n = 0;

	for (sb = 0; sb < SFTL_NUM_SB; sb++) {
		const struct sftl_sb_rec *r = &fs->rec[sb];

		if (r->type != SFTL_SB_ERASED &&
		    !(r->type == SFTL_SB_AWAIT_ERASE && r->valid == 0))
			continue;
		if (!fs->shape[sb].width || sb == fs->wr->fr.sb || erase_pending(fs, sb) ||
		    sb_bad(fs, sb))
			continue;
		if (four_banks && fs->shape[sb].width != SFTL_BANKS)
			continue;
		n++;
	}
	return n;
}

static int erase_sb(struct sftl *fs, u32 sb)
{
	u32 i;

	if (fs->cache_valid && fs->cache_block == sb)
		fs->cache_valid = false;
	for (i = 0; i < fs->shape[sb].width; i++) {
		int ret = fs->ops->erase_block(fs->hw, fs->shape[sb].bank[i], sb);

		if (ret)
			return ret;
	}
	fs->rec[sb].erases++;
	fs->rec[sb].type = SFTL_SB_ERASED;
	fs->rec[sb].btoc_flags = 0;
	fs->rec[sb].valid = 0;
	memset(&fs->useq[sb], 0, sizeof(fs->useq[sb]));
	return 0;
}

/*
 * Pick the least worn spare, erase it, return its number or negative.  A
 * spare whose erase fails is given up and the next one tried.
 */
static int take_spare(struct sftl *fs, bool four_banks)
{
	unsigned int tries;

	for (tries = 0; tries < 4; tries++) {
		u32 sb, best = SFTL_NUM_SB;
		int ret;

		for (sb = 0; sb < SFTL_NUM_SB; sb++) {
			const struct sftl_sb_rec *r = &fs->rec[sb];

			if (r->type != SFTL_SB_ERASED &&
			    !(r->type == SFTL_SB_AWAIT_ERASE && r->valid == 0))
				continue;
			if (!fs->shape[sb].width || sb == fs->wr->fr.sb ||
			    erase_pending(fs, sb) || sb_bad(fs, sb))
				continue;
			if (four_banks && fs->shape[sb].width != SFTL_BANKS)
				continue;
			if (fs->wr->fenced && sb != fs->wr->fence_sb)
				continue;
			if (best == SFTL_NUM_SB || r->erases < fs->rec[best].erases)
				best = sb;
		}
		if (best == SFTL_NUM_SB)
			return -ENOSPC;
		ret = erase_sb(fs, best);
		if (!ret)
			return (int)best;
		sftl_warn("erase of sb %u failed (%d): not used in this session", best, ret);
		mark_bad(fs, best);
		if (++fs->wr->failures > SFTL_FAIL_MAX)
			break;
	}
	return -EIO;
}

static int open_frontier(struct sftl *fs)
{
	struct sftl_frontier *fr = &fs->wr->fr;
	int sb;

	if (spare_count(fs, true) < 2)
		return -ENOSPC;
	sb = take_spare(fs, false);
	if (sb < 0)
		return sb;
	fs->rec[sb].type = SFTL_SB_OPEN;
	fr->sb = (u32)sb;
	fr->capacity = sftl_capacity(fs, sb);
	fr->next = 0;
	fr->first_set = false;
	fr->nent = 0;
	return 0;
}

static u32 nb_for_entries(u32 entries)
{
	return entries + 1 <= SFTL_PAGE_SIZE / 16 ? 4 : 8;
}

/* Would a page with up to 4 more entries still fit? */
static bool frontier_has_room(const struct sftl_frontier *fr)
{
	u32 ents = fr->nent + SFTL_LBAS_PER_PAGE;

	if (ents + 1 > 2 * SFTL_PAGE_SIZE / 16)
		return false;
	return fr->next + SFTL_LBAS_PER_PAGE <= fr->capacity - nb_for_entries(ents);
}

static void remap(struct sftl *fs, u32 lba, u32 vba)
{
	u32 old = sftl_map_get(&fs->map, lba);

	if (old != SFTL_UNMAPPED)
		fs->rec[sftl_sb_of(old)].valid--;
	sftl_map_set(&fs->map, lba, vba);
	fs->rec[sftl_sb_of(vba)].valid++;
}

static int frontier_fail(struct sftl *fs, const char *what, int err);

static int batch_flush(struct sftl *fs)
{
	struct sftl_writer *w = fs->wr;
	unsigned int i;
	int ret;

	if (!w->npages)
		return 0;
	ret = fs->ops->program(fs->hw, w->prog, w->npages);
	fs->cache_valid = false;
	if (ret)
		return frontier_fail(fs, "program", ret);
	for (i = 0; i < w->npages; i++) {
		u8 st;
		const struct sftl_prog *p = &w->prog[i];

		if (++w->prog_counter % w->verify_every)
			continue;
		ret = fs->ops->read_page(fs->hw, p->bank, p->block, p->page, false,
					 w->check_data, w->check_meta, &st);
		if (ret)
			return frontier_fail(fs, "verify read", ret);
		if ((st != SFTL_ST_OK && st != SFTL_ST_REFRESH) ||
		    memcmp(w->check_data, p->data, SFTL_PAGE_SIZE) ||
		    memcmp(w->check_meta, p->meta, SFTL_META_PAGE))
			return frontier_fail(fs, "verify", -EIO);
	}
	for (i = 0; i < w->pend_n; i++)
		remap(fs, w->pend_lba[i], w->pend_vba[i]);
	w->pend_n = 0;
	w->npages = 0;
	w->dirty = true;
	return 0;
}

static u64 take_weave(struct sftl *fs)
{
	return fs->next_weave++;
}

/* Start a page at the frontier; returns the batch index. */
static unsigned int page_begin(struct sftl *fs)
{
	struct sftl_writer *w = fs->wr;
	unsigned int idx = w->npages;
	struct sftl_prog *p = &w->prog[idx];
	u32 vba = sftl_vba(w->fr.sb, w->fr.next);
	u32 bank, page, slot;

	sftl_locate(fs, vba, &bank, &page, &slot);
	p->bank = (u8)bank;
	p->slc = false;
	p->block = (u16)w->fr.sb;
	p->page = (u16)page;
	p->data = w->pool + idx * SFTL_PAGE_SIZE;
	p->meta = w->meta_pool + idx * SFTL_META_PAGE;
	w->npages++;
	return idx;
}

/* Fill slot of the current page.  src == NULL with lba == PAD writes zeros. */
static void slot_fill(struct sftl *fs, unsigned int idx, unsigned int slot,
		      u32 lba, const u8 *src, u8 type, u32 user, bool keep_user)
{
	struct sftl_writer *w = fs->wr;
	struct sftl_frontier *fr = &w->fr;
	u8 *data = w->pool + idx * SFTL_PAGE_SIZE + slot * SFTL_LBA_SIZE;
	u8 *meta = w->meta_pool + idx * SFTL_META_PAGE + slot * SFTL_META_LBA;
	u64 weave = take_weave(fs);

	if (!keep_user)
		user = (u32)(weave >> 16);
	if (src)
		memcpy(data, src, SFTL_LBA_SIZE);
	else
		memset(data, 0, SFTL_LBA_SIZE);
	meta[0] = type;
	meta[1] = 0;
	sftl_put48(meta + 2, weave);
	sftl_put32(meta + 8, lba);
	sftl_put32(meta + 12, user);

	if (!fr->first_set) {
		fr->first_weave = weave;
		fr->first_set = true;
	}
	fr->last_weave = weave;
	sftl_btoc_append(w->entries, &fr->nent, fr->first_weave, lba, weave, user);

	if (lba < SFTL_TOKEN_MIN) {
		w->pend_lba[w->pend_n] = lba;
		w->pend_vba[w->pend_n] = sftl_vba(fr->sb, fr->next);
		w->pend_n++;
	}
	fr->next++;
}

/* Close the open frontier: write its BTOC into the last nb VBAs. */
static int frontier_close(struct sftl *fs)
{
	struct sftl_writer *w = fs->wr;
	struct sftl_frontier *fr = &w->fr;
	struct sftl_prog prog[2];
	u8 metas[2 * SFTL_META_PAGE];
	u32 nb, i, np, sb = fr->sb;
	int ret;

	if (sb == SFTL_UNMAPPED)
		return 0;
	if (!fr->first_set) {
		/* Opened but never written: put it back. */
		fs->rec[sb].type = SFTL_SB_ERASED;
		fr->sb = SFTL_UNMAPPED;
		return 0;
	}
	/*
	 * Every closed superblock iOS leaves on the media is written front to
	 * back with no hole before its BTOC: fill what is left with PAD.  A
	 * page of PAD adds at most one BTOC entry (runs of 16).
	 */
	for (;;) {
		unsigned int idx, s;

		nb = nb_for_entries(fr->nent + 1);
		if (fr->next + SFTL_LBAS_PER_PAGE > fr->capacity - nb)
			break;
		if (w->npages == SFTL_BATCH_PAGES) {
			ret = batch_flush(fs);
			if (ret)
				return ret;
		}
		idx = page_begin(fs);
		for (s = 0; s < SFTL_LBAS_PER_PAGE; s++)
			slot_fill(fs, idx, s, SFTL_TOKEN_PAD, NULL, SFTL_MT_USER, 0, false);
	}
	ret = batch_flush(fs);
	if (ret)
		return ret;
	sb = fr->sb;		/* a failure in the PAD fill moves to another SB */
	/* The BTOC follows the data directly: 4 VBAs when that is all that is left. */
	nb = nb_for_entries(fr->nent);
	if (nb == 4 && fr->next + 8 <= fr->capacity)
		nb = 8;
	sftl_btoc_build(w->entries, fr->nent, w->btoc_img, &nb, nb);
	np = nb / SFTL_LBAS_PER_PAGE;
	memset(metas, 0, sizeof(metas));
	for (i = 0; i < nb; i++) {
		u8 *m = metas + i * SFTL_META_LBA;

		m[0] = SFTL_MT_BTOC;
		m[1] = (u8)(nb << 1);
		sftl_put48(m + 2, fr->first_weave);
		sftl_put32(m + 8, (u32)(fr->last_weave - fr->first_weave + 1));
		sftl_put16(m + 12, (u16)nb);
		sftl_put16(m + 14, 0xffff);
	}
	for (i = 0; i < np; i++) {
		u32 bank, page, slot;

		sftl_locate(fs, sftl_vba(sb, fr->capacity - nb + i * SFTL_LBAS_PER_PAGE),
			    &bank, &page, &slot);
		prog[i].bank = (u8)bank;
		prog[i].slc = false;
		prog[i].block = (u16)sb;
		prog[i].page = (u16)page;
		prog[i].data = w->btoc_img + i * SFTL_PAGE_SIZE;
		prog[i].meta = metas + i * SFTL_META_PAGE;
	}
	ret = fs->ops->program(fs->hw, prog, np);
	fs->cache_valid = false;
	for (i = 0; !ret && i < np; i++) {
		u8 st;

		ret = fs->ops->read_page(fs->hw, prog[i].bank, sb, prog[i].page, false,
					 w->check_data, w->check_meta, &st);
		if (!ret && (st != SFTL_ST_OK || memcmp(w->check_data, prog[i].data,
		    SFTL_PAGE_SIZE) || memcmp(w->check_meta, prog[i].meta, SFTL_META_PAGE)))
			ret = -EIO;
	}
	if (ret) {
		/* Its data are all mapped: move them out and close where they land. */
		ret = frontier_fail(fs, "BTOC", ret);
		return ret ? ret : frontier_close(fs);
	}
	sftl_summarize_useq(w->btoc_img, fr->nent, &fs->useq[sb]);
	fs->rec[sb].type = SFTL_SB_CLOSED;
	fs->rec[sb].btoc_flags = (u8)(nb << 1);
	fr->sb = SFTL_UNMAPPED;
	return 0;
}

static int ensure_room(struct sftl *fs)
{
	struct sftl_writer *w = fs->wr;
	int ret;

	if (w->fr.sb != SFTL_UNMAPPED && !frontier_has_room(&w->fr)) {
		ret = batch_flush(fs);
		if (!ret)
			ret = frontier_close(fs);
		if (ret)
			return ret;
	}
	if (w->fr.sb == SFTL_UNMAPPED) {
		ret = batch_flush(fs);
		if (ret)
			return ret;
		return open_frontier(fs);
	}
	if (w->npages == SFTL_BATCH_PAGES)
		return batch_flush(fs);
	return 0;
}

static int gc_move(struct sftl *fs, const u32 *list, u32 n);
static int reclaim(struct sftl *fs);

static int read_sibling(struct sftl *fs, u32 vba, u8 *buf, u8 *meta16)
{
	return sftl_read_vba(fs, vba, buf, meta16);
}

/* ---- keeping the tree inside its 32 LBAs -------------------------------- */

#define TREE_RUN_LIMIT		(SFTL_TREE_MAX_LBAS * 511u - 64u)
#define COMPACT_WINDOW		1024u
#define COMPACT_TOP_WINDOWS	8u
#define COMPACT_MAX_PASSES	32u

static bool continues_run(u32 prev_vba, u32 vba)
{
	if (vba == SFTL_UNMAPPED)
		return prev_vba == SFTL_UNMAPPED;
	return prev_vba != SFTL_UNMAPPED && prev_vba + 1 == vba;
}

/*
 * A Linux context holds at most SFTL_TREE_MAX_LBAS tree LBAs of runs.  When
 * the map fragments past that, rewrite the most fragmented windows
 * contiguously (as GC does: type and user kept) so the commit can go through.
 */
static int tree_compact(struct sftl *fs)
{
	struct sftl_writer *w = fs->wr;
	u32 nwin = fs->total_lbas / COMPACT_WINDOW + 1;
	u32 *breaks;
	u32 pass;
	int ret = 0;

	breaks = sftl_alloc(nwin * sizeof(*breaks));
	if (!breaks)
		return -ENOMEM;

	for (pass = 0; pass < COMPACT_MAX_PASSES; pass++) {
		u32 lba, prev = SFTL_UNMAPPED, runs = 1, n = 0, t;
		u32 top[COMPACT_TOP_WINDOWS];

		memset(breaks, 0, nwin * sizeof(*breaks));
		for (lba = 0; lba < fs->total_lbas; lba++) {
			u32 v = sftl_map_get(&fs->map, lba);

			if (lba && !continues_run(prev, v)) {
				runs++;
				breaks[lba / COMPACT_WINDOW]++;
			}
			prev = v;
		}
		if (runs <= TREE_RUN_LIMIT)
			goto out;

		for (t = 0; t < COMPACT_TOP_WINDOWS; t++) {
			u32 best = nwin, i;

			for (i = 0; i < nwin; i++)
				if (breaks[i] > 1 && (best == nwin || breaks[i] > breaks[best]))
					best = i;
			top[t] = best;
			if (best == nwin)
				break;
			breaks[best] = 0;
		}
		if (!t) {
			ret = -ENOSPC;
			goto out;
		}
		while (t--) {
			u32 start = top[t] * COMPACT_WINDOW;
			u32 end = start + COMPACT_WINDOW;

			if (end > fs->total_lbas)
				end = fs->total_lbas;
			for (lba = start; lba < end; lba++)
				if (sftl_map_get(&fs->map, lba) != SFTL_UNMAPPED)
					w->gc_list[n++] = lba;
		}
		sftl_info("tree: %u runs, compacting %u LBAs", runs, n);
		ret = gc_move(fs, w->gc_list, n);
		if (ret)
			goto out;
	}
	ret = -ENOSPC;
out:
	sftl_free(breaks);
	return ret;
}

/*
 * Build a context from the current state and program it to sb, which must be
 * freshly erased and marked as a context in the SB array; verify every page.
 * On success the base weave and length are in fs->ctx_weave and ctx_lbas,
 * and the weave counter has moved past the context.
 */
int sftl_context_write(struct sftl *fs, u32 sb)
{
	struct sftl_writer *w = fs->wr;
	struct sftl_prog *prog = w->ctx_prog;
	u8 *metas = w->ctx_pages + SFTL_CTX_MAX_PAGES * SFTL_PAGE_SIZE;
	u64 weave = fs->next_weave;
	u32 i, nlbas = 0, np;
	int ret;

	if (fs->shape[sb].width != SFTL_BANKS)
		return -EINVAL;
	ret = sftl_ctx_build(fs, sb, weave, w->ctx_pages, metas, &nlbas);
	if (ret)
		return fail_wedge(fs, "context build", ret);
	np = nlbas / SFTL_LBAS_PER_PAGE;
	for (i = 0; i < np; i++) {
		prog[i].bank = fs->shape[sb].bank[i % SFTL_BANKS];
		prog[i].slc = false;
		prog[i].block = (u16)sb;
		prog[i].page = (u16)(i / SFTL_BANKS);
		prog[i].data = w->ctx_pages + i * SFTL_PAGE_SIZE;
		prog[i].meta = metas + i * SFTL_META_PAGE;
	}
	fs->next_weave = weave + nlbas;
	ret = fs->ops->program(fs->hw, prog, np);
	fs->cache_valid = false;
	for (i = 0; !ret && i < np; i++) {
		u8 st;

		ret = fs->ops->read_page(fs->hw, prog[i].bank, sb, prog[i].page,
					 false, w->check_data, w->check_meta, &st);
		if (!ret && (st != SFTL_ST_OK ||
		    memcmp(w->check_data, prog[i].data, SFTL_PAGE_SIZE) ||
		    memcmp(w->check_meta, prog[i].meta, SFTL_META_PAGE)))
			ret = -EIO;
	}
	if (ret) {
		/* The media failed, not the build: the caller may try another SB. */
		sftl_warn("context on sb %u failed (%d)", sb, ret);
		return -EAGAIN;
	}
	fs->ctx_weave = weave;
	fs->ctx_lbas = nlbas;
	sftl_info("context written to sb %u: weave %llu, %u LBAs (%u pages)", sb,
		  (unsigned long long)weave, nlbas, np);
	return 0;
}

/*
 * Commit: close the frontier, write a new context to a fresh superblock and
 * verify every page.  Older contexts and victims are erased only after that.
 */
static int commit(struct sftl *fs)
{
	struct sftl_writer *w = fs->wr;
	u64 weave;
	u32 i, new_sb, keep = fs->ctx_sb;
	int ret, sb;

	if (w->fenced && !w->fence_commit)
		return fail_wedge(fs, "commit while fenced", -EPERM);
	ret = tree_compact(fs);
	if (ret)
		return fail_wedge(fs, "tree does not fit a context", ret);
	ret = batch_flush(fs);
	if (ret)
		return ret;
	ret = frontier_close(fs);
	if (ret)
		return ret;

	sb = take_spare(fs, true);
	if (sb < 0)
		return fail_wedge(fs, "no spare for context", sb);
	new_sb = (u32)sb;
	fs->rec[new_sb].type = SFTL_SB_CONTEXT;

	/* Every context older than the one we keep is retired by this one. */
	for (i = 0; i < fs->n_ctx; i++) {
		u32 c = fs->ctx_list[i];

		if (c != keep && fs->rec[c].type == SFTL_SB_CONTEXT) {
			fs->rec[c].type = SFTL_SB_AWAIT_ERASE;
			fs->rec[c].valid = 0;
			if (w->n_pending < SFTL_MAX_VICTIMS * 2 + SFTL_MAX_CTX)
				w->pending_erase[w->n_pending++] = c;
		}
	}

	for (i = 0; ; i++) {
		ret = sftl_context_write(fs, new_sb);
		if (ret != -EAGAIN)
			break;
		mark_bad(fs, new_sb);
		if (i == 2 || ++w->failures > SFTL_FAIL_MAX)
			return fail_wedge(fs, "context program", -EIO);
		sb = take_spare(fs, true);
		if (sb < 0)
			return fail_wedge(fs, "no spare for context", sb);
		new_sb = (u32)sb;
		fs->rec[new_sb].type = SFTL_SB_CONTEXT;
	}
	if (ret)
		return ret;
	weave = fs->ctx_weave;

	/* The new context stands: retire and erase what it replaced. */
	for (i = 0; i < w->n_pending; i++) {
		if (w->pending_erase[i] == new_sb)
			return fail_wedge(fs, "new context queued for erase", -EIO);
		ret = erase_sb(fs, w->pending_erase[i]);
		if (ret) {
			/* It holds nothing valid already: give it up and go on. */
			sftl_warn("erase of sb %u after commit failed (%d)",
				  w->pending_erase[i], ret);
			mark_bad(fs, w->pending_erase[i]);
			fs->rec[w->pending_erase[i]].valid = 0;
			w->failures++;
		}
	}
	w->n_pending = 0;

	fs->n_ctx = 0;
	fs->ctx_list[fs->n_ctx++] = new_sb;
	if (keep != new_sb && fs->rec[keep].type == SFTL_SB_CONTEXT)
		fs->ctx_list[fs->n_ctx++] = keep;
	fs->ctx_sb = new_sb;
	fs->replayed = false;
	w->dirty = false;
	w->need_commit = false;
	sftl_info("committed context on sb %u, weave %llu", new_sb,
		  (unsigned long long)weave);
	return 0;
}

int sftl_checkpoint_due(struct sftl *fs)
{
	return fs->wr && !fs->wr->wedged &&
	       (fs->wr->dirty || fs->replayed || fs->wr->need_commit);
}

int sftl_checkpoint(struct sftl *fs)
{
	if (!fs->wr)
		return -EROFS;
	if (fs->wr->wedged)
		return -EIO;
	if (!sftl_checkpoint_due(fs))
		return 0;
	return commit(fs);
}

/*
 * Restrict the writer to one superblock, for the device ladder: the frontier
 * or a commit's context may only go there, reclaim refuses, and commit refuses
 * unless allowed.  The superblock must be an erased spare with four banks.
 */
int sftl_writer_fence(struct sftl *fs, u32 sb, bool allow_commit)
{
	struct sftl_writer *w = fs->wr;

	if (!w)
		return -EROFS;
	if (sb >= SFTL_NUM_SB || fs->rec[sb].type != SFTL_SB_ERASED ||
	    fs->shape[sb].width != SFTL_BANKS || w->fr.sb != SFTL_UNMAPPED)
		return -EINVAL;
	w->fenced = true;
	w->fence_commit = allow_commit;
	w->fence_sb = sb;
	return 0;
}

/* Flush what is pending and close the frontier with its BTOC; no commit. */
int sftl_writer_seal(struct sftl *fs)
{
	int ret;

	if (!fs->wr)
		return -EROFS;
	if (fs->wr->wedged)
		return -EIO;
	ret = batch_flush(fs);
	return ret ? ret : frontier_close(fs);
}

/* Run one reclaim now: move the victims' live LBAs, commit, erase them. */
int sftl_gc(struct sftl *fs)
{
	if (!fs->wr)
		return -EROFS;
	if (fs->wr->wedged)
		return -EIO;
	return reclaim(fs);
}

/* Move the LBA list (in list order) to the frontier, keeping type and user. */
static int gc_move(struct sftl *fs, const u32 *list, u32 n)
{
	struct sftl_writer *w = fs->wr;
	u8 meta[SFTL_META_LBA];
	u32 i = 0;
	int ret;

	while (i < n) {
		unsigned int idx, s;

		ret = ensure_room(fs);
		if (ret)
			return ret;
		idx = page_begin(fs);
		for (s = 0; s < SFTL_LBAS_PER_PAGE; s++) {
			u32 lba;

			if (i >= n) {
				slot_fill(fs, idx, s, SFTL_TOKEN_PAD, NULL,
					  SFTL_MT_USER, 0, false);
				continue;
			}
			lba = list[i++];
			ret = read_sibling(fs, sftl_map_get(&fs->map, lba),
					   w->check_data, meta);
			if (ret)
				return fail_wedge(fs, "gc read", ret);
			slot_fill(fs, idx, s, lba, w->check_data, meta[0],
				  sftl_get32(meta + 12), true);
		}
	}
	return batch_flush(fs);
}

/*
 * A program or verify failed on the frontier (spec 4.9): program nothing more
 * into that SB, leave it unclosed, give it up, and put everything it held on
 * a new frontier: the failed batch from memory, then every LBA the map still
 * places in it, read back, keeping type and user.  A commit follows soon.
 */
static int frontier_fail(struct sftl *fs, const char *what, int err)
{
	struct sftl_writer *w = fs->wr;
	struct sftl_frontier *fr = &w->fr;
	unsigned int d = w->fail_depth, i, s, k = 0;
	u32 sb = fr->sb, lba, n = 0;
	u8 *rd, *rm;
	u32 *rl, *list;
	int ret;

	if (sb == SFTL_UNMAPPED || d >= SFTL_FAIL_DEPTH || ++w->failures > SFTL_FAIL_MAX)
		return fail_wedge(fs, what, err);
	sftl_warn("%s failed on sb %u (%d): giving it up, moving its data", what, sb, err);
	rd = w->rescue_data + (size_t)d * SFTL_RESCUE_LBAS * SFTL_LBA_SIZE;
	rm = w->rescue_meta + d * SFTL_RESCUE_LBAS * SFTL_META_LBA;
	rl = w->rescue_lba + d * SFTL_RESCUE_LBAS;
	list = w->fail_list + d * SFTL_LBAS_PER_SB;
	w->fail_depth++;

	/* The failed batch: its data are still in memory. */
	for (i = 0; i < w->npages; i++) {
		for (s = 0; s < SFTL_LBAS_PER_PAGE; s++) {
			const u8 *m = w->meta_pool + i * SFTL_META_PAGE + s * SFTL_META_LBA;

			lba = sftl_get32(m + 8);
			if (lba >= SFTL_TOKEN_MIN)
				continue;
			memcpy(rd + (size_t)k * SFTL_LBA_SIZE,
			       w->pool + i * SFTL_PAGE_SIZE + s * SFTL_LBA_SIZE, SFTL_LBA_SIZE);
			memcpy(rm + k * SFTL_META_LBA, m, SFTL_META_LBA);
			rl[k++] = lba;
		}
	}
	w->npages = 0;
	w->pend_n = 0;
	mark_bad(fs, sb);
	fr->sb = SFTL_UNMAPPED;

	/* What the map still places in the failed SB, other than the batch. */
	for (lba = 0; lba < fs->total_lbas && n < SFTL_LBAS_PER_SB; lba++) {
		u32 v = sftl_map_get(&fs->map, lba), j;

		if (v == SFTL_UNMAPPED || sftl_sb_of(v) != sb)
			continue;
		for (j = 0; j < k && rl[j] != lba; j++)
			;
		if (j == k)
			list[n++] = lba;
	}

	for (i = 0; i < k; i += SFTL_LBAS_PER_PAGE) {
		unsigned int idx;

		ret = ensure_room(fs);
		if (ret)
			goto out;
		idx = page_begin(fs);
		for (s = 0; s < SFTL_LBAS_PER_PAGE; s++) {
			const u8 *m = rm + (i + s) * SFTL_META_LBA;

			if (i + s < k)
				slot_fill(fs, idx, s, rl[i + s], rd + (size_t)(i + s) * SFTL_LBA_SIZE,
					  m[0], sftl_get32(m + 12), true);
			else
				slot_fill(fs, idx, s, SFTL_TOKEN_PAD, NULL, SFTL_MT_USER, 0, false);
		}
	}
	ret = batch_flush(fs);
	if (!ret)
		ret = gc_move(fs, list, n);
	if (!ret) {
		fs->rec[sb].valid = 0;
		sftl_info("sb %u given up: %u LBAs from the failed batch and %u more moved",
			  sb, k, n);
	}
out:
	w->fail_depth--;
	return ret ? fail_wedge(fs, "moving data off a failed SB", ret) : 0;
}

static int victim_cmp(const struct sftl_sb_rec *a, const struct sftl_sb_rec *b)
{
	return (int)a->valid - (int)b->valid;
}

/*
 * Choose up to SFTL_MAX_VICTIMS closed, non-static SBs, fewest valid first,
 * at most 2048 live LBAs in all past the first, and list their live LBAs in
 * LBA order into w->gc_list.  No media access.  Returns the victim count.
 */
static u32 pick_victims(struct sftl *fs, u32 *victims, u32 *count_out, u32 *live_out)
{
	struct sftl_writer *w = fs->wr;
	u32 nv = 0, sb, lba, count = 0, live = 0;

	memset(w->victim_map, 0, (SFTL_NUM_SB + 7) / 8);
	for (;;) {
		u32 best = SFTL_NUM_SB;

		for (sb = 0; sb < SFTL_NUM_SB; sb++) {
			const struct sftl_sb_rec *r = &fs->rec[sb];

			if (r->type != SFTL_SB_CLOSED || (r->btoc_flags & 1) ||
			    !fs->shape[sb].width || sb == fs->ctx_sb ||
			    (w->victim_map[sb / 8] >> (sb % 8)) & 1)
				continue;
			if (best == SFTL_NUM_SB ||
			    victim_cmp(r, &fs->rec[best]) < 0)
				best = sb;
		}
		if (best == SFTL_NUM_SB)
			break;
		if (nv && live + fs->rec[best].valid > SFTL_LBAS_PER_SB / 2)
			break;
		live += fs->rec[best].valid;
		w->victim_map[best / 8] |= 1 << (best % 8);
		victims[nv++] = best;
		if (nv == SFTL_MAX_VICTIMS)
			break;
	}
	for (lba = 0; nv && lba < fs->total_lbas && count < SFTL_MAX_VICTIMS * SFTL_LBAS_PER_SB; lba++) {
		u32 vba = sftl_map_get(&fs->map, lba);

		if (vba == SFTL_UNMAPPED)
			continue;
		sb = sftl_sb_of(vba);
		if ((w->victim_map[sb / 8] >> (sb % 8)) & 1)
			w->gc_list[count++] = lba;
	}
	*count_out = count;
	*live_out = live;
	return nv;
}

/* What a reclaim would do now: its victims and the live LBAs it would move. */
int sftl_gc_plan(struct sftl *fs, u32 *victims, u32 *nv, u32 *lbas, u32 *count)
{
	u32 live;

	if (!fs->wr)
		return -EROFS;
	*nv = pick_victims(fs, victims, count, &live);
	if (*count != live)
		return -EIO;
	memcpy(lbas, fs->wr->gc_list, *count * sizeof(u32));
	return *nv ? 0 : -ENOSPC;
}

/* Up to SFTL_MAX_VICTIMS closed, non-static SBs; then a checkpoint. */
static int reclaim(struct sftl *fs)
{
	struct sftl_writer *w = fs->wr;
	u32 victims[SFTL_MAX_VICTIMS];
	u32 nv, i, sb, count = 0, live = 0;
	int ret;

	if (w->fenced)
		return fail_wedge(fs, "reclaim while fenced", -EPERM);
	nv = pick_victims(fs, victims, &count, &live);
	if (!nv)
		return -ENOSPC;
	if (count != live)
		return fail_wedge(fs, "reclaim list mismatch", -EIO);

	sftl_info("reclaim: %u victims, %u live LBAs", nv, count);
	if (count) {
		/* Prefer a clean frontier if the current one cannot hold them. */
		if (w->fr.sb != SFTL_UNMAPPED && w->fr.capacity - w->fr.next < count + 8) {
			ret = batch_flush(fs);
			if (!ret)
				ret = frontier_close(fs);
			if (ret)
				return ret;
		}
		ret = gc_move(fs, w->gc_list, count);
		if (ret)
			return ret;
	}
	for (i = 0; i < nv; i++) {
		sb = victims[i];
		if (fs->rec[sb].valid != 0)
			return fail_wedge(fs, "victim still valid", -EIO);
		fs->rec[sb].type = SFTL_SB_AWAIT_ERASE;
		w->pending_erase[w->n_pending++] = sb;
	}
	return commit(fs);
}

int sftl_write(struct sftl *fs, u32 lba, u32 count, const u8 *src)
{
	struct sftl_writer *w = fs->wr;
	u8 meta[SFTL_META_LBA];
	int ret;

	if (!w)
		return -EROFS;
	if (w->wedged)
		return -EIO;
	if (lba + count > fs->reported_lbas || lba + count < lba)
		return -EINVAL;

	while (spare_count(fs, true) < SFTL_SPARE_FLOOR) {
		u32 before = spare_count(fs, true);

		ret = reclaim(fs);
		if (ret)
			return ret;
		if (spare_count(fs, true) <= before)
			break;
	}
	if (spare_count(fs, true) < SFTL_SPARE_RESERVE)
		return -ENOSPC;

	while (count) {
		u32 base = lba & ~(SFTL_LBAS_PER_PAGE - 1);
		u32 n = SFTL_LBAS_PER_PAGE - (lba - base);
		unsigned int idx, s;

		if (n > count)
			n = count;
		ret = ensure_room(fs);
		if (ret)
			return ret;
		idx = page_begin(fs);
		for (s = 0; s < SFTL_LBAS_PER_PAGE; s++) {
			u32 l = base + s;

			if (l >= lba && l < lba + n) {
				slot_fill(fs, idx, s, l, src + (l - lba) * SFTL_LBA_SIZE,
					  SFTL_MT_USER, 0, false);
				continue;
			}
			{
				u32 vba = l < fs->total_lbas ?
					  sftl_map_get(&fs->map, l) : SFTL_UNMAPPED;

				if (vba == SFTL_UNMAPPED) {
					slot_fill(fs, idx, s, SFTL_TOKEN_PAD, NULL,
						  SFTL_MT_USER, 0, false);
				} else {
					ret = read_sibling(fs, vba, w->check_data, meta);
					if (ret)
						return fail_wedge(fs, "sibling read", ret);
					slot_fill(fs, idx, s, l, w->check_data, meta[0],
						  sftl_get32(meta + 12), true);
				}
			}
		}
		src += n * SFTL_LBA_SIZE;
		lba += n;
		count -= n;
	}
	ret = batch_flush(fs);
	if (!ret && w->need_commit && !w->hold_commit)
		ret = commit(fs);
	return ret;
}

/*
 * Erase a spare again, for recovery after an erase that was cut off.  Anything
 * that is not a spare (valid data, the context, the frontier, a queued
 * reclaim victim) is refused.  A failed erase gives the SB up for the session.
 */
int sftl_erase_spare(struct sftl *fs, u32 sb)
{
	const struct sftl_sb_rec *r;
	int ret;

	if (!fs->wr || fs->wr->wedged)
		return -EIO;
	if (sb >= SFTL_NUM_SB || !fs->shape[sb].width)
		return -EINVAL;
	r = &fs->rec[sb];
	if (sb == fs->ctx_sb || sb == fs->wr->fr.sb || r->valid || erase_pending(fs, sb) ||
	    (r->type != SFTL_SB_ERASED && r->type != SFTL_SB_AWAIT_ERASE))
		return -EINVAL;
	ret = erase_sb(fs, sb);
	if (ret)
		mark_bad(fs, sb);
	return ret;
}

/*
 * Spec 4.9 at mount: an open SB whose replay walk met a page that did not read
 * back (a program that failed or was cut off, by a crash or by anything else
 * driving the controller) is given up like a failed frontier: every LBA the
 * map places in it is moved, and it is listed waiting for erase with nothing
 * valid, so neither Linux nor iOS programs into it again before an erase.
 */
static int give_up_torn(struct sftl *fs)
{
	struct sftl_writer *w = fs->wr;
	u32 sb, lba, n;
	int ret;

	for (sb = 0; sb < SFTL_NUM_SB; sb++) {
		if (!((fs->torn[sb / 8] >> (sb % 8)) & 1))
			continue;
		if (fs->rec[sb].type != SFTL_SB_OPEN) {
			sftl_warn("sb %u: a page did not read back at replay, but the SB is closed; kept",
				  sb);
			continue;
		}
		n = 0;
		for (lba = 0; lba < fs->total_lbas && n < SFTL_LBAS_PER_SB; lba++) {
			u32 v = sftl_map_get(&fs->map, lba);

			if (v != SFTL_UNMAPPED && sftl_sb_of(v) == sb)
				w->fail_list[n++] = lba;
		}
		sftl_warn("sb %u: a page did not read back at replay: giving it up, moving %u LBAs",
			  sb, n);
		mark_bad(fs, sb);
		ret = gc_move(fs, w->fail_list, n);
		if (ret)
			return ret;
		fs->rec[sb].valid = 0;
	}
	return 0;
}

/* Set up the writer: reconcile the SB array with what is on the media. */
int sftl_writer_start(struct sftl *fs, unsigned int verify_every)
{
	struct sftl_writer *w;
	u8 *sig;
	u32 sb;
	int ret;

	if (fs->sig_bank >= SFTL_BANKS) {
		sftl_err("no signature block in the VFL");
		return -EINVAL;
	}
	sig = sftl_alloc(SFTL_PAGE_SIZE);
	if (!sig)
		return -ENOMEM;
	{
		static const u32 expected[] = {
			0x776d7278, 0, 6, 1, 1, 0, 2, 2, 0x20000, 0, 1, 0, 0 /* sig_word12 */, 2,
		};
		u8 st;
		unsigned int i;

		ret = fs->ops->read_page(fs->hw, fs->sig_bank, fs->sig_block, 0, true,
					 sig, fs->io_meta, &st);
		if (!ret && st != SFTL_ST_OK)
			ret = -EIO;
		if (!ret && (sftl_get32(sig + 32) != 1 ||
			     sftl_get32(sig + 36) != 0x600))
			ret = -EPERM;
		for (i = 0; !ret && i < 14; i++)
			if (sftl_get32(sig + 128 + 4 * i) !=
			    (i == 12 ? sftl_geo.sig_word12 : expected[i]))
				ret = -EPERM;
	}
	sftl_free(sig);
	if (ret) {
		sftl_err("signature block mismatch; refusing to write (%d)", ret);
		return ret;
	}

	w = sftl_alloc(sizeof(*w));
	if (!w)
		return -ENOMEM;
	fs->wr = w;
	w->verify_every = verify_every ? verify_every : SFTL_VERIFY_DEFAULT;
	w->fr.sb = SFTL_UNMAPPED;
	w->pool = sftl_alloc(SFTL_BATCH_PAGES * SFTL_PAGE_SIZE);
	w->meta_pool = sftl_alloc(SFTL_BATCH_PAGES * SFTL_META_PAGE);
	w->pend_lba = sftl_alloc(SFTL_BATCH_PAGES * SFTL_LBAS_PER_PAGE * sizeof(u32));
	w->pend_vba = sftl_alloc(SFTL_BATCH_PAGES * SFTL_LBAS_PER_PAGE * sizeof(u32));
	w->entries = sftl_alloc(2 * SFTL_PAGE_SIZE / 16 * sizeof(*w->entries));
	w->check_data = sftl_alloc(SFTL_PAGE_SIZE);
	w->btoc_img = sftl_alloc(2 * SFTL_PAGE_SIZE);
	w->ctx_pages = sftl_alloc(SFTL_CTX_MAX_PAGES * (SFTL_PAGE_SIZE + SFTL_META_PAGE));
	w->ctx_prog = sftl_alloc(SFTL_CTX_MAX_PAGES * sizeof(*w->ctx_prog));
	w->gc_list = sftl_alloc(SFTL_MAX_VICTIMS * SFTL_LBAS_PER_SB * sizeof(u32));
	w->victim_map = sftl_alloc((SFTL_NUM_SB + 7) / 8);
	w->bad_map = sftl_alloc((SFTL_NUM_SB + 7) / 8);
	w->rescue_data = sftl_alloc((size_t)SFTL_FAIL_DEPTH * SFTL_RESCUE_LBAS * SFTL_LBA_SIZE);
	w->rescue_meta = sftl_alloc(SFTL_FAIL_DEPTH * SFTL_RESCUE_LBAS * SFTL_META_LBA);
	w->rescue_lba = sftl_alloc(SFTL_FAIL_DEPTH * SFTL_RESCUE_LBAS * sizeof(u32));
	w->fail_list = sftl_alloc(SFTL_FAIL_DEPTH * SFTL_LBAS_PER_SB * sizeof(u32));
	if (!w->pool || !w->meta_pool || !w->pend_lba || !w->pend_vba ||
	    !w->entries || !w->check_data || !w->btoc_img || !w->ctx_pages || !w->ctx_prog ||
	    !w->gc_list || !w->victim_map || !w->bad_map || !w->rescue_data ||
	    !w->rescue_meta || !w->rescue_lba || !w->fail_list) {
		ret = -ENOMEM;
		goto fail;
	}

	ret = sftl_recount_valid(fs);
	if (ret)
		goto fail;

	/* Bring the SB array in line with the media (spec 4.7, 4.8). */
	for (sb = 0; sb < SFTL_NUM_SB; sb++) {
		const struct sftl_sb_probe *pr = &fs->probe[sb];
		struct sftl_sb_rec *r = &fs->rec[sb];

		if (!fs->shape[sb].width)
			continue;
		if (pr->first_type == SFTL_MT_ERASED) {
			if (r->valid) {
				sftl_err("sb %u: erased but %u LBAs mapped", sb, r->valid);
				ret = -EIO;
				goto fail;
			}
			r->type = SFTL_SB_ERASED;
			r->btoc_flags = 0;
		} else if (pr->first_type == SFTL_MT_CONTEXT) {
			r->type = SFTL_SB_CONTEXT;
		} else if (pr->first_weave > fs->ctx_weave ||
			   r->type == SFTL_SB_OPEN || r->type == SFTL_SB_OPEN_ALT) {
			u8 last[SFTL_META_LBA];

			if (sftl_sb_closed(fs, sb, last)) {
				u32 nb, ents;

				r->type = SFTL_SB_CLOSED;
				r->btoc_flags = last[1];
				ret = sftl_read_btoc(fs, sb, &nb, w->btoc_img);
				if (ret)
					goto fail;
				for (ents = 0; ents < nb * SFTL_LBA_SIZE / 16; ents++)
					if (!sftl_get32(w->btoc_img + 16 * ents + 12))
						break;
				sftl_summarize_useq(w->btoc_img, ents, &fs->useq[sb]);
			} else {
				r->type = SFTL_SB_OPEN;
				r->btoc_flags = 0;
			}
		}
	}
	w->dirty = false;
	ret = give_up_torn(fs);
	if (ret)
		goto fail;
	return 0;
fail:
	sftl_writer_stop(fs);
	return ret;
}

void sftl_writer_stop(struct sftl *fs)
{
	struct sftl_writer *w = fs->wr;

	if (!w)
		return;
	sftl_free(w->pool);
	sftl_free(w->meta_pool);
	sftl_free(w->pend_lba);
	sftl_free(w->pend_vba);
	sftl_free(w->entries);
	sftl_free(w->check_data);
	sftl_free(w->btoc_img);
	sftl_free(w->ctx_pages);
	sftl_free(w->ctx_prog);
	sftl_free(w->gc_list);
	sftl_free(w->victim_map);
	sftl_free(w->bad_map);
	sftl_free(w->rescue_data);
	sftl_free(w->rescue_meta);
	sftl_free(w->rescue_lba);
	sftl_free(w->fail_list);
	sftl_free(w);
	fs->wr = NULL;
}
