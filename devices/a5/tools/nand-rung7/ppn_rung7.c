// SPDX-License-Identifier: GPL-2.0-only
/*
 * Kernel test module for ladder rung 7 (spec section 8.2): a real reclaim
 * (GC) with its commit, left on the media.
 * The test is rung7.c, shared with the offline harness; the FTL core is built
 * in, so apple_ftl must not be loaded.  Needs apple_ppn with allow_write=1.
 * Always loads; the outcome is the "result" parameter and the kernel log.
 */
#include <linux/module.h>
#include <linux/moduleparam.h>

#include "apple_ppn.h"
#include "sftl.h"

#include "sftl_map.c"
#include "sftl_mount.c"
#include "sftl_format.c"
#include "sftl_write.c"
#include "rung7.c"

static int result = 1;
module_param(result, int, 0444);
MODULE_PARM_DESC(result, "0 = passed, negative errno = failed");

static int r7_read(void *hw, unsigned int bank, unsigned int block, unsigned int page,
		   bool slc, u8 *data, u8 *meta, u8 *status)
{
	return apple_ppn_read_page(hw, bank, block, page, slc, data, meta, status);
}

static int r7_erase_block(void *hw, unsigned int bank, unsigned int block)
{
	return apple_ppn_erase(hw, bank, block);
}

static int r7_program(void *hw, const struct sftl_prog *pages, unsigned int n)
{
	return apple_ppn_program(hw, pages, n);
}

static const struct sftl_ops r7_ops = {
	.read_page = r7_read,
	.erase_block = r7_erase_block,
	.program = r7_program,
};

static int __init r7_init(void)
{
	struct apple_ppn *ppn = apple_ppn_get();
	struct rung7_args a = { .ops = &r7_ops };

	if (!ppn) {
		result = -ENODEV;
		return 0;
	}
	if (!apple_ppn_write_allowed(ppn)) {
		pr_err("rung7: apple_ppn not loaded with allow_write=1\n");
		result = -EPERM;
	} else {
		a.hw = ppn;
		result = rung7_run(&a);
	}
	pr_info("rung7: done: %d\n", result);
	apple_ppn_put(ppn);
	return 0;
}

static void __exit r7_exit(void)
{
}

module_init(r7_init);
module_exit(r7_exit);
MODULE_DESCRIPTION("P105 NAND ladder rung 7: real reclaim and commit");
MODULE_LICENSE("GPL");
