// SPDX-License-Identifier: GPL-2.0-only
/*
 * Kernel module around lwvmw.c: writes a planned LwVM header through the FTL
 * (spec 6.4 step 3 / 6.5 step 2).  The FTL core is built in, so apple_ftl must
 * not be loaded; apple_ppn needs allow_write=1 unless dry_run=1.
 *
 *   insmod ppn_lwvmw.ko from=/tmp/lwvm-from.bin to=/tmp/lwvm-to.bin [dry_run=1]
 *
 * Always loads (busybox insmod repeats a failed load); the outcome is the
 * "result" parameter and the kernel log, lines "sftl: lwvmw:".
 */
#include <linux/kernel_read_file.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/vmalloc.h>

#include "apple_ppn.h"
#include "sftl.h"

#include "sftl_map.c"
#include "sftl_mount.c"
#include "sftl_format.c"
#include "sftl_write.c"
#include "sftl_lwvm.c"
#include "lwvmw.c"

static char *from, *to;
module_param(from, charp, 0444);
module_param(to, charp, 0444);
static bool dry_run;
module_param(dry_run, bool, 0444);
static int result = 1;
module_param(result, int, 0444);
MODULE_PARM_DESC(result, "0 = passed, negative errno = failed");

static int w_read(void *hw, unsigned int bank, unsigned int block, unsigned int page,
		  bool slc, u8 *data, u8 *meta, u8 *status)
{
	return apple_ppn_read_page(hw, bank, block, page, slc, data, meta, status);
}

static int w_erase(void *hw, unsigned int bank, unsigned int block)
{
	return apple_ppn_erase(hw, bank, block);
}

static int w_program(void *hw, const struct sftl_prog *pages, unsigned int n)
{
	return apple_ppn_program(hw, pages, n);
}

static const struct sftl_ops w_ops = {
	.read_page = w_read,
	.erase_block = w_erase,
	.program = w_program,
};

static int load(const char *path, void **buf)
{
	size_t size = 0;
	ssize_t n;

	if (!path)
		return -EINVAL;
	n = kernel_read_file_from_path(path, 0, buf, SFTL_LBA_SIZE, &size, READING_UNKNOWN);
	if (n < 0)
		return n;
	if (n != SFTL_LBA_SIZE || size != SFTL_LBA_SIZE) {
		vfree(*buf);
		*buf = NULL;
		return -EINVAL;
	}
	return 0;
}

static int __init w_init(void)
{
	struct apple_ppn *ppn = apple_ppn_get();
	struct lwvmw_args a = { .ops = &w_ops, .dry_run = dry_run };
	void *f = NULL, *t = NULL;

	if (!ppn) {
		pr_err("lwvmw: no PPN controller bound\n");
		result = -ENODEV;
		return 0;
	}
	{
		struct apple_ppn_geo geo;

		apple_ppn_geometry(ppn, &geo);
		result = sftl_geo_select(geo.blocks, geo.mlc_pages, geo.slc_pages);
		if (result)
			pr_err("lwvmw: no FTL profile for %u x %u\n", geo.blocks, geo.mlc_pages);
		else
			pr_info("lwvmw: package profile %s\n", sftl_geo.name);
	}
	if (!result)
		result = load(from, &f);
	if (!result)
		result = load(to, &t);
	if (result) {
		pr_err("lwvmw: need from= and to=, 4096-byte files (%d)\n", result);
	} else if (!dry_run && !apple_ppn_write_allowed(ppn)) {
		pr_err("lwvmw: apple_ppn not loaded with allow_write=1\n");
		result = -EPERM;
	} else {
		a.hw = ppn;
		a.from = f;
		a.to = t;
		result = lwvmw_run(&a);
	}
	pr_info("lwvmw: done: %d\n", result);
	vfree(f);
	vfree(t);
	apple_ppn_put(ppn);
	return 0;
}

static void __exit w_exit(void)
{
}

module_init(w_init);
module_exit(w_exit);
MODULE_DESCRIPTION("P105 NAND: write a planned LwVM header through the FTL");
MODULE_LICENSE("GPL");
