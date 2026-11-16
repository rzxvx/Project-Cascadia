// SPDX-License-Identifier: GPL-2.0-only
/*
 * Swiss FTL block devices for the iPad mini 1 PPN NAND.
 *
 * Exposes the FTL as ftl0 (all reported LBAs) and ftl0pN (one per LwVM
 * volume).  Everything is read-only unless enable_write is set on the kernel
 * command line, the controller permits it, and the volume is the one named
 * by write_volume; even then System, Data and effaceable volumes refuse.
 *
 * Written from devices/a5/docs/research/p105-nand-spec.md.  Credit: the
 * on-media format shared with the iPhone 5 and the first write path on this
 * package are the work of crystal-lized.
 */

#include <linux/blk-mq.h>
#include <linux/blkdev.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/reboot.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/vmalloc.h>

#include "apple_ppn.h"
#include "sftl.h"
#include "sftl_port.h"

#define FTL_NAME		"ftl"
#define FTL_MAX_DISKS		(SFTL_MAX_PARTS + 1)
#define FTL_WHOLE_DISK		((u32)-1)
#define MAX_REQ_LBAS		(SFTL_BATCH_PAGES * SFTL_LBAS_PER_PAGE)
#define EFFACEABLE_ATTRS	0x3000000000000ull

static bool auto_mount = true;
module_param(auto_mount, bool, 0444);

static bool enable_write;
module_param(enable_write, bool, 0444);
MODULE_PARM_DESC(enable_write, "Allow the FTL to write");

static char *write_volume = "";
module_param(write_volume, charp, 0444);
MODULE_PARM_DESC(write_volume, "Name of the only LwVM volume that may be written");

static bool skip_checkpoint;
module_param(skip_checkpoint, bool, 0644);
MODULE_PARM_DESC(skip_checkpoint,
		 "Test only: unmount without the final checkpoint, as a crash would");

static unsigned int verify_every = SFTL_VERIFY_DEFAULT;
module_param(verify_every, uint, 0444);
MODULE_PARM_DESC(verify_every, "Read back every Nth programmed page (1 = all)");

struct ftl_instance;

struct ftl_disk {
	struct gendisk *gd;
	struct ftl_instance *inst;
	u32 part;			/* partition index, or FTL_WHOLE_DISK */
	u32 nlbas;
	bool writable;
};

struct ftl_instance {
	struct apple_ppn *ppn;
	struct sftl fs;
	struct sftl_lwvm lv;
	struct mutex lock;		/* serialises every media operation */
	struct blk_mq_tag_set tag_set;
	struct ftl_disk disk[FTL_MAX_DISKS];
	unsigned int ndisks;
	bool writing;
	bool going;
	u8 *xfer;			/* MAX_REQ_LBAS * 4 KiB */
	u8 *tmp_lba;
};

static DEFINE_MUTEX(ftl_global_lock);
static struct ftl_instance *ftl_mounted;
static int ftl_major;

/* ---- glue between the portable core and the PPN driver ---------------- */

static int glue_read(void *hw, unsigned int bank, unsigned int block, unsigned int page,
		     bool slc, u8 *data, u8 *meta, u8 *status)
{
	return apple_ppn_read_page(hw, bank, block, page, slc, data, meta, status);
}

static int glue_erase(void *hw, unsigned int bank, unsigned int block)
{
	return apple_ppn_erase(hw, bank, block);
}

static int glue_program(void *hw, const struct sftl_prog *pages, unsigned int n)
{
	return apple_ppn_program(hw, pages, n);
}

static const struct sftl_ops ftl_glue_ops = {
	.read_page = glue_read,
	.erase_block = glue_erase,
	.program = glue_program,
};

/* ---- block device ------------------------------------------------------ */

/* Disk-relative LBA to FTL LBA. */
static int disk_to_ftl(const struct ftl_disk *d, u32 rel, u32 *lba)
{
	if (rel >= d->nlbas)
		return -EINVAL;
	if (d->part == FTL_WHOLE_DISK) {
		*lba = rel;
		return 0;
	}
	return sftl_part_lba(&d->inst->lv.part[d->part], rel, lba);
}

static int do_read(struct ftl_disk *d, u32 rel, u32 count, u8 *buf)
{
	struct ftl_instance *in = d->inst;
	u32 i, lba;
	int ret;

	for (i = 0; i < count; i++) {
		ret = disk_to_ftl(d, rel + i, &lba);
		if (ret)
			return ret;
		ret = sftl_read_lba(&in->fs, lba, buf + i * SFTL_LBA_SIZE, NULL);
		if (ret)
			return ret;
	}
	return 0;
}

/* Write in maximal runs of contiguous FTL LBAs: a run ends at a chunk edge. */
static int do_write(struct ftl_disk *d, u32 rel, u32 count, const u8 *buf)
{
	struct ftl_instance *in = d->inst;
	u32 i = 0;

	while (i < count) {
		u32 start, lba, n = 1;
		int ret;

		ret = disk_to_ftl(d, rel + i, &start);
		if (ret)
			return ret;
		while (i + n < count) {
			ret = disk_to_ftl(d, rel + i + n, &lba);
			if (ret)
				return ret;
			if (lba != start + n)
				break;
			n++;
		}
		ret = sftl_write(&in->fs, start, n, buf + i * SFTL_LBA_SIZE);
		if (ret)
			return ret;
		i += n;
	}
	return 0;
}

static blk_status_t ftl_queue_rq(struct blk_mq_hw_ctx *hctx,
				 const struct blk_mq_queue_data *bd)
{
	struct request *rq = bd->rq;
	struct ftl_disk *d = rq->q->disk->private_data;
	struct ftl_instance *in = d->inst;
	struct req_iterator iter;
	struct bio_vec bv;
	u32 rel = (u32)(blk_rq_pos(rq) >> 3);
	u32 count = blk_rq_bytes(rq) / SFTL_LBA_SIZE;
	u32 done = 0;
	blk_status_t st = BLK_STS_OK;
	int ret = 0;

	blk_mq_start_request(rq);

	if (req_op(rq) == REQ_OP_FLUSH) {
		blk_mq_end_request(rq, BLK_STS_OK);
		return BLK_STS_OK;
	}
	if (req_op(rq) != REQ_OP_READ && req_op(rq) != REQ_OP_WRITE) {
		blk_mq_end_request(rq, BLK_STS_NOTSUPP);
		return BLK_STS_OK;
	}
	if (req_op(rq) == REQ_OP_WRITE && !d->writable) {
		blk_mq_end_request(rq, BLK_STS_IOERR);
		return BLK_STS_OK;
	}
	if (count > MAX_REQ_LBAS || rel + count > d->nlbas) {
		blk_mq_end_request(rq, BLK_STS_IOERR);
		return BLK_STS_OK;
	}

	mutex_lock(&in->lock);
	if (in->going) {
		ret = -ENODEV;
		goto unlock;
	}
	if (req_op(rq) == REQ_OP_READ) {
		ret = do_read(d, rel, count, in->xfer);
		if (!ret)
			rq_for_each_segment(bv, rq, iter) {
				memcpy_to_bvec(&bv, in->xfer + done);
				done += bv.bv_len;
			}
	} else {
		rq_for_each_segment(bv, rq, iter) {
			memcpy_from_bvec(in->xfer + done, &bv);
			done += bv.bv_len;
		}
		ret = do_write(d, rel, count, in->xfer);
	}
unlock:
	mutex_unlock(&in->lock);

	if (ret) {
		pr_err_ratelimited("apple_ftl: %s rel %u count %u: %d\n",
				   req_op(rq) == REQ_OP_WRITE ? "write" : "read",
				   rel, count, ret);
		st = BLK_STS_IOERR;
	}
	blk_mq_end_request(rq, st);
	return BLK_STS_OK;
}

static const struct blk_mq_ops ftl_mq_ops = {
	.queue_rq = ftl_queue_rq,
};

static const struct block_device_operations ftl_fops = {
	.owner = THIS_MODULE,
};

static bool volume_allowed(const struct sftl_part *p)
{
	if (!enable_write || !write_volume[0])
		return false;
	if (!sysfs_streq(p->name, write_volume))
		return false;
	if (!strcasecmp(p->name, "System") || !strcasecmp(p->name, "Data"))
		return false;
	if (p->attrs & EFFACEABLE_ATTRS)
		return false;
	return true;
}

static int create_disk(struct ftl_instance *in, u32 part, u32 nlbas, bool writable,
		       const char *name)
{
	struct queue_limits lim = {
		.logical_block_size = SFTL_LBA_SIZE,
		.physical_block_size = SFTL_PAGE_SIZE,
		.max_hw_sectors = MAX_REQ_LBAS << 3,
		.max_segments = USHRT_MAX,
		.max_segment_size = UINT_MAX,
	};
	struct ftl_disk *d = &in->disk[in->ndisks];
	struct gendisk *gd;
	int ret;

	gd = blk_mq_alloc_disk(&in->tag_set, &lim, d);
	if (IS_ERR(gd))
		return PTR_ERR(gd);
	gd->major = ftl_major;
	gd->first_minor = in->ndisks;
	gd->minors = 1;
	gd->flags |= GENHD_FL_NO_PART;
	gd->fops = &ftl_fops;
	gd->private_data = d;
	strscpy(gd->disk_name, name, sizeof(gd->disk_name));
	set_capacity(gd, (sector_t)nlbas << 3);
	set_disk_ro(gd, !writable);

	d->gd = gd;
	d->inst = in;
	d->part = part;
	d->nlbas = nlbas;
	d->writable = writable;
	in->ndisks++;

	ret = add_disk(gd);
	if (ret) {
		in->ndisks--;
		put_disk(gd);
		d->gd = NULL;
	}
	return ret;
}

static int create_disks(struct ftl_instance *in)
{
	u32 i;
	int ret;
	char name[DISK_NAME_LEN];

	ret = create_disk(in, FTL_WHOLE_DISK, in->fs.reported_lbas, false, FTL_NAME "0");
	if (ret)
		return ret;
	for (i = 0; i < in->lv.nparts; i++) {
		struct sftl_part *p = &in->lv.part[i];
		bool w = in->writing && volume_allowed(p);

		snprintf(name, sizeof(name), FTL_NAME "0p%u", i);
		ret = create_disk(in, i, p->nlbas, w, name);
		if (ret)
			return ret;
		pr_info("apple_ftl: %s = %s (%u LBAs)%s\n", name, p->name, p->nlbas,
			w ? " writable" : "");
	}
	return 0;
}

static void remove_disks(struct ftl_instance *in)
{
	unsigned int i;

	for (i = 0; i < in->ndisks; i++) {
		if (in->disk[i].gd) {
			del_gendisk(in->disk[i].gd);
			put_disk(in->disk[i].gd);
			in->disk[i].gd = NULL;
		}
	}
	in->ndisks = 0;
}

static void instance_free(struct ftl_instance *in)
{
	sftl_writer_stop(&in->fs);
	sftl_exit(&in->fs);
	vfree(in->xfer);
	kfree(in->tmp_lba);
	if (in->ppn)
		apple_ppn_put(in->ppn);
	kfree(in);
}

/* Checkpoint when needed.  On return the writer is stopped from committing again. */
static void final_checkpoint(struct ftl_instance *in)
{
	int ret;

	if (!in->writing)
		return;
	if (skip_checkpoint) {
		pr_warn("apple_ftl: final checkpoint skipped (skip_checkpoint)\n");
		return;
	}
	mutex_lock(&in->lock);
	ret = sftl_checkpoint(&in->fs);
	mutex_unlock(&in->lock);
	if (ret)
		pr_err("apple_ftl: final checkpoint failed: %d\n", ret);
}

static int ftl_do_mount(void)
{
	struct ftl_instance *in;
	struct apple_ppn *ppn;
	int ret;

	if (ftl_mounted)
		return -EBUSY;
	ppn = apple_ppn_get();
	if (!ppn) {
		pr_err("apple_ftl: controller is not bound\n");
		return -ENODEV;
	}
	in = kzalloc(sizeof(*in), GFP_KERNEL);
	if (!in) {
		apple_ppn_put(ppn);
		return -ENOMEM;
	}
	in->ppn = ppn;
	mutex_init(&in->lock);
	in->xfer = vmalloc(MAX_REQ_LBAS * SFTL_LBA_SIZE);
	if (!in->xfer) {
		ret = -ENOMEM;
		goto err_free;
	}
	ret = sftl_init(&in->fs, &ftl_glue_ops, ppn);
	if (ret)
		goto err_free;

	ret = sftl_mount(&in->fs);
	if (ret) {
		pr_err("apple_ftl: mount failed: %d\n", ret);
		goto err_free;
	}
	pr_info("apple_ftl: context sb %u, replayed %u LBAs\n", in->fs.ctx_sb,
		in->fs.replay_applied);

	ret = sftl_lwvm_load(&in->fs, &in->lv);
	if (ret) {
		pr_err("apple_ftl: LwVM header refused: %d\n", ret);
		goto err_free;
	}

	if (enable_write) {
		if (!apple_ppn_write_allowed(ppn)) {
			pr_err("apple_ftl: enable_write set but controller not allow_write\n");
			ret = -EPERM;
			goto err_free;
		}
		ret = sftl_writer_start(&in->fs, verify_every);
		if (ret) {
			pr_err("apple_ftl: writer refused: %d\n", ret);
			goto err_free;
		}
		in->writing = true;
	}

	in->tag_set.ops = &ftl_mq_ops;
	in->tag_set.nr_hw_queues = 1;
	in->tag_set.queue_depth = 4;
	in->tag_set.numa_node = NUMA_NO_NODE;
	in->tag_set.flags = BLK_MQ_F_BLOCKING;
	ret = blk_mq_alloc_tag_set(&in->tag_set);
	if (ret)
		goto err_free;

	ret = create_disks(in);
	if (ret) {
		remove_disks(in);
		blk_mq_free_tag_set(&in->tag_set);
		goto err_free;
	}

	ftl_mounted = in;
	return 0;

err_free:
	instance_free(in);
	return ret;
}

static void ftl_do_unmount(void)
{
	struct ftl_instance *in = ftl_mounted;

	if (!in)
		return;
	ftl_mounted = NULL;

	mutex_lock(&in->lock);
	in->going = true;
	mutex_unlock(&in->lock);

	remove_disks(in);
	blk_mq_free_tag_set(&in->tag_set);
	in->going = false;
	final_checkpoint(in);
	instance_free(in);
}

static int ftl_reboot(struct notifier_block *nb, unsigned long code, void *data)
{
	mutex_lock(&ftl_global_lock);
	if (ftl_mounted)
		final_checkpoint(ftl_mounted);
	mutex_unlock(&ftl_global_lock);
	return NOTIFY_DONE;
}

static struct notifier_block ftl_reboot_nb = {
	.notifier_call = ftl_reboot,
};

static int mount_param_set(const char *val, const struct kernel_param *kp)
{
	bool want;
	int ret;

	ret = kstrtobool(val, &want);
	if (ret)
		return ret;
	mutex_lock(&ftl_global_lock);
	if (want)
		ret = ftl_do_mount();
	else
		ftl_do_unmount();
	mutex_unlock(&ftl_global_lock);
	return ret;
}

static int mount_param_get(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf, "%d\n", ftl_mounted ? 1 : 0);
}

static const struct kernel_param_ops mount_param_ops = {
	.set = mount_param_set,
	.get = mount_param_get,
};
module_param_cb(mount, &mount_param_ops, NULL, 0200);
MODULE_PARM_DESC(mount, "Write 1 to mount the FTL and create the block devices");

static int __init apple_ftl_init(void)
{
	int ret;

	ftl_major = register_blkdev(0, FTL_NAME);
	if (ftl_major < 0)
		return ftl_major;
	register_reboot_notifier(&ftl_reboot_nb);
	if (auto_mount) {
		mutex_lock(&ftl_global_lock);
		ret = ftl_do_mount();
		mutex_unlock(&ftl_global_lock);
		if (ret)
			pr_info("apple_ftl: not mounted at load (%d); write 1 to 'mount'\n", ret);
	}
	return 0;
}

static void __exit apple_ftl_exit(void)
{
	mutex_lock(&ftl_global_lock);
	ftl_do_unmount();
	mutex_unlock(&ftl_global_lock);
	unregister_reboot_notifier(&ftl_reboot_nb);
	unregister_blkdev(ftl_major, FTL_NAME);
}

module_init(apple_ftl_init);
module_exit(apple_ftl_exit);

MODULE_DESCRIPTION("Swiss FTL for the iPad mini 1 PPN NAND");
MODULE_AUTHOR("Project Cascadia; format findings by crystal-lized");
MODULE_LICENSE("GPL");
