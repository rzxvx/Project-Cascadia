// SPDX-License-Identifier: GPL-2.0-only
/*
 * Ladder rung 5 of the P105 NAND specification (section 8.2): a Linux
 * context committed, loaded, and withdrawn.  Portable: offline harness and
 * kernel test module.
 *
 *  1. mount; list the contexts on the media; start the writer;
 *  2. pick a four-bank superblock listed erased whose every page reads
 *     erased, and fence the writer to it with commit allowed;
 *  3. commit through sftl_checkpoint(), the path a real checkpoint takes.
 *     Every erase and program goes through a watcher: anything outside the
 *     chosen superblock fails the test, and erases of retired contexts are
 *     listed (with one context on the media, there are none);
 *  4. a fresh mount loads the Linux context with the step-1 map;
 *  5. withdraw: erase the Linux context, check every page reads erased;
 *  6. a fresh mount loads the context of step 1 again, same map, same replay.
 */
#include "sftl.h"
#include "sftl_port.h"
#include "rung5.h"

#define R5_MAX_CANDIDATES	8u

struct r5_watch {
	const struct sftl_ops *ops;
	void *hw;
	u32 sb;			/* the only superblock that may be touched */
	bool armed;
	u32 erases, programs, foreign;
};

static int w_read(void *hw, unsigned int bank, unsigned int block, unsigned int page,
		  bool slc, u8 *data, u8 *meta, u8 *status)
{
	struct r5_watch *w = hw;

	return w->ops->read_page(w->hw, bank, block, page, slc, data, meta, status);
}

static int w_erase(void *hw, unsigned int bank, unsigned int block)
{
	struct r5_watch *w = hw;

	if (!w->armed || block != w->sb) {
		sftl_err("rung5: refused erase of bank %u block %u", bank, block);
		w->foreign++;
		return -EPERM;
	}
	w->erases++;
	return w->ops->erase_block(w->hw, bank, block);
}

static int w_program(void *hw, const struct sftl_prog *p, unsigned int n)
{
	struct r5_watch *w = hw;
	unsigned int i;

	for (i = 0; i < n; i++) {
		if (!w->armed || p[i].block != w->sb) {
			sftl_err("rung5: refused program of bank %u block %u page %u",
				 p[i].bank, p[i].block, p[i].page);
			w->foreign++;
			return -EPERM;
		}
	}
	w->programs += n;
	return w->ops->program(w->hw, p, n);
}

static const struct sftl_ops watch_ops = {
	.read_page = w_read,
	.erase_block = w_erase,
	.program = w_program,
};

struct r5_facts {
	u64 digest;
	u32 mapped;
	u32 replay_applied;
	u32 ctx_sb;
	u64 ctx_weave;
	u32 ctx_lbas;
};

static u64 r5_digest(struct sftl *fs, u32 *mapped)
{
	u64 h = 0xcbf29ce484222325ull;
	u32 lba, n = 0;

	for (lba = 0; lba < fs->total_lbas; lba++) {
		u32 v = sftl_map_get(&fs->map, lba);
		unsigned int i;

		if (v != SFTL_UNMAPPED)
			n++;
		for (i = 0; i < 4; i++) {
			h ^= (v >> (8 * i)) & 0xff;
			h *= 0x100000001b3ull;
		}
	}
	*mapped = n;
	return h;
}

static struct sftl *r5_mount(struct r5_watch *w, struct r5_facts *f)
{
	struct sftl *fs = sftl_alloc(sizeof(*fs));
	int ret;

	if (!fs)
		return NULL;
	ret = sftl_init(fs, &watch_ops, w);
	if (!ret)
		ret = sftl_mount(fs);
	if (ret) {
		sftl_err("rung5: mount failed (%d)", ret);
		sftl_exit(fs);
		sftl_free(fs);
		return NULL;
	}
	f->digest = r5_digest(fs, &f->mapped);
	f->replay_applied = fs->replay_applied;
	f->ctx_sb = fs->ctx_sb;
	f->ctx_weave = fs->ctx_weave;
	f->ctx_lbas = fs->ctx_lbas;
	sftl_info("rung5: mount: context sb %u weave %llu, %u LBAs (tree %u), %u mapped, %u replayed, digest %016llx",
		  f->ctx_sb, (unsigned long long)f->ctx_weave, f->ctx_lbas, fs->tree_used,
		  f->mapped, f->replay_applied, (unsigned long long)f->digest);
	return fs;
}

static void r5_unmount(struct sftl *fs)
{
	if (!fs)
		return;
	sftl_writer_stop(fs);
	sftl_exit(fs);
	sftl_free(fs);
}

static int r5_sb_erased(struct sftl *fs, u32 sb, u8 *data, u8 *meta)
{
	u32 i, pg, k;

	for (i = 0; i < fs->shape[sb].width; i++) {
		for (pg = 0; pg < SFTL_MLC_PAGES; pg++) {
			u8 st;
			int ret = fs->ops->read_page(fs->hw, fs->shape[sb].bank[i], sb, pg,
						     false, data, meta, &st);

			if (ret)
				return ret;
			if (st != SFTL_ST_ERASED)
				return 1;
			for (k = 0; k < SFTL_META_PAGE; k++)
				if (meta[k] != 0xff)
					return 1;
			for (k = 0; k < SFTL_PAGE_SIZE; k += 512)
				if (data[k] != 0xff)
					return 1;
		}
	}
	return 0;
}

int rung5_run(const struct rung5_args *a)
{
	struct r5_watch w = { .ops = a->ops, .hw = a->hw, .sb = SFTL_NUM_SB };
	struct r5_facts f0, f1, f2;
	struct sftl *fs = NULL;
	u8 *buf = NULL, *meta = NULL;
	u32 cand, tried = 0, i;
	bool committed = false;
	int ret;

	buf = sftl_alloc(SFTL_PAGE_SIZE);
	meta = sftl_alloc(SFTL_META_PAGE);
	if (!buf || !meta) {
		ret = -ENOMEM;
		goto out;
	}

	/* 1. */
	fs = r5_mount(&w, &f0);
	if (!fs) {
		ret = -EIO;
		goto out;
	}
	for (i = 0; i < fs->n_ctx; i++)
		sftl_info("rung5: context on the media: sb %u, first weave %llu", fs->ctx_list[i],
			  (unsigned long long)fs->probe[fs->ctx_list[i]].first_weave);
	ret = sftl_writer_start(fs, 1);
	if (ret)
		goto out;

	/* 2. */
	for (cand = SFTL_NUM_SB; cand-- > 0 && tried < R5_MAX_CANDIDATES;) {
		int r;

		if (fs->rec[cand].type != SFTL_SB_ERASED || fs->shape[cand].width != SFTL_BANKS)
			continue;
		tried++;
		r = r5_sb_erased(fs, cand, buf, meta);
		if (r < 0) {
			ret = r;
			goto out;
		}
		if (!r) {
			w.sb = cand;
			break;
		}
		sftl_warn("rung5: sb %u is listed erased but holds data; skipped", cand);
	}
	if (w.sb == SFTL_NUM_SB) {
		sftl_err("rung5: no erased four-bank spare");
		ret = -ENOSPC;
		goto out;
	}
	ret = sftl_writer_fence(fs, w.sb, true);
	if (ret)
		goto out;
	sftl_info("rung5: target sb %u, every page reads erased", w.sb);

	/* 3. */
	w.armed = true;
	committed = true;
	fs->wr->dirty = true;
	ret = sftl_checkpoint(fs);
	w.armed = false;
	sftl_info("rung5: commit: %d; %u erases, %u page programs, %u refused", ret,
		  w.erases, w.programs, w.foreign);
	if (!ret && (fs->ctx_sb != w.sb || w.foreign)) {
		sftl_err("rung5: commit went to sb %u", fs->ctx_sb);
		ret = -EIO;
	}
	if (ret)
		goto undo;
	r5_unmount(fs);
	fs = NULL;

	/* 4. */
	fs = r5_mount(&w, &f1);
	if (!fs) {
		ret = -EIO;
		goto undo;
	}
	if (f1.ctx_sb != w.sb || f1.digest != f0.digest || f1.mapped != f0.mapped ||
	    f1.replay_applied) {
		sftl_err("rung5: the Linux context does not load as committed");
		ret = -EIO;
		goto undo;
	}
	sftl_info("rung5: fresh mount loads the Linux context on sb %u with the same map", w.sb);
	ret = 0;

undo:
	/* 5. Withdraw, whatever happened above. */
	if (committed) {
		int r = 0;

		if (!fs) {
			fs = sftl_alloc(sizeof(*fs));
			if (!fs || sftl_init(fs, &watch_ops, &w) || sftl_load_vfl(fs))
				r = -EIO;
		}
		w.armed = true;
		for (i = 0; !r && i < fs->shape[w.sb].width; i++)
			r = watch_ops.erase_block(&w, fs->shape[w.sb].bank[i], w.sb);
		w.armed = false;
		if (!r)
			r = r5_sb_erased(fs, w.sb, buf, meta);
		if (r) {
			sftl_err("rung5: withdraw of sb %u failed (%d): LINUX CONTEXT LEFT ON MEDIA",
				 w.sb, r);
			ret = ret ? ret : -EIO;
			goto out;
		}
		sftl_info("rung5: withdrawn: sb %u erased, every page reads erased", w.sb);
	}
	r5_unmount(fs);
	fs = NULL;
	if (ret)
		goto out;

	/* 6. */
	fs = r5_mount(&w, &f2);
	if (!fs) {
		ret = -EIO;
		goto out;
	}
	if (f2.ctx_sb != f0.ctx_sb || f2.ctx_weave != f0.ctx_weave ||
	    f2.digest != f0.digest || f2.mapped != f0.mapped ||
	    f2.replay_applied != f0.replay_applied) {
		sftl_err("rung5: state after withdraw differs from before");
		ret = -EIO;
		goto out;
	}
	sftl_info("rung5: PASS: back on context sb %u, digest %016llx; touched only sb %u (%u erases, %u programs)",
		  f2.ctx_sb, (unsigned long long)f2.digest, w.sb, w.erases, w.programs);
out:
	r5_unmount(fs);
	sftl_free(buf);
	sftl_free(meta);
	return ret;
}
