/* SPDX-License-Identifier: GPL-2.0-only */
/* Host/kernel shims for the portable Swiss FTL core. */
#ifndef SFTL_PORT_H
#define SFTL_PORT_H

#ifdef __KERNEL__
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/string.h>
#include <linux/printk.h>
#include <linux/sort.h>

#define sftl_alloc(n)	kvzalloc((n), GFP_KERNEL)
#define sftl_free(p)	kvfree(p)
#define sftl_info(fmt, ...)	pr_info("sftl: " fmt "\n", ##__VA_ARGS__)
#define sftl_warn(fmt, ...)	pr_warn("sftl: " fmt "\n", ##__VA_ARGS__)
#define sftl_err(fmt, ...)	pr_err("sftl: " fmt "\n", ##__VA_ARGS__)
#else
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static inline void *sftl_alloc(size_t n)
{
	return calloc(1, n);
}

#define sftl_free(p)	free(p)
#define sftl_info(fmt, ...)	printf("sftl: " fmt "\n", ##__VA_ARGS__)
#define sftl_warn(fmt, ...)	printf("sftl: WARN " fmt "\n", ##__VA_ARGS__)
#define sftl_err(fmt, ...)	printf("sftl: ERR " fmt "\n", ##__VA_ARGS__)
#endif

#endif
