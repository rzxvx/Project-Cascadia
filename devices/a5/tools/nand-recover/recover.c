// SPDX-License-Identifier: GPL-2.0-only
/*
 * Recovery after a session whose writer stopped before any commit, for
 * example when something else drove the NAND controller under the driver.
 * Portable: offline harness and kernel module.
 *
 *  1. mount: the last context, then replay of everything newer (iOS's writes
 *     and Linux's uncommitted ones); pages that do not read back are skipped
 *     and their SB noted as torn;
 *  2. start the writer: each torn open SB is given up (spec 4.9): its LBAs are
 *     moved, it is listed waiting for erase with nothing valid;
 *  3. erase again the spares whose erase was cut off, and check every page of
 *     them reads erased, so no half-erased block is ever listed as erased;
 *  4. commit, then a fresh mount: nothing to replay, no LBA in a torn SB.
 *
 * A dry run stops after step 1 and says what steps 2-4 would do.
 */
#include "sftl.h"
#include "sftl_port.h"
#include "recover.h"

static struct sftl *r_mount(const struct recover_args *a)
{
	struct sftl *fs = sftl_alloc(sizeof(*fs));
	int ret;

	if (!fs)
		return NULL;
	ret = sftl_init(fs, a->ops, a->hw);
	if (!ret)
		ret = sftl_mount(fs);
	if (ret) {
		sftl_err("recover: mount failed (%d)", ret);
		sftl_exit(fs);
		sftl_free(fs);
		return NULL;
	}
	return fs;
}

static void r_unmount(struct sftl *fs)
{
	if (!fs)
		return;
	sftl_writer_stop(fs);
	sftl_exit(fs);
	sftl_free(fs);
}

static bool torn(const struct sftl *fs, u32 sb)
{
	return (fs->torn[sb / 8] >> (sb % 8)) & 1;
}

static int all_erased(struct sftl *fs, u32 sb, u8 *data, u8 *meta)
{
	u32 i, pg, k;

	for (i = 0; i < fs->shape[sb].width; i++) {
		for (pg = 0; pg < SFTL_MLC_PAGES; pg++) {
			u8 st;
			int ret = fs->ops->read_page(fs->hw, fs->shape[sb].bank[i], sb, pg, false,
						     data, meta, &st);

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

int recover_run(const struct recover_args *a)
{
	struct sftl *fs;
	u8 *data = sftl_alloc(SFTL_PAGE_SIZE), *meta = sftl_alloc(SFTL_META_PAGE);
	u32 sb, i, ntorn = 0, inmap = 0;
	int ret = 0;

	if (!data || !meta) {
		ret = -ENOMEM;
		goto out_free;
	}

	/* 1. */
	fs = r_mount(a);
	if (!fs) {
		ret = -EIO;
		goto out_free;
	}
	sftl_info("recover: context sb %u weave %llu, %u replayed", fs->ctx_sb,
		  (unsigned long long)fs->ctx_weave, fs->replay_applied);
	for (sb = 0; sb < SFTL_NUM_SB; sb++)
		if (torn(fs, sb)) {
			ntorn++;
			sftl_info("recover: sb %u has a page that did not read back (SB array type %u)",
				  sb, fs->rec[sb].type);
		}
	for (i = 0; i < a->nerase; i++) {
		const struct sftl_sb_rec *r = &fs->rec[a->erase[i]];
		const struct sftl_sb_probe *pr = &fs->probe[a->erase[i]];

		if (a->erase[i] >= SFTL_NUM_SB || !fs->shape[a->erase[i]].width ||
		    a->erase[i] == fs->ctx_sb || r->valid ||
		    (r->type != SFTL_SB_ERASED && r->type != SFTL_SB_AWAIT_ERASE)) {
			sftl_err("recover: sb %u is not a spare (type %u, %u valid): refusing",
				 a->erase[i], r->type, r->valid);
			ret = -EINVAL;
			goto out;
		}
		sftl_info("recover: sb %u to erase again: type %u, first page type %#x weave %llu",
			  a->erase[i], r->type, pr->first_type, (unsigned long long)pr->first_weave);
	}
	if (a->dry_run) {
		sftl_info("recover: dry run: %u torn SB(s), %u spare(s) to erase; nothing written",
			  ntorn, a->nerase);
		goto out;
	}

	/* 2. */
	ret = sftl_writer_start(fs, 1);
	if (ret) {
		sftl_err("recover: writer start failed (%d)", ret);
		goto out;
	}
	for (sb = 0; sb < SFTL_NUM_SB; sb++)
		if (torn(fs, sb))
			sftl_info("recover: sb %u now type %u, %u valid", sb, fs->rec[sb].type,
				  fs->rec[sb].valid);

	/* 3. */
	for (i = 0; i < a->nerase; i++) {
		u32 s = a->erase[i];

		if (s == fs->wr->fr.sb) {
			sftl_info("recover: sb %u was erased again as the new frontier", s);
			continue;
		}
		ret = sftl_erase_spare(fs, s);
		if (!ret)
			ret = all_erased(fs, s, data, meta);
		if (ret) {
			/* given up for the session; listed waiting for erase at the commit */
			sftl_err("recover: sb %u: erase or read-back failed (%d)", s, ret);
			if (ret > 0)
				fs->rec[s].type = SFTL_SB_AWAIT_ERASE;
			ret = 0;
			continue;
		}
		sftl_info("recover: sb %u erased, every page reads erased", s);
	}

	/* 4. */
	fs->wr->dirty = true;
	ret = sftl_checkpoint(fs);
	if (ret) {
		sftl_err("recover: commit failed (%d): do not boot iOS", ret);
		goto out;
	}
	sftl_info("recover: committed: context sb %u", fs->ctx_sb);
	r_unmount(fs);
	fs = r_mount(a);
	if (!fs) {
		ret = -EIO;
		goto out_free;
	}
	for (sb = 0; sb < SFTL_NUM_SB; sb++)
		if (torn(fs, sb))
			ntorn = SFTL_NUM_SB;
	for (i = 0; i < fs->total_lbas; i++) {
		u32 v = sftl_map_get(&fs->map, i);

		if (v != SFTL_UNMAPPED && fs->rec[sftl_sb_of(v)].type == SFTL_SB_AWAIT_ERASE)
			inmap++;
	}
	if (fs->replay_applied || ntorn == SFTL_NUM_SB || inmap) {
		sftl_err("recover: fresh mount: %u replayed, torn %s, %u LBAs in SBs waiting for erase",
			 fs->replay_applied, ntorn == SFTL_NUM_SB ? "still" : "none", inmap);
		ret = -EIO;
		goto out;
	}
	sftl_info("recover: PASS: context sb %u, nothing to replay, no LBA in an SB waiting for erase",
		  fs->ctx_sb);
out:
	r_unmount(fs);
out_free:
	sftl_free(data);
	sftl_free(meta);
	return ret;
}
