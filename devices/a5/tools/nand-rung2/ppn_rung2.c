// SPDX-License-Identifier: GPL-2.0-only
/*
 * Ladder rung 2 of the P105 NAND specification (section 8.2): program, erase
 * and read cycles on the six reserved blocks the VFL marks as available, and
 * nowhere else.  It drives apple_ppn directly; the FTL is not involved.
 *
 * Actions (module parameter "action"), all reported in the kernel log:
 *   survey  read only: status summary of the six blocks and a checksum of each
 *           of them and of their neighbours on the same bank;
 *   page    one block: erase, program page 0, read it back, erase again;
 *   cycle   one block: erase, program every page, read all back, erase, and
 *           check that every page reads erased.
 * The module stays loaded whatever the outcome; read the "result" parameter,
 * then rmmod it.  "page" and "cycle" take bank=, block= and slc=, need apple_ppn loaded with
 * allow_write=1, and refuse a block that is not one of the six or that does
 * not read fully erased first.
 *
 * Credit: the hardware findings this test builds on are crystal-lized's.
 */

#include <linux/crc32.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/random.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include "apple_ppn.h"
#include "sftl.h"

#define PAGE_DATA	16384u
#define PAGE_META	64u
#define PAGES_MLC	256u
#define PAGES_SLC	128u
#define ST_READY	0x40
#define ST_ERASED	0x49

static char *action = "survey";
module_param(action, charp, 0444);
static unsigned int bank;
module_param(bank, uint, 0444);
static unsigned int block;
module_param(block, uint, 0444);
static bool slc;
module_param(slc, bool, 0444);
static unsigned int seed;
module_param(seed, uint, 0444);
static int result = 1;
module_param(result, int, 0444);
MODULE_PARM_DESC(result, "Outcome of the action: 0 = passed, negative errno = failed");

static const struct { u8 bank; u16 block; } reserved[] = {
	{ 0, 1054 }, { 1, 1054 }, { 1, 1055 }, { 1, 1056 }, { 3, 1055 }, { 3, 1056 },
};

static struct apple_ppn *ppn;
static u8 *rdata, *rmeta, *wdata, *wmeta;

static bool is_reserved(unsigned int bk, unsigned int bl)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(reserved); i++)
		if (reserved[i].bank == bk && reserved[i].block == bl)
			return true;
	return false;
}

/* A pattern that differs per bank, block, page, mode and seed. */
static void fill(unsigned int bk, unsigned int bl, unsigned int pg, bool s)
{
	u32 x = 0x9e3779b9u ^ seed ^ bk << 28 ^ bl << 12 ^ pg ^ (s ? 0x80000000u : 0);
	u32 *w = (u32 *)wdata;
	unsigned int i;

	for (i = 0; i < PAGE_DATA / 4; i++) {
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		w[i] = x;
	}
	w = (u32 *)wmeta;
	for (i = 0; i < PAGE_META / 4; i++) {
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		w[i] = x;
	}
}

static bool all_ff(const u8 *p, unsigned int n)
{
	return !memchr_inv(p, 0xff, n);
}

/* Status histogram of a whole block, and whether every page reads erased. */
static int block_summary(unsigned int bk, unsigned int bl, bool s, u32 *crc, bool *erased)
{
	static unsigned int hist[256];
	unsigned int pages = s ? PAGES_SLC : PAGES_MLC, pg, i, not_ff = 0;
	u32 c = ~0u;
	u8 st;
	int ret;

	memset(hist, 0, sizeof(hist));
	*erased = true;
	for (pg = 0; pg < pages; pg++) {
		ret = apple_ppn_read_page(ppn, bk, bl, pg, s, rdata, rmeta, &st);
		if (ret) {
			pr_err("rung2: read bank %u block %u page %u slc %d: %d status %#x\n",
			       bk, bl, pg, s, ret, st);
			return ret;
		}
		hist[st]++;
		c = crc32_le(c, &st, 1);
		c = crc32_le(c, rdata, PAGE_DATA);
		c = crc32_le(c, rmeta, PAGE_META);
		if (st != ST_ERASED)
			*erased = false;
		if (!all_ff(rdata, PAGE_DATA) || !all_ff(rmeta, PAGE_META))
			not_ff++;
	}
	*crc = ~c;
	pr_info("rung2: bank %u block %u %s: crc %08x, %u pages not all-FF, statuses:",
		bk, bl, s ? "SLC" : "MLC", *crc, not_ff);
	for (i = 0; i < 256; i++)
		if (hist[i])
			pr_cont(" %02x:%u", i, hist[i]);
	pr_cont("\n");
	return 0;
}

static int do_survey(void)
{
	unsigned int i;
	bool erased;
	u32 crc;
	int ret;

	for (i = 0; i < ARRAY_SIZE(reserved); i++) {
		unsigned int bk = reserved[i].bank, bl = reserved[i].block;

		ret = block_summary(bk, bl, false, &crc, &erased);
		if (!ret)
			ret = block_summary(bk, bl, true, &crc, &erased);
		if (!ret && !is_reserved(bk, bl - 1))
			ret = block_summary(bk, bl - 1, false, &crc, &erased);
		if (!ret && !is_reserved(bk, bl + 1))
			ret = block_summary(bk, bl + 1, false, &crc, &erased);
		if (ret)
			return ret;
	}
	return 0;
}

static int erase(unsigned int bk, unsigned int bl)
{
	int ret = apple_ppn_erase(ppn, bk, bl);

	pr_info("rung2: erase bank %u block %u: %d\n", bk, bl, ret);
	return ret;
}

static int program(unsigned int bk, unsigned int bl, unsigned int pg, bool s)
{
	struct sftl_prog p = {
		.bank = bk, .slc = s, .block = bl, .page = pg,
		.data = wdata, .meta = wmeta,
	};

	fill(bk, bl, pg, s);
	return apple_ppn_program(ppn, &p, 1);
}

/* Read a page back and compare it with its pattern. */
static int verify(unsigned int bk, unsigned int bl, unsigned int pg, bool s)
{
	u8 st;
	int ret;

	ret = apple_ppn_read_page(ppn, bk, bl, pg, s, rdata, rmeta, &st);
	if (ret || st != ST_READY) {
		pr_err("rung2: verify page %u: read %d status %#x\n", pg, ret, st);
		return ret ? ret : -EIO;
	}
	fill(bk, bl, pg, s);
	if (memcmp(rdata, wdata, PAGE_DATA) || memcmp(rmeta, wmeta, PAGE_META)) {
		unsigned int i;

		for (i = 0; i < PAGE_DATA && rdata[i] == wdata[i]; i++)
			;
		pr_err("rung2: verify page %u: mismatch, first data byte %u; meta wrote %*ph read %*ph\n",
		       pg, i, 16, wmeta, 16, rmeta);
		print_hex_dump(KERN_ERR, "rung2 read ", DUMP_PREFIX_OFFSET, 16, 1,
			       rdata, 64, false);
		print_hex_dump(KERN_ERR, "rung2 want ", DUMP_PREFIX_OFFSET, 16, 1,
			       wdata, 64, false);
		return -EIO;
	}
	return 0;
}

static int check_erased(unsigned int bk, unsigned int bl, bool s)
{
	bool erased;
	u32 crc;
	int ret;

	ret = block_summary(bk, bl, s, &crc, &erased);
	if (!ret && !erased) {
		pr_err("rung2: bank %u block %u does not read erased in %s\n",
		       bk, bl, s ? "SLC" : "MLC");
		ret = -EIO;
	}
	return ret;
}

static int do_page(void)
{
	u8 st;
	int ret;

	ret = check_erased(bank, block, slc);
	if (!ret)
		ret = erase(bank, block);
	if (!ret) {
		ret = program(bank, block, 0, slc);
		pr_info("rung2: program bank %u block %u page 0 slc %d: %d\n",
			bank, block, slc, ret);
	}
	if (!ret)
		ret = verify(bank, block, 0, slc);
	if (!ret) {
		ret = apple_ppn_read_page(ppn, bank, block, 1, slc, rdata, rmeta, &st);
		pr_info("rung2: page 1 after: %d status %#x\n", ret, st);
	}
	if (!ret)
		ret = erase(bank, block);
	if (!ret)
		ret = check_erased(bank, block, slc);
	return ret;
}

static int do_cycle(void)
{
	unsigned int pages = slc ? PAGES_SLC : PAGES_MLC, pg;
	ktime_t t0;
	int ret;

	ret = check_erased(bank, block, slc);
	if (!ret)
		ret = erase(bank, block);
	if (ret)
		return ret;
	t0 = ktime_get();
	for (pg = 0; pg < pages; pg++) {
		ret = program(bank, block, pg, slc);
		if (ret) {
			pr_err("rung2: program page %u: %d\n", pg, ret);
			return ret;
		}
	}
	pr_info("rung2: programmed %u pages in %lld ms\n", pages,
		ktime_ms_delta(ktime_get(), t0));
	for (pg = 0; pg < pages; pg++) {
		ret = verify(bank, block, pg, slc);
		if (ret)
			return ret;
	}
	pr_info("rung2: all %u pages verified\n", pages);
	ret = erase(bank, block);
	if (!ret)
		ret = check_erased(bank, block, false);
	if (!ret)
		ret = check_erased(bank, block, true);
	return ret;
}

static int __init rung2_init(void)
{
	int ret;

	ppn = apple_ppn_get();
	if (!ppn) {
		pr_err("rung2: no PPN controller bound\n");
		return -ENODEV;
	}
	rdata = vmalloc(PAGE_DATA);
	wdata = vmalloc(PAGE_DATA);
	rmeta = kmalloc(PAGE_META, GFP_KERNEL);
	wmeta = kmalloc(PAGE_META, GFP_KERNEL);
	if (!rdata || !wdata || !rmeta || !wmeta) {
		ret = -ENOMEM;
		goto out;
	}

	if (!strcmp(action, "survey")) {
		ret = do_survey();
	} else if (!strcmp(action, "page") || !strcmp(action, "cycle")) {
		if (!is_reserved(bank, block)) {
			pr_err("rung2: bank %u block %u is not a reserved block\n", bank, block);
			ret = -EINVAL;
		} else if (!apple_ppn_write_allowed(ppn)) {
			pr_err("rung2: apple_ppn not loaded with allow_write=1\n");
			ret = -EPERM;
		} else {
			ret = action[0] == 'p' ? do_page() : do_cycle();
		}
	} else {
		ret = -EINVAL;
	}
	pr_info("rung2: %s done: %d\n", action, ret);
out:
	vfree(rdata);
	vfree(wdata);
	kfree(rmeta);
	kfree(wmeta);
	apple_ppn_put(ppn);
	/*
	 * Always load, even after a failure: busybox insmod repeats a failed
	 * load through a second system call, which would run the action twice.
	 * The outcome is the "result" parameter; unload with rmmod.
	 */
	result = ret;
	return 0;
}

static void __exit rung2_exit(void)
{
}

module_init(rung2_init);
module_exit(rung2_exit);
MODULE_DESCRIPTION("P105 NAND ladder rung 2: reserved-block program/erase test");
MODULE_LICENSE("GPL");
