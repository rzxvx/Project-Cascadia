// SPDX-License-Identifier: GPL-2.0-only
/*
 * Ladder rung 3 of the P105 NAND specification (section 8.2): FTL writes that
 * are undone.  Portable: built into the offline harness against the dump and
 * into a kernel test module against the PPN driver.
 *
 *  1. mount; digest of the whole map;
 *  2. pick a four-bank superblock that the SB array calls erased and whose
 *     every page reads erased on the media; fence the writer to it (no commit,
 *     no reclaim, no other superblock);
 *  3. rewrite a run of mapped LBAs with their own content (unaligned at both
 *     ends, so sibling slots are carried), then seal: PAD fill and BTOC;
 *  4. the BTOC read back through the mount path equals the one written and the
 *     one rebuilt from every data page's metadata; every LBA reads its data;
 *  5. a fresh mount replays the new superblock and maps the run into it;
 *  6. erase the superblock, check every page reads erased;
 *  7. a fresh mount gives the map digest of step 1.
 *
 * The context on the media is never written, so after step 6 the media holds
 * what it held before, except for the erase counts of four blocks.
 */
#include "sftl.h"
#include "sftl_port.h"
#include "rung3.h"

#define R3_MAX_CANDIDATES	8u

struct rung3_mount_facts {
	u64 digest;
	u32 mapped;
	u32 replay_applied;
	u32 ctx_sb;
	u64 ctx_weave;
};

static u64 map_digest(struct sftl *fs, u32 *mapped)
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

static struct sftl *mount_one(const struct rung3_args *a, struct rung3_mount_facts *f)
{
	struct sftl *fs = sftl_alloc(sizeof(*fs));
	int ret;

	if (!fs)
		return NULL;
	ret = sftl_init(fs, a->ops, a->hw);
	if (!ret)
		ret = sftl_mount(fs);
	if (ret) {
		sftl_err("rung3: mount failed (%d)", ret);
		sftl_exit(fs);
		sftl_free(fs);
		return NULL;
	}
	f->digest = map_digest(fs, &f->mapped);
	f->replay_applied = fs->replay_applied;
	f->ctx_sb = fs->ctx_sb;
	f->ctx_weave = fs->ctx_weave;
	sftl_info("rung3: mount: context sb %u weave %llu, %u mapped, %u replayed, digest %016llx",
		  f->ctx_sb, (unsigned long long)f->ctx_weave, f->mapped, f->replay_applied,
		  (unsigned long long)f->digest);
	return fs;
}

static void unmount_one(struct sftl *fs)
{
	if (!fs)
		return;
	sftl_writer_stop(fs);
	sftl_exit(fs);
	sftl_free(fs);
}

/* Every page of every block of the superblock reads erased (status and bytes). */
static int sb_reads_erased(struct sftl *fs, u32 sb, u8 *data, u8 *meta)
{
	u32 i, pg, k;

	for (i = 0; i < fs->shape[sb].width; i++) {
		u32 bank = fs->shape[sb].bank[i];

		for (pg = 0; pg < SFTL_MLC_PAGES; pg++) {
			u8 st;
			int ret = fs->ops->read_page(fs->hw, bank, sb, pg, false, data, meta, &st);

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

static int pick_sb(struct sftl *fs, u8 *data, u8 *meta, u32 *out)
{
	u32 sb, tried = 0;

	for (sb = SFTL_NUM_SB; sb-- > 0 && tried < R3_MAX_CANDIDATES;) {
		int r;

		if (fs->rec[sb].type != SFTL_SB_ERASED || fs->shape[sb].width != SFTL_BANKS)
			continue;
		tried++;
		r = sb_reads_erased(fs, sb, data, meta);
		if (r < 0)
			return r;
		if (r == 0) {
			*out = sb;
			return 0;
		}
		sftl_warn("rung3: sb %u is listed erased but holds data; skipped", sb);
	}
	return -ENOSPC;
}

/* First run of n mapped LBAs at or after from. */
static int find_run(struct sftl *fs, u32 from, u32 n, u32 *start)
{
	u32 lba, len = 0;

	for (lba = from; lba < fs->reported_lbas; lba++) {
		if (sftl_map_get(&fs->map, lba) == SFTL_UNMAPPED) {
			len = 0;
			continue;
		}
		if (++len == n) {
			*start = lba + 1 - n;
			return 0;
		}
	}
	return -ENOENT;
}

int rung3_run(const struct rung3_args *a)
{
	struct rung3_mount_facts f0, f1, f2;
	struct sftl *fs = NULL;
	u8 *src = NULL, *buf = NULL, *img = NULL, *rebuilt = NULL, *meta = NULL;
	struct sftl_btoc_entry *ent = NULL;
	u32 *vbas = NULL;
	u32 sb = 0, start = 0, i, nb = 0, nb2, nent = 0, cap, n = a->nlbas;
	bool sb_written = false;
	int ret;

	if (n < 8 || n > 3000)
		return -EINVAL;
	src = sftl_alloc((size_t)n * SFTL_LBA_SIZE);
	buf = sftl_alloc(SFTL_PAGE_SIZE);
	meta = sftl_alloc(SFTL_META_PAGE);
	img = sftl_alloc(2 * SFTL_PAGE_SIZE);
	rebuilt = sftl_alloc(2 * SFTL_PAGE_SIZE);
	ent = sftl_alloc(2 * SFTL_PAGE_SIZE / 16 * sizeof(*ent));
	vbas = sftl_alloc(n * sizeof(*vbas));
	if (!src || !buf || !meta || !img || !rebuilt || !ent || !vbas) {
		ret = -ENOMEM;
		goto out;
	}

	/* 1. */
	fs = mount_one(a, &f0);
	if (!fs) {
		ret = -EIO;
		goto out;
	}

	/* 2. */
	ret = sftl_writer_start(fs, 1);
	if (ret) {
		sftl_err("rung3: writer start %d", ret);
		goto out;
	}
	ret = pick_sb(fs, buf, meta, &sb);
	if (ret) {
		sftl_err("rung3: no erased four-bank spare (%d)", ret);
		goto out;
	}
	ret = sftl_writer_fence(fs, sb);
	if (ret)
		goto out;
	cap = sftl_capacity(fs, sb);
	sftl_info("rung3: target sb %u (erases %u, capacity %u VBAs), every page reads erased",
		  sb, fs->rec[sb].erases, cap);

	/* 3. */
	ret = find_run(fs, a->search_from, n, &start);
	if (ret) {
		sftl_err("rung3: no run of %u mapped LBAs", n);
		goto out;
	}
	for (i = 0; i < n; i++) {
		ret = sftl_read_lba(fs, start + i, src + (size_t)i * SFTL_LBA_SIZE, NULL);
		if (ret) {
			sftl_err("rung3: read LBA %u: %d", start + i, ret);
			goto out;
		}
	}
	sftl_info("rung3: rewriting LBAs %u..%u (written %u..%u, siblings carried)",
		  start, start + n - 1, start + 1, start + n - 2);
	sb_written = true;
	ret = sftl_write(fs, start + 1, n - 2, src + SFTL_LBA_SIZE);
	if (!ret)
		ret = sftl_writer_seal(fs);
	if (ret) {
		sftl_err("rung3: write/seal %d", ret);
		goto undo;
	}
	if (fs->rec[sb].type != SFTL_SB_CLOSED) {
		sftl_err("rung3: sb %u not closed after seal (type %u)", sb, fs->rec[sb].type);
		ret = -EIO;
		goto undo;
	}

	/* 4. */
	if (!sftl_sb_closed(fs, sb, meta)) {
		sftl_err("rung3: the mount path does not see sb %u closed", sb);
		ret = -EIO;
		goto undo;
	}
	ret = sftl_read_btoc(fs, sb, &nb, img);
	if (ret)
		goto undo;
	if (memcmp(img, fs->wr->btoc_img, nb * SFTL_LBA_SIZE)) {
		sftl_err("rung3: BTOC on the media differs from the one written");
		ret = -EIO;
		goto undo;
	}
	for (i = 0; i < cap - nb; i++) {
		u8 m16[SFTL_META_LBA];

		ret = sftl_read_vba(fs, sftl_vba(sb, i), buf, m16);
		if (ret)
			goto undo;
		if (!i && sftl_get48(m16 + 2) != sftl_get48(meta + 2)) {
			sftl_err("rung3: first weave %llu, BTOC says %llu",
				 (unsigned long long)sftl_get48(m16 + 2),
				 (unsigned long long)sftl_get48(meta + 2));
			ret = -EIO;
			goto undo;
		}
		sftl_btoc_append(ent, &nent, sftl_get48(meta + 2), sftl_get32(m16 + 8),
				 sftl_get48(m16 + 2), sftl_get32(m16 + 12));
	}
	nb2 = nb;
	sftl_btoc_build(ent, nent, rebuilt, &nb2, nb);
	if (nb2 != nb || memcmp(img, rebuilt, nb * SFTL_LBA_SIZE)) {
		sftl_err("rung3: BTOC rebuilt from page metadata differs (nb %u/%u)", nb, nb2);
		ret = -EIO;
		goto undo;
	}
	sftl_info("rung3: BTOC: nb %u, %u entries, identical to written and to rebuilt", nb, nent);
	for (i = 0; i < n; i++) {
		vbas[i] = sftl_map_get(&fs->map, start + i);
		if (sftl_sb_of(vbas[i]) != sb && i && i != n - 1) {
			sftl_err("rung3: LBA %u maps to %#x, outside sb %u", start + i, vbas[i], sb);
			ret = -EIO;
			goto undo;
		}
		ret = sftl_read_lba(fs, start + i, buf, NULL);
		if (ret || memcmp(buf, src + (size_t)i * SFTL_LBA_SIZE, SFTL_LBA_SIZE)) {
			sftl_err("rung3: LBA %u reads back wrong (%d)", start + i, ret);
			ret = ret ? ret : -EIO;
			goto undo;
		}
	}
	f1.digest = map_digest(fs, &f1.mapped);
	sftl_info("rung3: all %u LBAs read back; map now %u mapped, digest %016llx",
		  n, f1.mapped, (unsigned long long)f1.digest);
	unmount_one(fs);
	fs = NULL;

	/* 5. */
	fs = mount_one(a, &f2);
	if (!fs) {
		ret = -EIO;
		goto undo;
	}
	for (i = 0; i < n; i++) {
		if (sftl_map_get(&fs->map, start + i) != vbas[i]) {
			sftl_err("rung3: after remount LBA %u at %#x, written at %#x", start + i,
				 sftl_map_get(&fs->map, start + i), vbas[i]);
			ret = -EIO;
			goto undo;
		}
	}
	if (f2.digest != f1.digest || f2.mapped != f1.mapped) {
		sftl_err("rung3: replayed map differs from the written one");
		ret = -EIO;
		goto undo;
	}
	sftl_info("rung3: remount replays sb %u: map equals the written one", sb);
	ret = 0;

undo:
	/* 6. Whatever happened above, the superblock goes back to erased. */
	if (!fs) {
		fs = sftl_alloc(sizeof(*fs));
		if (fs && sftl_init(fs, a->ops, a->hw)) {
			sftl_exit(fs);
			sftl_free(fs);
			fs = NULL;
		}
		if (fs && sftl_load_vfl(fs)) {
			unmount_one(fs);
			fs = NULL;
		}
	}
	if (!fs) {
		sftl_err("rung3: cannot reach the VFL to erase sb %u: LEFT WRITTEN", sb);
		ret = ret ? ret : -EIO;
		goto out;
	}
	if (sb_written) {
		int r = 0;

		for (i = 0; i < fs->shape[sb].width && !r; i++)
			r = a->ops->erase_block(a->hw, fs->shape[sb].bank[i], sb);
		if (!r)
			r = sb_reads_erased(fs, sb, buf, meta);
		if (r) {
			sftl_err("rung3: erase of sb %u failed (%d): LEFT WRITTEN", sb, r);
			ret = ret ? ret : -EIO;
			goto out;
		}
		sftl_info("rung3: sb %u erased, every page reads erased", sb);
	}
	unmount_one(fs);
	fs = NULL;
	if (ret)
		goto out;

	/* 7. */
	fs = mount_one(a, &f2);
	if (!fs) {
		ret = -EIO;
		goto out;
	}
	if (f2.digest != f0.digest || f2.mapped != f0.mapped ||
	    f2.replay_applied != f0.replay_applied || f2.ctx_sb != f0.ctx_sb) {
		sftl_err("rung3: map after undo differs from before");
		ret = -EIO;
		goto out;
	}
	sftl_info("rung3: PASS: map digest after undo equals before (%016llx)",
		  (unsigned long long)f0.digest);
out:
	unmount_one(fs);
	sftl_free(src);
	sftl_free(buf);
	sftl_free(meta);
	sftl_free(img);
	sftl_free(rebuilt);
	sftl_free(ent);
	sftl_free(vbas);
	return ret;
}
