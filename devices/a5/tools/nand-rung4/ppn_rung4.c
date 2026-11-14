// SPDX-License-Identifier: GPL-2.0-only
/*
 * Kernel test module for ladder rung 4 (spec section 8.2): a whole context
 * written to one erased spare superblock, loaded by a fresh mount, erased.
 * The test is rung4.c, shared with the offline harness; the FTL core is built
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
#include "rung4.c"

static int result = 1;
module_param(result, int, 0444);
MODULE_PARM_DESC(result, "0 = passed, negative errno = failed");

static int r4_read(void *hw, unsigned int bank, unsigned int block, unsigned int page,
		   bool slc, u8 *data, u8 *meta, u8 *status)
{
	return apple_ppn_read_page(hw, bank, block, page, slc, data, meta, status);
}

static int r4_erase_block(void *hw, unsigned int bank, unsigned int block)
{
	return apple_ppn_erase(hw, bank, block);
}

static int r4_program(void *hw, const struct sftl_prog *pages, unsigned int n)
{
	return apple_ppn_program(hw, pages, n);
}

static const struct sftl_ops r4_ops = {
	.read_page = r4_read,
	.erase_block = r4_erase_block,
	.program = r4_program,
};

static int __init r4_init(void)
{
	struct apple_ppn *ppn = apple_ppn_get();
	struct rung4_args a = { .ops = &r4_ops };

	if (!ppn) {
		result = -ENODEV;
		return 0;
	}
	if (!apple_ppn_write_allowed(ppn)) {
		pr_err("rung4: apple_ppn not loaded with allow_write=1\n");
		result = -EPERM;
	} else {
		a.hw = ppn;
		result = rung4_run(&a);
	}
	pr_info("rung4: done: %d\n", result);
	apple_ppn_put(ppn);
	return 0;
}

static void __exit r4_exit(void)
{
}

module_init(r4_init);
module_exit(r4_exit);
MODULE_DESCRIPTION("P105 NAND ladder rung 4: context written unused");
MODULE_LICENSE("GPL");
