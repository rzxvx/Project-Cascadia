// SPDX-License-Identifier: GPL-2.0-only
/*
 * Ladder rung 6 of the P105 NAND specification (section 8.2): iOS boots on a
 * Linux context.  Two actions, portable (offline harness and kernel module):
 *
 * commit: mount, commit a Linux context through sftl_checkpoint() to one
 *         erased spare (a watcher refuses any other block), check that a
 *         fresh mount loads it with the same map, and leave it on the media.
 *
 * check:  read only, after iOS has run: mount and report the context loaded,
 *         every context on the media and what the SB array says of the given
 *         earlier superblocks; rebuild the loaded context from its own tables
 *         and compare it with the media (metadata of every page, base, SB
 *         array, user sequences, read counts, statistics, normalised tree);
 *         read a sample of mapped LBAs (the metadata must name the LBA) and
 *         the LwVM header.
 */
#include "sftl.h"
#include "sftl_port.h"
#include "rung6.h"

#define R6_MAX_CANDIDATES	8u

struct r6_watch {
	const struct sftl_ops *ops;
	void *hw;
	u32 sb;
	bool armed;
	u32 erases, programs, foreign;
};

static int w_read(void *hw, unsigned int bank, unsigned int block, unsigned int page,
		  bool slc, u8 *data, u8 *meta, u8 *status)
{
	struct r6_watch *w = hw;

	return w->ops->read_page(w->hw, bank, block, page, slc, data, meta, status);
}

static int w_erase(void *hw, unsigned int bank, unsigned int block)
{
	struct r6_watch *w = hw;

	if (!w->armed || block != w->sb) {
		sftl_err("rung6: refused erase of bank %u block %u", bank, block);
		w->foreign++;
		return -EPERM;
	}
	w->erases++;
	return w->ops->erase_block(w->hw, bank, block);
}

static int w_program(void *hw, const struct sftl_prog *p, unsigned int n)
{
	struct r6_watch *w = hw;
	unsigned int i;

	for (i = 0; i < n; i++) {
		if (!w->armed || p[i].block != w->sb) {
			sftl_err("rung6: refused program of bank %u block %u page %u",
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

static u64 r6_digest(struct sftl *fs, u32 *mapped)
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

static struct sftl *r6_mount(struct r6_watch *w, u64 *digest, u32 *mapped)
{
	struct sftl *fs = sftl_alloc(sizeof(*fs));
	int ret;

	if (!fs)
		return NULL;
	ret = sftl_init(fs, &watch_ops, w);
	if (!ret)
		ret = sftl_mount(fs);
	if (ret) {
		sftl_err("rung6: mount failed (%d)", ret);
		sftl_exit(fs);
		sftl_free(fs);
		return NULL;
	}
	*digest = r6_digest(fs, mapped);
	sftl_info("rung6: mount: context sb %u weave %llu, %u LBAs (tree %u, %u runs), %u mapped, %u replayed, digest %016llx",
		  fs->ctx_sb, (unsigned long long)fs->ctx_weave, fs->ctx_lbas, fs->tree_used,
		  fs->tree_runs, *mapped, fs->replay_applied, (unsigned long long)*digest);
	return fs;
}

static void r6_unmount(struct sftl *fs)
{
	if (!fs)
		return;
	sftl_writer_stop(fs);
	sftl_exit(fs);
	sftl_free(fs);
}

static int r6_sb_erased(struct sftl *fs, u32 sb, u8 *data, u8 *meta)
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

static int do_commit(struct r6_watch *w)
{
	struct sftl *fs;
	u64 d0, d1;
	u32 m0, m1, cand, tried = 0, i;
	u8 *buf = sftl_alloc(SFTL_PAGE_SIZE), *meta = sftl_alloc(SFTL_META_PAGE);
	int ret;

	if (!buf || !meta) {
		ret = -ENOMEM;
		goto out_free;
	}
	fs = r6_mount(w, &d0, &m0);
	if (!fs) {
		ret = -EIO;
		goto out_free;
	}
	for (i = 0; i < fs->n_ctx; i++)
		sftl_info("rung6: context on the media: sb %u, first weave %llu", fs->ctx_list[i],
			  (unsigned long long)fs->probe[fs->ctx_list[i]].first_weave);
	ret = sftl_writer_start(fs, 1);
	if (ret)
		goto out;
	for (cand = SFTL_NUM_SB; cand-- > 0 && tried < R6_MAX_CANDIDATES;) {
		int r;

		if (fs->rec[cand].type != SFTL_SB_ERASED || fs->shape[cand].width != SFTL_BANKS)
			continue;
		tried++;
		r = r6_sb_erased(fs, cand, buf, meta);
		if (r < 0) {
			ret = r;
			goto out;
		}
		if (!r) {
			w->sb = cand;
			break;
		}
		sftl_warn("rung6: sb %u is listed erased but holds data; skipped", cand);
	}
	if (w->sb == SFTL_NUM_SB) {
		ret = -ENOSPC;
		goto out;
	}
	ret = sftl_writer_fence(fs, w->sb, true);
	if (ret)
		goto out;
	w->armed = true;
	fs->wr->dirty = true;
	ret = sftl_checkpoint(fs);
	w->armed = false;
	sftl_info("rung6: commit to sb %u: %d; %u erases, %u page programs, %u refused",
		  w->sb, ret, w->erases, w->programs, w->foreign);
	if (!ret && (fs->ctx_sb != w->sb || w->foreign))
		ret = -EIO;
	r6_unmount(fs);
	fs = NULL;
	if (ret)
		goto out;

	fs = r6_mount(w, &d1, &m1);
	if (!fs) {
		ret = -EIO;
		goto out;
	}
	if (fs->ctx_sb != w->sb || d1 != d0 || m1 != m0 || fs->replay_applied) {
		sftl_err("rung6: the committed context does not load as committed");
		ret = -EIO;
		goto out;
	}
	sftl_info("rung6: COMMITTED: Linux context on sb %u loads with the same map; left on the media",
		  w->sb);
out:
	r6_unmount(fs);
out_free:
	sftl_free(buf);
	sftl_free(meta);
	return ret;
}

/* Normalised (vba, span) runs of a tree image of n LBAs. */
static u32 tree_flatten(const u8 *img, u32 nlbas, u32 *vba, u32 *span, u32 max)
{
	u32 n = 0, t, r;

	for (t = 0; t < nlbas; t++) {
		const u8 *p = img + (size_t)t * SFTL_LBA_SIZE;

		if (sftl_get32(p + 4) != 0xfffffff0u)
			break;
		for (r = 0; r < 511; r++) {
			u32 v = sftl_get32(p + 8 + 8 * r), s = sftl_get32(p + 12 + 8 * r);

			if (v == 0xffffffffu)
				break;
			if (n && ((v == SFTL_UNMAPPED && vba[n - 1] == SFTL_UNMAPPED) ||
				  (v != SFTL_UNMAPPED && vba[n - 1] != SFTL_UNMAPPED &&
				   vba[n - 1] + span[n - 1] == v))) {
				span[n - 1] += s;
				continue;
			}
			if (n == max)
				return max + 1;
			vba[n] = v;
			span[n] = s;
			n++;
		}
	}
	return n;
}

/* Rebuild the loaded context from its tables and compare with the media. */
static int golden(struct sftl *fs)
{
	u32 sb = fs->ctx_sb, nl = 0, i, bad_meta = 0, bad_base = 0, bad_tab = 0, bad_stats = 0;
	u32 maxruns = SFTL_TREE_MAX_LBAS * 511, na, nb, *va, *sa, *vb, *sb2;
	u8 *ours = sftl_alloc(SFTL_CTX_MAX_PAGES * (SFTL_PAGE_SIZE + SFTL_META_PAGE));
	u8 *media = sftl_alloc((size_t)SFTL_CTX_MAX_PAGES * SFTL_PAGE_SIZE);
	u8 *ours_meta = ours ? ours + SFTL_CTX_MAX_PAGES * SFTL_PAGE_SIZE : NULL;
	u8 m16[SFTL_META_LBA];
	int ret = 0;

	va = sftl_alloc(4 * maxruns * sizeof(u32));
	if (!ours || !media || !va) {
		ret = -ENOMEM;
		goto out;
	}
	sa = va + maxruns;
	vb = sa + maxruns;
	sb2 = vb + maxruns;
	ret = sftl_ctx_build(fs, sb, fs->ctx_weave, ours, ours_meta, &nl);
	if (ret)
		goto out;
	if (fs->ctx_lbas > SFTL_CTX_TREE_VBA + SFTL_TREE_MAX_LBAS) {
		ret = -E2BIG;
		goto out;
	}
	for (i = 0; i < fs->ctx_lbas; i++) {
		ret = sftl_read_vba(fs, sftl_vba(sb, i), media + (size_t)i * SFTL_LBA_SIZE, m16);
		if (ret)
			goto out;
		if (i < nl && memcmp(m16, ours_meta + i * SFTL_META_LBA, SFTL_META_LBA))
			bad_meta++;
	}
	if (memcmp(media, ours, 4 * SFTL_LBA_SIZE))
		bad_base++;
	if (memcmp(media + SFTL_PAGE_SIZE, ours + SFTL_PAGE_SIZE, SFTL_PAGE_SIZE))
		bad_stats++;
	if (memcmp(media + 2 * SFTL_PAGE_SIZE, ours + 2 * SFTL_PAGE_SIZE, 8 * SFTL_NUM_SB) ||
	    memcmp(media + 3 * SFTL_PAGE_SIZE, ours + 3 * SFTL_PAGE_SIZE, 16 * SFTL_NUM_SB) ||
	    memcmp(media + 5 * SFTL_PAGE_SIZE, ours + 5 * SFTL_PAGE_SIZE, 4 * SFTL_NUM_SB))
		bad_tab++;
	na = tree_flatten(media + 6 * SFTL_PAGE_SIZE, fs->ctx_lbas - SFTL_CTX_TREE_VBA, va, sa,
			  maxruns);
	nb = tree_flatten(ours + 6 * SFTL_PAGE_SIZE, nl - SFTL_CTX_TREE_VBA, vb, sb2, maxruns);
	sftl_info("rung6: golden: context %u LBAs on media, %u rebuilt; metadata differs on %u LBAs; base %s, statistics %s, tables %s; tree %u vs %u normalised runs%s",
		  fs->ctx_lbas, nl, bad_meta, bad_base ? "DIFFERS" : "same",
		  bad_stats ? "DIFFER" : "same", bad_tab ? "DIFFER" : "same", na, nb,
		  na == nb && !memcmp(va, vb, na * 4) && !memcmp(sa, sb2, na * 4) ?
		  ", identical" : ", DIFFERENT");
	if (nl != fs->ctx_lbas || bad_meta || bad_base || bad_tab || bad_stats || na != nb ||
	    memcmp(va, vb, na * 4) || memcmp(sa, sb2, na * 4))
		ret = 1;
out:
	sftl_free(ours);
	sftl_free(media);
	sftl_free(va);
	return ret;
}

static u32 r6_rand(u32 *x)
{
	*x ^= *x << 13;
	*x ^= *x >> 17;
	*x ^= *x << 5;
	return *x;
}

static int do_check(struct r6_watch *w, const struct rung6_args *a)
{
	struct sftl *fs;
	struct sftl_lwvm lv;
	u64 d;
	u32 m, i, x = 0x6b1d2c3e, checked = 0, bad = 0;
	u8 *buf = sftl_alloc(SFTL_LBA_SIZE);
	int ret, g;

	if (!buf)
		return -ENOMEM;
	/* Load the context without replay first: the golden compares it as stored. */
	fs = sftl_alloc(sizeof(*fs));
	ret = fs ? sftl_init(fs, &watch_ops, w) : -ENOMEM;
	if (!ret)
		ret = sftl_load_vfl(fs);
	if (!ret)
		ret = sftl_scan(fs);
	if (!ret)
		ret = sftl_load_context(fs);
	if (ret) {
		sftl_err("rung6: context load failed (%d)", ret);
		r6_unmount(fs);
		sftl_free(buf);
		return ret;
	}
	g = golden(fs);
	ret = sftl_replay(fs);
	if (ret) {
		sftl_err("rung6: replay failed (%d)", ret);
		r6_unmount(fs);
		sftl_free(buf);
		return ret;
	}
	d = r6_digest(fs, &m);
	sftl_info("rung6: mount: context sb %u weave %llu, %u LBAs (tree %u, %u runs), %u mapped, %u replayed, digest %016llx",
		  fs->ctx_sb, (unsigned long long)fs->ctx_weave, fs->ctx_lbas, fs->tree_used,
		  fs->tree_runs, m, fs->replay_applied, (unsigned long long)d);
	for (i = 0; i < fs->n_ctx; i++)
		sftl_info("rung6: context on the media: sb %u, first weave %llu%s", fs->ctx_list[i],
			  (unsigned long long)fs->probe[fs->ctx_list[i]].first_weave,
			  fs->ctx_list[i] == fs->ctx_sb ? " (loaded)" : "");
	for (i = 0; i < SFTL_NUM_SB; i++)
		if (fs->rec[i].type == SFTL_SB_CONTEXT)
			sftl_info("rung6: SB array lists sb %u as a context", i);
	for (i = 0; i < RUNG6_WATCH && a->watch_sb[i]; i++) {
		u32 s = a->watch_sb[i];

		if (s >= SFTL_NUM_SB)
			continue;
		sftl_info("rung6: sb %u: first page type %02x weave %llu; SB array type %u valid %u erases %u",
			  s, fs->probe[s].first_type, (unsigned long long)fs->probe[s].first_weave,
			  fs->rec[s].type, fs->rec[s].valid, fs->rec[s].erases);
	}

	ret = g < 0 ? g : 0;

	for (i = 0; !ret && checked < a->samples && i < a->samples * 64; i++) {
		u32 lba = r6_rand(&x) % fs->total_lbas;

		if (sftl_map_get(&fs->map, lba) == SFTL_UNMAPPED)
			continue;
		checked++;
		if (sftl_read_lba(fs, lba, buf, NULL))
			bad++;
	}
	sftl_info("rung6: %u random mapped LBAs read, %u failed (metadata must name the LBA)",
		  checked, bad);
	if (!ret) {
		int r = sftl_lwvm_load(fs, &lv);

		sftl_info("rung6: LwVM: %d, %u partitions", r, r ? 0 : lv.nparts);
		for (i = 0; !r && i < lv.nparts; i++)
			sftl_info("rung6:   %s: %u LBAs", lv.part[i].name, lv.part[i].nlbas);
		if (r)
			ret = r;
	}
	if (!ret && (g || bad))
		ret = -EIO;
	sftl_info("rung6: check %s", ret ? "FOUND DIFFERENCES" : "clean");
	r6_unmount(fs);
	sftl_free(buf);
	return ret;
}

int rung6_run(const struct rung6_args *a)
{
	struct r6_watch w = { .ops = a->ops, .hw = a->hw, .sb = SFTL_NUM_SB };

	if (a->commit)
		return do_commit(&w);
	return do_check(&w, a);
}
