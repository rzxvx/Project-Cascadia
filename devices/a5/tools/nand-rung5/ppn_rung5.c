// SPDX-License-Identifier: GPL-2.0-only
/*
 * Kernel test module for ladder rung 5 (spec section 8.2): a Linux context
 * committed to one erased spare superblock, loaded, and withdrawn.
 * The test is rung5.c, shared with the offline harness; the FTL core is built
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
#include "rung5.c"

static int result = 1;
module_param(result, int, 0444);
MODULE_PARM_DESC(result, "0 = passed, negative errno = failed");

static int r5_read(void *hw, unsigned int bank, unsigned int block, unsigned int page,
		   bool slc, u8 *data, u8 *meta, u8 *status)
{
	return apple_ppn_read_page(hw, bank, block, page, slc, data, meta, status);
}

static int r5_erase_block(void *hw, unsigned int bank, unsigned int block)
{
	return apple_ppn_erase(hw, bank, block);
}

static int r5_program(void *hw, const struct sftl_prog *pages, unsigned int n)
{
	return apple_ppn_program(hw, pages, n);
}

static const struct sftl_ops r5_ops = {
	.read_page = r5_read,
	.erase_block = r5_erase_block,
	.program = r5_program,
};

static int __init r5_init(void)
{
	struct apple_ppn *ppn = apple_ppn_get();
	struct rung5_args a = { .ops = &r5_ops };

	if (!ppn) {
		result = -ENODEV;
		return 0;
	}
	if (!apple_ppn_write_allowed(ppn)) {
		pr_err("rung5: apple_ppn not loaded with allow_write=1\n");
		result = -EPERM;
	} else {
		a.hw = ppn;
		result = rung5_run(&a);
	}
	pr_info("rung5: done: %d\n", result);
	apple_ppn_put(ppn);
	return 0;
}

static void __exit r5_exit(void)
{
}

module_init(r5_init);
module_exit(r5_exit);
MODULE_DESCRIPTION("P105 NAND ladder rung 5: Linux context committed and withdrawn");
MODULE_LICENSE("GPL");
