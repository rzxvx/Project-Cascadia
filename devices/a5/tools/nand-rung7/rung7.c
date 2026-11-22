// SPDX-License-Identifier: GPL-2.0-only
/*
 * Ladder rung 7 of the P105 NAND specification (section 8.2): a real reclaim
 * (GC) and its commit, left on the media for iOS to boot on.  Portable:
 * offline harness and kernel module.
 *
 *  1. mount, start the writer (verify every page), map digest;
 *  2. ask the writer which victims a reclaim would take and which live LBAs
 *     it would move; read each of those LBAs: data hash, type and user;
 *  3. run the reclaim through sftl_gc(): move, close, commit, erase victims.
 *     A watcher lets erases through only for victims, spares (erased or
 *     waiting for erase with nothing valid) and retired contexts, and
 *     programs only into blocks this run has erased;
 *  4. in memory and after a fresh mount: every moved LBA reads the same hash,
 *     type and user from outside the victims; every other LBA maps where it
 *     did; the victims read erased in full.
 */
#include "sftl.h"
#include "sftl_port.h"
#include "rung7.h"

#define R7_MAX_LBAS	(SFTL_MAX_VICTIMS * 4096u)	/* the largest SB, reference package */

struct r7_watch {
	const struct sftl_ops *ops;
	void *hw;
	bool armed;
	u8 erase_ok[(SFTL_MAX_SB + 7) / 8];
	u8 erased[SFTL_BANKS][(SFTL_MAX_BLOCKS + 7) / 8];
	u32 erases, programs, refused;
};

static bool bit(const u8 *m, u32 i)
{
	return (m[i / 8] >> (i % 8)) & 1;
}

static void set_bit8(u8 *m, u32 i)
{
	m[i / 8] |= 1 << (i % 8);
}

static int w_read(void *hw, unsigned int bank, unsigned int block, unsigned int page,
		  bool slc, u8 *data, u8 *meta, u8 *status)
{
	struct r7_watch *w = hw;

	return w->ops->read_page(w->hw, bank, block, page, slc, data, meta, status);
}

static int w_erase(void *hw, unsigned int bank, unsigned int block)
{
	struct r7_watch *w = hw;
	int ret;

	if (!w->armed || block >= SFTL_NUM_SB || !bit(w->erase_ok, block)) {
		sftl_err("rung7: refused erase of bank %u block %u", bank, block);
		w->refused++;
		return -EPERM;
	}
	ret = w->ops->erase_block(w->hw, bank, block);
	if (!ret) {
		set_bit8(w->erased[bank], block);
		w->erases++;
	}
	return ret;
}

static int w_program(void *hw, const struct sftl_prog *p, unsigned int n)
{
	struct r7_watch *w = hw;
	unsigned int i;

	for (i = 0; i < n; i++) {
		if (!w->armed || p[i].bank >= SFTL_BANKS ||
		    !bit(w->erased[p[i].bank], p[i].block)) {
			sftl_err("rung7: refused program of bank %u block %u page %u",
				 p[i].bank, p[i].block, p[i].page);
			w->refused++;
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

static u64 fnv(u64 h, const u8 *p, u32 n)
{
	u32 i;

	for (i = 0; i < n; i++) {
		h ^= p[i];
		h *= 0x100000001b3ull;
	}
	return h;
}

/* Digest of the map over every LBA not in the moved set, and the mapped count. */
static u64 rest_digest(struct sftl *fs, const u8 *moved, u32 *mapped)
{
	u64 h = 0xcbf29ce484222325ull;
	u32 lba, n = 0;

	for (lba = 0; lba < fs->total_lbas; lba++) {
		u32 v = sftl_map_get(&fs->map, lba);
		u8 b[4];

		if (v != SFTL_UNMAPPED)
			n++;
		if (bit(moved, lba))
			continue;
		sftl_put32(b, v);
		h = fnv(h, b, 4);
	}
	*mapped = n;
	return h;
}

struct r7_lba {
	u32 lba;
	u64 hash;
	u8 type;
	u32 user;
};

static int read_set(struct sftl *fs, struct r7_lba *set, u32 n, u8 *buf, const u32 *victims,
		    u32 nv, bool check)
{
	u32 i, k, bad = 0;

	for (i = 0; i < n; i++) {
		u8 m16[SFTL_META_LBA];
		u32 sb = sftl_sb_of(sftl_map_get(&fs->map, set[i].lba));
		u64 h;
		int ret = sftl_read_lba(fs, set[i].lba, buf, m16);

		if (ret)
			return ret;
		h = fnv(0xcbf29ce484222325ull, buf, SFTL_LBA_SIZE);
		if (!check) {
			set[i].hash = h;
			set[i].type = m16[0];
			set[i].user = sftl_get32(m16 + 12);
			continue;
		}
		for (k = 0; k < nv; k++)
			if (sb == victims[k])
				break;
		if (k < nv || h != set[i].hash || m16[0] != set[i].type ||
		    sftl_get32(m16 + 12) != set[i].user) {
			if (bad++ < 8)
				sftl_err("rung7: LBA %u: sb %u, hash %s, type %02x/%02x, user %08x/%08x",
					 set[i].lba, sb, h == set[i].hash ? "same" : "DIFFERS",
					 m16[0], set[i].type, sftl_get32(m16 + 12), set[i].user);
		}
	}
	return bad ? -EIO : 0;
}

static int sb_erased(struct sftl *fs, u32 sb, u8 *data, u8 *meta)
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
		}
	}
	return 0;
}

static struct sftl *r7_mount(struct r7_watch *w)
{
	struct sftl *fs = sftl_alloc(sizeof(*fs));
	int ret;

	if (!fs)
		return NULL;
	ret = sftl_init(fs, &watch_ops, w);
	if (!ret)
		ret = sftl_mount(fs);
	if (ret) {
		sftl_err("rung7: mount failed (%d)", ret);
		sftl_exit(fs);
		sftl_free(fs);
		return NULL;
	}
	return fs;
}

static void r7_unmount(struct sftl *fs)
{
	if (!fs)
		return;
	sftl_writer_stop(fs);
	sftl_exit(fs);
	sftl_free(fs);
}

int rung7_run(const struct rung7_args *a)
{
	struct r7_watch *w = sftl_alloc(sizeof(*w));
	struct sftl *fs = NULL;
	struct r7_lba *set = NULL;
	u32 *lbas = NULL, victims[SFTL_MAX_VICTIMS], nv = 0, n = 0, i, sb;
	u8 *moved = NULL, *buf = sftl_alloc(SFTL_PAGE_SIZE), *meta = sftl_alloc(SFTL_META_PAGE);
	u64 d0, d1;
	u32 m0, m1, ctx0, ctx1;
	int ret;

	if (!w || !buf || !meta) {
		ret = -ENOMEM;
		goto out;
	}
	w->ops = a->ops;
	w->hw = a->hw;

	/* 1. */
	fs = r7_mount(w);
	if (!fs) {
		ret = -EIO;
		goto out;
	}
	ret = sftl_writer_start(fs, 1);
	if (ret)
		goto out;
	ctx0 = fs->ctx_sb;
	moved = sftl_alloc((fs->total_lbas + 7) / 8);
	lbas = sftl_alloc(R7_MAX_LBAS * sizeof(*lbas));
	set = sftl_alloc(R7_MAX_LBAS * sizeof(*set));
	if (!moved || !lbas || !set) {
		ret = -ENOMEM;
		goto out;
	}

	/* 2. */
	ret = sftl_gc_plan(fs, victims, &nv, lbas, &n);
	if (ret) {
		sftl_err("rung7: no reclaim possible (%d)", ret);
		goto out;
	}
	for (i = 0; i < nv; i++)
		sftl_info("rung7: victim sb %u: valid %u, BTOC flags %#x, first weave %llu, erases %u",
			  victims[i], fs->rec[victims[i]].valid, fs->rec[victims[i]].btoc_flags,
			  (unsigned long long)fs->probe[victims[i]].first_weave,
			  fs->rec[victims[i]].erases);
	for (i = 0; i < n; i++) {
		set[i].lba = lbas[i];
		set_bit8(moved, lbas[i]);
	}
	ret = read_set(fs, set, n, buf, victims, nv, false);
	if (ret)
		goto out;
	d0 = rest_digest(fs, moved, &m0);
	sftl_info("rung7: context sb %u; %u live LBAs to move (first %u, last %u); rest digest %016llx, %u mapped",
		  ctx0, n, n ? lbas[0] : 0, n ? lbas[n - 1] : 0, (unsigned long long)d0, m0);

	/* 3. What may be erased: victims, spares, retired contexts. */
	for (sb = 0; sb < SFTL_NUM_SB; sb++) {
		const struct sftl_sb_rec *r = &fs->rec[sb];

		if (r->type == SFTL_SB_ERASED ||
		    (r->type == SFTL_SB_AWAIT_ERASE && r->valid == 0))
			set_bit8(w->erase_ok, sb);
	}
	for (i = 0; i < nv; i++)
		set_bit8(w->erase_ok, victims[i]);
	for (i = 0; i < fs->n_ctx; i++)
		if (fs->ctx_list[i] != ctx0)
			set_bit8(w->erase_ok, fs->ctx_list[i]);
	w->armed = true;
	ret = sftl_gc(fs);
	w->armed = false;
	sftl_info("rung7: reclaim + commit: %d; context now sb %u; %u erases, %u page programs, %u refused",
		  ret, fs->ctx_sb, w->erases, w->programs, w->refused);
	ctx1 = fs->ctx_sb;
	for (i = 0; i < nv; i++)
		if (ctx1 == victims[i])
			ret = ret ? ret : -EIO;
	if (!ret && (w->refused || ctx1 == ctx0))
		ret = -EIO;
	if (ret)
		goto out;

	/* 4. In memory, then from the media. */
	ret = read_set(fs, set, n, buf, victims, nv, true);
	if (ret)
		goto out;
	r7_unmount(fs);
	fs = r7_mount(w);
	if (!fs) {
		ret = -EIO;
		goto out;
	}
	ret = read_set(fs, set, n, buf, victims, nv, true);
	if (ret)
		goto out;
	d1 = rest_digest(fs, moved, &m1);
	sftl_info("rung7: remount: context sb %u, %u replayed; moved LBAs read the same; rest digest %016llx, %u mapped",
		  fs->ctx_sb, fs->replay_applied, (unsigned long long)d1, m1);
	if (fs->ctx_sb != ctx1 || fs->replay_applied) {
		sftl_err("rung7: the remount did not load the new context sb %u", ctx1);
		ret = -EIO;
		goto out;
	}
	if (d1 != d0 || m1 != m0) {
		sftl_err("rung7: the map outside the moved LBAs changed");
		ret = -EIO;
		goto out;
	}
	for (i = 0; i < nv; i++) {
		int r = sb_erased(fs, victims[i], buf, meta);

		if (r) {
			sftl_err("rung7: victim sb %u does not read erased (%d)", victims[i], r);
			ret = -EIO;
			goto out;
		}
	}
	sftl_info("rung7: PASS: %u victims erased, %u LBAs moved intact, rest of the map unchanged; left on the media",
		  nv, n);
out:
	r7_unmount(fs);
	sftl_free(w);
	sftl_free(set);
	sftl_free(lbas);
	sftl_free(moved);
	sftl_free(buf);
	sftl_free(meta);
	return ret;
}
