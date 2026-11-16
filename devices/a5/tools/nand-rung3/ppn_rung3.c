// SPDX-License-Identifier: GPL-2.0-only
/*
 * Kernel test module for ladder rung 3 (spec section 8.2): FTL writes to one
 * erased spare superblock, BTOC checks, remount, erase, remount.  The test
 * itself is rung3.c, shared with the offline harness; the FTL core is built
 * into this module from the driver sources, so apple_ftl need not be loaded
 * and must not be.  Needs apple_ppn with allow_write=1.
 *
 * Always loads (busybox insmod repeats a failed load); the outcome is the
 * "result" parameter and the kernel log, lines "sftl: rung3:".
 */
#include <linux/module.h>
#include <linux/moduleparam.h>

#include "apple_ppn.h"
#include "sftl.h"

#include "sftl_map.c"
#include "sftl_mount.c"
#include "sftl_format.c"
#include "sftl_write.c"
#include "rung3.c"

static unsigned int nlbas = 2000;
module_param(nlbas, uint, 0444);
static unsigned int search_from = 100000;
module_param(search_from, uint, 0444);
static int result = 1;
module_param(result, int, 0444);
MODULE_PARM_DESC(result, "0 = passed, negative errno = failed");

static int r3_read(void *hw, unsigned int bank, unsigned int block, unsigned int page,
		   bool slc, u8 *data, u8 *meta, u8 *status)
{
	return apple_ppn_read_page(hw, bank, block, page, slc, data, meta, status);
}

static int r3_erase(void *hw, unsigned int bank, unsigned int block)
{
	return apple_ppn_erase(hw, bank, block);
}

static int r3_program(void *hw, const struct sftl_prog *pages, unsigned int n)
{
	return apple_ppn_program(hw, pages, n);
}

static const struct sftl_ops r3_ops = {
	.read_page = r3_read,
	.erase_block = r3_erase,
	.program = r3_program,
};

static int __init r3_init(void)
{
	struct apple_ppn *ppn = apple_ppn_get();
	struct rung3_args a = { .ops = &r3_ops, .nlbas = nlbas, .search_from = search_from };

	if (!ppn) {
		pr_err("rung3: no PPN controller bound\n");
		result = -ENODEV;
		return 0;
	}
	if (!apple_ppn_write_allowed(ppn)) {
		pr_err("rung3: apple_ppn not loaded with allow_write=1\n");
		result = -EPERM;
	} else {
		a.hw = ppn;
		result = rung3_run(&a);
	}
	pr_info("rung3: done: %d\n", result);
	apple_ppn_put(ppn);
	return 0;
}

static void __exit r3_exit(void)
{
}

module_init(r3_init);
module_exit(r3_exit);
MODULE_DESCRIPTION("P105 NAND ladder rung 3: FTL writes that are undone");
MODULE_LICENSE("GPL");
