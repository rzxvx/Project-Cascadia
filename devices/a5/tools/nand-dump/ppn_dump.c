// SPDX-License-Identifier: GPL-2.0-only
/*
 * Read-only raw dump of the PPN NAND through apple_ppn, in the layout of the
 * earlier dumps (w0c): per bus, CAU and block range one file of 16448-byte
 * pages exactly as they cross the bus (per 4 KiB unit: 1024 data, 16 metadata,
 * 3072 data), block-major, MLC rows, and one status byte per page.
 *
 *   echo "BUS CAU FIRST_BLOCK NBLOCKS" > /dev/ppndump   select a region
 *   cat /dev/ppndump    > busB-cauC.bS.bin             its raw pages
 *   cat /dev/ppndump_st > busB-cauC.bS.bin.st          the statuses read
 *
 * Nothing here programs or erases.
 */
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>

#include "apple_ppn.h"

#define RAW_PAGE	16448u
#define DATA_PAGE	16384u
#define META_PAGE	64u
/* the package's geometry, read from apple_ppn at load */
static unsigned int PAGES = 256, BLOCKS = 1064;
#define ST_FAILED_READ	0xee	/* status byte written when the read itself failed */

static struct apple_ppn *ppn;
static DEFINE_MUTEX(dump_lock);
static unsigned int r_bus, r_cau, r_first, r_count;
static u8 *statuses;			/* r_count * PAGES */
static u8 *raw, *data, *meta;
static long cached = -1;
static unsigned long failed;

static int load_page(unsigned long idx)
{
	unsigned int block = r_first + idx / PAGES, page = idx % PAGES, u;
	u8 st = 0;
	int ret;

	if ((long)idx == cached)
		return 0;
	ret = apple_ppn_read_page(ppn, r_cau * 2 + r_bus, block, page, false, data, meta, &st);
	if (ret) {
		memset(data, 0, DATA_PAGE);
		memset(meta, 0, META_PAGE);
		st = ST_FAILED_READ;
		failed++;
		pr_warn("ppndump: bus %u cau %u block %u page %u: read %d\n",
			r_bus, r_cau, block, page, ret);
	}
	for (u = 0; u < 4; u++) {
		u8 *r = raw + u * 4112;

		memcpy(r, data + u * 4096, 1024);
		memcpy(r + 1024, meta + u * 16, 16);
		memcpy(r + 1040, data + u * 4096 + 1024, 3072);
	}
	statuses[idx] = st;
	cached = idx;
	return 0;
}

static ssize_t dump_read(struct file *f, char __user *buf, size_t len, loff_t *pos)
{
	unsigned long total, idx, off;
	size_t done = 0;
	ssize_t ret = 0;

	mutex_lock(&dump_lock);
	if (!statuses) {
		ret = -EINVAL;
		goto out;
	}
	total = (unsigned long)r_count * PAGES * RAW_PAGE;
	while (done < len && *pos < total) {
		size_t n;

		idx = *pos / RAW_PAGE;
		off = *pos % RAW_PAGE;
		load_page(idx);
		n = min_t(size_t, len - done, RAW_PAGE - off);
		if (copy_to_user(buf + done, raw + off, n)) {
			ret = -EFAULT;
			goto out;
		}
		done += n;
		*pos += n;
		if (fatal_signal_pending(current))
			break;
	}
	ret = done;
out:
	mutex_unlock(&dump_lock);
	return ret;
}

static ssize_t dump_write(struct file *f, const char __user *buf, size_t len, loff_t *pos)
{
	char tmp[64];
	unsigned int b, c, first, n;

	if (len >= sizeof(tmp))
		return -EINVAL;
	if (copy_from_user(tmp, buf, len))
		return -EFAULT;
	tmp[len] = 0;
	if (sscanf(tmp, "%u %u %u %u", &b, &c, &first, &n) != 4 || b > 1 || c > 1 ||
	    !n || first >= BLOCKS || n > BLOCKS - first)
		return -EINVAL;
	mutex_lock(&dump_lock);
	vfree(statuses);
	statuses = vzalloc(n * PAGES);
	r_bus = b;
	r_cau = c;
	r_first = first;
	r_count = statuses ? n : 0;
	cached = -1;
	failed = 0;
	mutex_unlock(&dump_lock);
	pr_info("ppndump: region bus %u cau %u blocks %u..%u\n", b, c, first, first + n - 1);
	return statuses ? len : -ENOMEM;
}

static ssize_t st_read(struct file *f, char __user *buf, size_t len, loff_t *pos)
{
	ssize_t ret;

	mutex_lock(&dump_lock);
	if (!statuses) {
		ret = -EINVAL;
	} else {
		ret = simple_read_from_buffer(buf, len, pos, statuses, r_count * PAGES);
		if (*pos >= r_count * PAGES && failed)
			pr_warn("ppndump: %lu pages of this region failed to read\n", failed);
	}
	mutex_unlock(&dump_lock);
	return ret;
}

static const struct file_operations dump_fops = {
	.owner = THIS_MODULE,
	.read = dump_read,
	.write = dump_write,
	.llseek = default_llseek,
};

static const struct file_operations st_fops = {
	.owner = THIS_MODULE,
	.read = st_read,
	.llseek = default_llseek,
};

static struct miscdevice dump_dev = {
	.minor = MISC_DYNAMIC_MINOR, .name = "ppndump", .fops = &dump_fops, .mode = 0600,
};
static struct miscdevice st_dev = {
	.minor = MISC_DYNAMIC_MINOR, .name = "ppndump_st", .fops = &st_fops, .mode = 0400,
};

static int __init dump_init(void)
{
	int ret;

	ppn = apple_ppn_get();
	if (!ppn)
		return -ENODEV;
	{
		struct apple_ppn_geo geo;

		apple_ppn_geometry(ppn, &geo);
		PAGES = geo.mlc_pages;
		BLOCKS = geo.blocks;
		pr_info("ppndump: package %s, %u blocks x %u pages per CAU\n",
			geo.firmware, BLOCKS, PAGES);
	}
	raw = kmalloc(RAW_PAGE, GFP_KERNEL);
	data = kmalloc(DATA_PAGE, GFP_KERNEL);
	meta = kmalloc(META_PAGE, GFP_KERNEL);
	ret = (raw && data && meta) ? 0 : -ENOMEM;
	if (!ret)
		ret = misc_register(&dump_dev);
	if (!ret) {
		ret = misc_register(&st_dev);
		if (ret)
			misc_deregister(&dump_dev);
	}
	if (ret) {
		kfree(raw);
		kfree(data);
		kfree(meta);
		apple_ppn_put(ppn);
	}
	return ret;
}

static void __exit dump_exit(void)
{
	misc_deregister(&st_dev);
	misc_deregister(&dump_dev);
	vfree(statuses);
	kfree(raw);
	kfree(data);
	kfree(meta);
	apple_ppn_put(ppn);
}

module_init(dump_init);
module_exit(dump_exit);
MODULE_DESCRIPTION("Read-only raw dump of the A5 PPN NAND");
MODULE_LICENSE("GPL");
