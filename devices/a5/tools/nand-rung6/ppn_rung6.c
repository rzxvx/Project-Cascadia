// SPDX-License-Identifier: GPL-2.0-only
/*
 * Kernel test module for ladder rung 6 (spec section 8.2): action=commit
 * commits a Linux context and leaves it (needs apple_ppn allow_write=1);
 * action=check is read only, after iOS has run.  The test is rung6.c, shared
 * with the offline harness; the FTL core is built in, so apple_ftl must not
 * be loaded.  Always loads; the outcome is the "result" parameter and the log.
 */
#include <linux/module.h>
#include <linux/moduleparam.h>

#include "apple_ppn.h"
#include "sftl.h"

#include "sftl_map.c"
#include "sftl_mount.c"
#include "sftl_format.c"
#include "sftl_write.c"
#include "sftl_lwvm.c"
#include "rung6.c"

static char *action = "check";
module_param(action, charp, 0444);
static unsigned int samples = 20000;
module_param(samples, uint, 0444);
static unsigned int watch[RUNG6_WATCH];
static int nwatch;
module_param_array(watch, uint, &nwatch, 0444);
static int result = 1;
module_param(result, int, 0444);
MODULE_PARM_DESC(result, "0 = passed, negative errno = failed");

static int r6_read(void *hw, unsigned int bank, unsigned int block, unsigned int page,
		   bool slc, u8 *data, u8 *meta, u8 *status)
{
	return apple_ppn_read_page(hw, bank, block, page, slc, data, meta, status);
}

static int r6_erase_block(void *hw, unsigned int bank, unsigned int block)
{
	return apple_ppn_erase(hw, bank, block);
}

static int r6_program(void *hw, const struct sftl_prog *pages, unsigned int n)
{
	return apple_ppn_program(hw, pages, n);
}

static const struct sftl_ops r6_ops = {
	.read_page = r6_read,
	.erase_block = r6_erase_block,
	.program = r6_program,
};

static int __init r6_init(void)
{
	struct apple_ppn *ppn = apple_ppn_get();
	struct rung6_args a = { .ops = &r6_ops, .samples = samples };
	int i;

	if (!ppn) {
		result = -ENODEV;
		return 0;
	}
	a.hw = ppn;
	for (i = 0; i < nwatch; i++)
		a.watch_sb[i] = watch[i];
	if (!strcmp(action, "commit")) {
		a.commit = true;
		if (!apple_ppn_write_allowed(ppn)) {
			pr_err("rung6: apple_ppn not loaded with allow_write=1\n");
			result = -EPERM;
			goto out;
		}
	} else if (strcmp(action, "check")) {
		result = -EINVAL;
		goto out;
	}
	result = rung6_run(&a);
out:
	pr_info("rung6: %s done: %d\n", action, result);
	apple_ppn_put(ppn);
	return 0;
}

static void __exit r6_exit(void)
{
}

module_init(r6_init);
module_exit(r6_exit);
MODULE_DESCRIPTION("P105 NAND ladder rung 6: iOS on a Linux context");
MODULE_LICENSE("GPL");
