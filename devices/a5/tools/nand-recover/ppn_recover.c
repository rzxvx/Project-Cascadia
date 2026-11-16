// SPDX-License-Identifier: GPL-2.0-only
/*
 * Kernel module around recover.c.  The FTL core is built in, so apple_ftl
 * must not be loaded; apple_ppn needs allow_write=1 unless dry_run=1.
 *
 *   insmod ppn_recover.ko [give_up=198] [erase=453,468,514,694] [dry_run=1]
 *
 * Always loads; the outcome is the "result" parameter and lines "sftl: recover:".
 */
#include <linux/module.h>
#include <linux/moduleparam.h>

#include "apple_ppn.h"
#include "sftl.h"

#include "sftl_map.c"
#include "sftl_mount.c"
#include "sftl_format.c"
#include "sftl_write.c"
#include "recover.c"

static unsigned int erase[RECOVER_MAX_ERASE];
static int nerase;
module_param_array(erase, uint, &nerase, 0444);
static unsigned int give_up[RECOVER_MAX_ERASE];
static int ngive;
module_param_array(give_up, uint, &ngive, 0444);
static bool dry_run;
module_param(dry_run, bool, 0444);
static int result = 1;
module_param(result, int, 0444);
MODULE_PARM_DESC(result, "0 = passed, negative errno = failed");

static int r_read(void *hw, unsigned int bank, unsigned int block, unsigned int page,
		  bool slc, u8 *data, u8 *meta, u8 *status)
{
	return apple_ppn_read_page(hw, bank, block, page, slc, data, meta, status);
}

static int r_erase(void *hw, unsigned int bank, unsigned int block)
{
	return apple_ppn_erase(hw, bank, block);
}

static int r_program(void *hw, const struct sftl_prog *pages, unsigned int n)
{
	return apple_ppn_program(hw, pages, n);
}

static const struct sftl_ops r_ops = {
	.read_page = r_read,
	.erase_block = r_erase,
	.program = r_program,
};

static int __init r_init(void)
{
	struct apple_ppn *ppn = apple_ppn_get();
	struct recover_args a = { .ops = &r_ops, .dry_run = dry_run };
	int i;

	if (!ppn) {
		pr_err("recover: no PPN controller bound\n");
		result = -ENODEV;
		return 0;
	}
	for (i = 0; i < nerase; i++)
		a.erase[i] = erase[i];
	a.nerase = nerase;
	for (i = 0; i < ngive; i++)
		a.give_up[i] = give_up[i];
	a.ngive = ngive;
	if (!dry_run && !apple_ppn_write_allowed(ppn)) {
		pr_err("recover: apple_ppn not loaded with allow_write=1\n");
		result = -EPERM;
	} else {
		a.hw = ppn;
		result = recover_run(&a);
	}
	pr_info("recover: done: %d\n", result);
	apple_ppn_put(ppn);
	return 0;
}

static void __exit r_exit(void)
{
}

module_init(r_init);
module_exit(r_exit);
MODULE_DESCRIPTION("P105 NAND: recovery after a writer that stopped without a commit");
MODULE_LICENSE("GPL");
