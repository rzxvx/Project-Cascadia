// SPDX-License-Identifier: GPL-2.0-only
/*
 * Ladder rung 4 of the P105 NAND specification (section 8.2): a whole context
 * written to a spare superblock, verified, and erased again.  Portable: built
 * into the offline harness and into a kernel test module.
 *
 *  1. mount; note the context, the map digest and the tables;
 *  2. pick a four-bank superblock listed erased whose every page reads erased;
 *  3. erase it, mark it a context, write a context of the mounted state
 *     with the writer's own code (sftl_context_write, which also verifies
 *     every page against what it programmed);
 *  4. a fresh mount must load that context, with the same map digest and the
 *     same SB array, user sequences, read counts and statistics;
 *  5. erase the superblock and check that every page reads erased;
 *  6. a fresh mount loads the earlier context again with the step-1 digest.
 *
 * Between steps 3 and 5 the new context is the newest on the media: a mount
 * in that window, by Linux or iOS, would load it.  It describes the state
 * the media holds, but the window is kept to the test's own run.
 */
#include "sftl.h"
#include "sftl_port.h"
#include "rung4.h"

#define R4_MAX_CANDIDATES	8u

struct r4_facts {
	u64 digest;
	u32 mapped;
	u32 replay_applied;
	u32 ctx_sb;
	u64 ctx_weave;
	u32 ctx_lbas;
	u32 tree_used;
};

static u64 r4_digest(struct sftl *fs, u32 *mapped)
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

static struct sftl *r4_mount(const struct rung4_args *a, struct r4_facts *f)
{
	struct sftl *fs = sftl_alloc(sizeof(*fs));
	int ret;

	if (!fs)
		return NULL;
	ret = sftl_init(fs, a->ops, a->hw);
	if (!ret)
		ret = sftl_mount(fs);
	if (ret) {
		sftl_err("rung4: mount failed (%d)", ret);
		sftl_exit(fs);
		sftl_free(fs);
		return NULL;
	}
	f->digest = r4_digest(fs, &f->mapped);
	f->replay_applied = fs->replay_applied;
	f->ctx_sb = fs->ctx_sb;
	f->ctx_weave = fs->ctx_weave;
	f->ctx_lbas = fs->ctx_lbas;
	f->tree_used = fs->tree_used;
	sftl_info("rung4: mount: context sb %u weave %llu, %u LBAs (tree %u), %u mapped, %u replayed, digest %016llx",
		  f->ctx_sb, (unsigned long long)f->ctx_weave, f->ctx_lbas, f->tree_used,
		  f->mapped, f->replay_applied, (unsigned long long)f->digest);
	return fs;
}

static void r4_unmount(struct sftl *fs)
{
	if (!fs)
		return;
	sftl_writer_stop(fs);
	sftl_exit(fs);
	sftl_free(fs);
}

static int r4_sb_erased(struct sftl *fs, u32 sb, u8 *data, u8 *meta)
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

static int r4_erase(struct sftl *fs, u32 sb)
{
	u32 i;
	int ret = 0;

	for (i = 0; i < fs->shape[sb].width && !ret; i++)
		ret = fs->ops->erase_block(fs->hw, fs->shape[sb].bank[i], sb);
	return ret;
}

int rung4_run(const struct rung4_args *a)
{
	struct r4_facts f0, f1, f2;
	struct sftl *fs = NULL, *chk = NULL;
	u8 *buf = NULL, *meta = NULL;
	struct sftl_sb_rec *rec = NULL;
	struct sftl_useq *useq = NULL;
	u32 *reads = NULL;
	u8 *stats = NULL;
	u32 sb = SFTL_NUM_SB, tried = 0, cand, i;
	u64 weave = 0;
	u32 nlbas = 0;
	bool written = false;
	int ret;

	buf = sftl_alloc(SFTL_PAGE_SIZE);
	meta = sftl_alloc(SFTL_META_PAGE);
	rec = sftl_alloc(SFTL_NUM_SB * sizeof(*rec));
	useq = sftl_alloc(SFTL_NUM_SB * sizeof(*useq));
	reads = sftl_alloc(SFTL_NUM_SB * sizeof(*reads));
	stats = sftl_alloc(SFTL_PAGE_SIZE);
	if (!buf || !meta || !rec || !useq || !reads || !stats) {
		ret = -ENOMEM;
		goto out;
	}

	/* 1. */
	fs = r4_mount(a, &f0);
	if (!fs) {
		ret = -EIO;
		goto out;
	}
	ret = sftl_writer_start(fs, 1);
	if (ret)
		goto out;

	/* 2. */
	for (cand = SFTL_NUM_SB; cand-- > 0 && tried < R4_MAX_CANDIDATES;) {
		int r;

		if (fs->rec[cand].type != SFTL_SB_ERASED || fs->shape[cand].width != SFTL_BANKS)
			continue;
		tried++;
		r = r4_sb_erased(fs, cand, buf, meta);
		if (r < 0) {
			ret = r;
			goto out;
		}
		if (!r) {
			sb = cand;
			break;
		}
		sftl_warn("rung4: sb %u is listed erased but holds data; skipped", cand);
	}
	if (sb == SFTL_NUM_SB) {
		sftl_err("rung4: no erased four-bank spare");
		ret = -ENOSPC;
		goto out;
	}
	ret = sftl_writer_fence(fs, sb);
	if (ret)
		goto out;
	sftl_info("rung4: target sb %u (erases %u), every page reads erased", sb,
		  fs->rec[sb].erases);

	/* 3. As a commit does: erase, mark, write. */
	written = true;
	ret = r4_erase(fs, sb);
	if (ret) {
		sftl_err("rung4: erase before write: %d", ret);
		goto undo;
	}
	fs->rec[sb].erases++;
	fs->rec[sb].type = SFTL_SB_CONTEXT;
	fs->rec[sb].btoc_flags = 0;
	fs->rec[sb].valid = 0;
	ret = sftl_context_write(fs, sb);
	if (ret) {
		sftl_err("rung4: context write: %d", ret);
		goto undo;
	}
	weave = fs->ctx_weave;
	nlbas = fs->ctx_lbas;
	memcpy(rec, fs->rec, SFTL_NUM_SB * sizeof(*rec));
	memcpy(useq, fs->useq, SFTL_NUM_SB * sizeof(*useq));
	memcpy(reads, fs->reads, SFTL_NUM_SB * sizeof(*reads));
	memcpy(stats, fs->stats, SFTL_PAGE_SIZE);
	sftl_info("rung4: context on sb %u, weave %llu, %u LBAs; every page verified",
		  sb, (unsigned long long)weave, nlbas);

	/* 4. */
	chk = r4_mount(a, &f1);
	if (!chk) {
		ret = -EIO;
		goto undo;
	}
	if (f1.ctx_sb != sb || f1.ctx_weave != weave || f1.ctx_lbas != nlbas) {
		sftl_err("rung4: the fresh mount did not load the written context as written");
		ret = -EIO;
		goto undo;
	}
	if (f1.digest != f0.digest || f1.mapped != f0.mapped) {
		sftl_err("rung4: map from the written context differs");
		ret = -EIO;
		goto undo;
	}
	for (i = 0; i < SFTL_NUM_SB; i++) {
		if (memcmp(&chk->rec[i], &rec[i], sizeof(*rec)) ||
		    memcmp(&chk->useq[i], &useq[i], sizeof(*useq)) ||
		    chk->reads[i] != reads[i]) {
			sftl_err("rung4: tables differ at sb %u", i);
			ret = -EIO;
			goto undo;
		}
	}
	if (memcmp(chk->stats, stats, SFTL_PAGE_SIZE)) {
		sftl_err("rung4: statistics section differs");
		ret = -EIO;
		goto undo;
	}
	sftl_info("rung4: fresh mount loads it: map, SB array, user sequences, read counts, statistics equal");
	ret = 0;

undo:
	r4_unmount(chk);
	chk = NULL;
	/* 5. Whatever happened above, the superblock goes back to erased. */
	if (written) {
		int r = r4_erase(fs, sb);

		if (!r)
			r = r4_sb_erased(fs, sb, buf, meta);
		if (r) {
			sftl_err("rung4: erase of sb %u failed (%d): CONTEXT LEFT ON MEDIA", sb, r);
			ret = ret ? ret : -EIO;
			goto out;
		}
		sftl_info("rung4: sb %u erased, every page reads erased", sb);
	}
	r4_unmount(fs);
	fs = NULL;
	if (ret)
		goto out;

	/* 6. */
	fs = r4_mount(a, &f2);
	if (!fs) {
		ret = -EIO;
		goto out;
	}
	if (f2.digest != f0.digest || f2.mapped != f0.mapped || f2.ctx_sb != f0.ctx_sb ||
	    f2.ctx_weave != f0.ctx_weave || f2.replay_applied != f0.replay_applied) {
		sftl_err("rung4: state after undo differs from before");
		ret = -EIO;
		goto out;
	}
	sftl_info("rung4: PASS: back on context sb %u, digest %016llx", f2.ctx_sb,
		  (unsigned long long)f2.digest);
out:
	r4_unmount(chk);
	r4_unmount(fs);
	sftl_free(buf);
	sftl_free(meta);
	sftl_free(rec);
	sftl_free(useq);
	sftl_free(reads);
	sftl_free(stats);
	return ret;
}
