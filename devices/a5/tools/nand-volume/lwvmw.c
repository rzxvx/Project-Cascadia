// SPDX-License-Identifier: GPL-2.0-only
/*
 * Write a planned LwVM header through the FTL, as spec 6.4 step 3 (and 6.5
 * step 2) say.  Portable: offline harness and kernel module.
 *
 *  1. mount; both header copies must equal the header the plan was made from,
 *     or (an earlier run stopped half way) the backup already holds the plan;
 *     the plan must parse as a valid LwVM header here too;
 *  2. write LBA 4095 (backup), read back; write LBA 0 (primary), read back;
 *     LBAs 1-3 and 4092-4094 unchanged;
 *  3. commit (close the frontier, write a context), so iOS never boots on an
 *     open Linux frontier;
 *  4. fresh mount: both copies equal the plan, nothing to replay, and the
 *     partitions come out as planned.
 *
 * Nothing else is written.  A dry run stops after step 1.
 */
#include "sftl.h"
#include "sftl_port.h"
#include "lwvmw.h"

#define LBA_PRIMARY	0u
#define LBA_BACKUP	4095u

static const u32 neighbours[] = { 1, 2, 3, 4092, 4093, 4094 };

static u64 fnv(const u8 *p, u32 n)
{
	u64 h = 0xcbf29ce484222325ull;
	u32 i;

	for (i = 0; i < n; i++) {
		h ^= p[i];
		h *= 0x100000001b3ull;
	}
	return h;
}

static struct sftl *w_mount(const struct lwvmw_args *a)
{
	struct sftl *fs = sftl_alloc(sizeof(*fs));
	int ret;

	if (!fs)
		return NULL;
	ret = sftl_init(fs, a->ops, a->hw);
	if (!ret)
		ret = sftl_mount(fs);
	if (ret) {
		sftl_err("lwvmw: mount failed (%d)", ret);
		sftl_exit(fs);
		sftl_free(fs);
		return NULL;
	}
	return fs;
}

static void w_unmount(struct sftl *fs)
{
	if (!fs)
		return;
	sftl_writer_stop(fs);
	sftl_exit(fs);
	sftl_free(fs);
}

static void print_layout(const char *what, const struct sftl_lwvm *lv)
{
	u32 i;

	for (i = 0; i < lv->nparts; i++)
		sftl_info("lwvmw: %s: partition %u %s, %u LBAs, %u chunks", what, i,
			  lv->part[i].name, lv->part[i].nlbas, lv->part[i].nchunks);
}

static int read_both(struct sftl *fs, u8 *p, u8 *b)
{
	int ret = sftl_read_lba(fs, LBA_PRIMARY, p, NULL);

	return ret ? ret : sftl_read_lba(fs, LBA_BACKUP, b, NULL);
}

static int write_one(struct sftl *fs, u32 lba, const u8 *src, u8 *buf)
{
	int ret = sftl_write(fs, lba, 1, src);

	if (ret) {
		sftl_err("lwvmw: write of LBA %u failed (%d)", lba, ret);
		return ret;
	}
	ret = sftl_read_lba(fs, lba, buf, NULL);
	if (ret || memcmp(buf, src, SFTL_LBA_SIZE)) {
		sftl_err("lwvmw: LBA %u reads back %s", lba, ret ? "with an error" : "different");
		return ret ? ret : -EIO;
	}
	sftl_info("lwvmw: LBA %u written and read back", lba);
	return 0;
}

int lwvmw_run(const struct lwvmw_args *a)
{
	struct sftl_lwvm *lv = sftl_alloc(sizeof(*lv));
	u8 *p = sftl_alloc(SFTL_LBA_SIZE), *b = sftl_alloc(SFTL_LBA_SIZE);
	u8 m16[SFTL_META_LBA];
	u64 near[6];
	struct sftl *fs = NULL;
	bool need_backup, need_primary;
	u32 i;
	int ret;

	if (!lv || !p || !b) {
		ret = -ENOMEM;
		goto out;
	}

	/* 1. */
	ret = sftl_lwvm_parse(a->to, NULL, lv);
	if (ret) {
		sftl_err("lwvmw: the planned header does not parse (%d)", ret);
		goto out;
	}
	print_layout("plan", lv);
	ret = sftl_lwvm_parse(a->from, NULL, lv);
	if (ret) {
		sftl_err("lwvmw: the header planned from does not parse (%d)", ret);
		goto out;
	}
	fs = w_mount(a);
	if (!fs) {
		ret = -EIO;
		goto out;
	}
	sftl_info("lwvmw: context sb %u, %u replayed", fs->ctx_sb, fs->replay_applied);
	ret = sftl_read_lba(fs, LBA_PRIMARY, p, m16);
	if (!ret)
		ret = sftl_read_lba(fs, LBA_BACKUP, b, NULL);
	if (ret)
		goto out;
	sftl_info("lwvmw: LBA 0 metadata type %02x user %08x", m16[0], sftl_get32(m16 + 12));
	if (!memcmp(p, a->from, SFTL_LBA_SIZE) && !memcmp(b, a->from, SFTL_LBA_SIZE)) {
		need_backup = need_primary = true;
	} else if (!memcmp(p, a->from, SFTL_LBA_SIZE) && !memcmp(b, a->to, SFTL_LBA_SIZE)) {
		sftl_warn("lwvmw: the backup already holds the plan (an earlier run stopped); finishing");
		need_backup = false;
		need_primary = true;
	} else if (!memcmp(p, a->to, SFTL_LBA_SIZE) && !memcmp(b, a->to, SFTL_LBA_SIZE)) {
		sftl_warn("lwvmw: both copies already hold the plan; committing only");
		need_backup = need_primary = false;
	} else {
		sftl_err("lwvmw: the header on the media is not the one planned from: stopping");
		ret = -ESTALE;
		goto out;
	}
	for (i = 0; i < 6; i++) {
		ret = sftl_read_lba(fs, neighbours[i], p, NULL);
		if (ret)
			goto out;
		near[i] = fnv(p, SFTL_LBA_SIZE);
	}
	if (a->dry_run) {
		sftl_info("lwvmw: dry run: checks passed, nothing written");
		goto out;
	}

	/* 2. */
	ret = sftl_writer_start(fs, 1);
	if (ret)
		goto out;
	if (need_backup) {
		ret = write_one(fs, LBA_BACKUP, a->to, p);
		if (ret)
			goto out;
	}
	if (need_primary) {
		ret = write_one(fs, LBA_PRIMARY, a->to, p);
		if (ret)
			goto out;
	}
	for (i = 0; i < 6; i++) {
		ret = sftl_read_lba(fs, neighbours[i], p, NULL);
		if (ret)
			goto out;
		if (fnv(p, SFTL_LBA_SIZE) != near[i]) {
			sftl_err("lwvmw: LBA %u changed", neighbours[i]);
			ret = -EIO;
			goto out;
		}
	}

	/* 3. */
	fs->wr->dirty = true;
	ret = sftl_checkpoint(fs);
	if (ret) {
		sftl_err("lwvmw: commit failed (%d): do not boot iOS; run again", ret);
		goto out;
	}
	sftl_info("lwvmw: committed: context sb %u", fs->ctx_sb);
	w_unmount(fs);

	/* 4. */
	fs = w_mount(a);
	if (!fs) {
		ret = -EIO;
		goto out;
	}
	ret = read_both(fs, p, b);
	if (ret)
		goto out;
	if (memcmp(p, a->to, SFTL_LBA_SIZE) || memcmp(b, a->to, SFTL_LBA_SIZE) ||
	    fs->replay_applied) {
		sftl_err("lwvmw: fresh mount: copies %s/%s, %u replayed",
			 memcmp(p, a->to, SFTL_LBA_SIZE) ? "differ" : "ok",
			 memcmp(b, a->to, SFTL_LBA_SIZE) ? "differ" : "ok", fs->replay_applied);
		ret = -EIO;
		goto out;
	}
	ret = sftl_lwvm_load(fs, lv);
	if (ret) {
		sftl_err("lwvmw: fresh mount: LwVM does not load (%d)", ret);
		goto out;
	}
	print_layout("media", lv);
	sftl_info("lwvmw: PASS: header written, committed (context sb %u), both copies equal the plan",
		  fs->ctx_sb);
out:
	w_unmount(fs);
	sftl_free(lv);
	sftl_free(p);
	sftl_free(b);
	return ret;
}
