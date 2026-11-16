// SPDX-License-Identifier: GPL-2.0-only
/* Read-only kernel wrapper of ctxdump.c; always loads, result parameter. */
#define SFTL_NO_WRITE
#include <linux/module.h>
#include <linux/moduleparam.h>

#include "apple_ppn.h"
#include "sftl.h"

#include "sftl_map.c"
#include "sftl_mount.c"
#include "ctxdump.c"

static int result = 1;
module_param(result, int, 0444);

static int d_read(void *hw, unsigned int bank, unsigned int block, unsigned int page,
		  bool slc, u8 *data, u8 *meta, u8 *status)
{
	return apple_ppn_read_page(hw, bank, block, page, slc, data, meta, status);
}

static const struct sftl_ops d_ops = { .read_page = d_read };

static int __init d_init(void)
{
	struct apple_ppn *ppn = apple_ppn_get();

	if (!ppn) {
		result = -ENODEV;
		return 0;
	}
	result = ctxdump_run(&d_ops, ppn);
	apple_ppn_put(ppn);
	return 0;
}

static void __exit d_exit(void)
{
}

module_init(d_init);
module_exit(d_exit);
MODULE_LICENSE("GPL");
