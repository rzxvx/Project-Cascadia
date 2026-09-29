// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple S5L H2FMI + Hynix PPN NAND -- READ ONLY.
 *
 * The iPad mini 1 has two PPN packages, one on CE0 of each of the SoC's two
 * flash buses.  "PPN" means the package has its own controller: it does the
 * ECC and part of the bad-block work itself, so this is a *managed* NAND and
 * deliberately NOT a driver on top of drivers/mtd/nand/raw -- that framework
 * assumes the host computes ECC and would have to be fought the whole way.
 * What it is instead is a plain mtd_info per die with its own _read.
 *
 * Every register use and every command sequence here comes from iBEC's own
 * disassembly (H2fmi.c, H2fmi_ppn.c, fmiss_ppn.c) by way of the userspace tool
 * this is a port of, tools/nand/ppn.c, which read the whole NAND bit-for-bit
 * repeatably.  docs/research/p105-nand.md is the notebook.
 *
 * Geometry: 2 buses x 1 CE x 2 CAUs x 1064 blocks x 256 pages.  A page comes
 * off the bus as 16448 bytes -- four times (1024 data, 16 metadata, 3072
 * data).  The metadata is the FTL's, and becomes MTD's OOB; strip it and a
 * page is 16 KiB of data with 64 bytes of OOB.  One MTD is exported per die,
 * which is also how the verified raw dumps in docs are laid out, so a fresh
 * read can be diffed against them byte for byte.
 *
 * READ ONLY, and structurally so: the opcode allowlist below has no program
 * (80/10, PPN boot page 8A/17) and no erase (60/D0, PPN 67) in it, and there
 * is no code path that could issue one.  Writing is a separate job that costs
 * iOS the first time it runs; see the README.
 */

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mtd/mtd.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#define H2FMI_BUSES		2
#define H2FMI_CAUS		2
#define H2FMI_DIES		(H2FMI_BUSES * H2FMI_CAUS)
#define H2FMI_BLOCKS		1064
#define H2FMI_PAGES		256

#define H2FMI_CHUNKS		4		/* per page */
#define H2FMI_CHUNK_LO		1024		/* data before the metadata */
#define H2FMI_CHUNK_META	16
#define H2FMI_CHUNK_HI		3072		/* data after it */
#define H2FMI_CHUNK_RAW		(H2FMI_CHUNK_LO + H2FMI_CHUNK_META + H2FMI_CHUNK_HI)

#define H2FMI_PAGE_RAW		(H2FMI_CHUNKS * H2FMI_CHUNK_RAW)	/* 16448 */
#define H2FMI_PAGE_DATA		(H2FMI_CHUNKS * (H2FMI_CHUNK_LO + H2FMI_CHUNK_HI))
#define H2FMI_PAGE_OOB		(H2FMI_CHUNKS * H2FMI_CHUNK_META)	/* 64 */
#define H2FMI_ERASESIZE		((u32)H2FMI_PAGES * H2FMI_PAGE_DATA)	/* 4 MiB */

/* Data page and erase block are both powers of two (16 KiB and 4 MiB), so an
 * offset splits with shifts and masks.  This is a 32-bit kernel and the MTD is
 * 4 GiB, so every offset is 64-bit: a plain / or % here would be a link error
 * for __aeabi_uldivmod, which is exactly the kind of thing that only shows up
 * at the end of a build. */
#define H2FMI_PAGE_SHIFT	14
#define H2FMI_PAGES_SHIFT	8			/* 256 pages a block */
#define H2FMI_PAGE_MASK		((u32)H2FMI_PAGE_DATA - 1)

/* The ADT's ce-bitmap is 0x101: CE0 on each bus, and nothing else.  Addressing
 * a CE with no chip behind it wedges the FMC in DDR -- it waits for a DQS that
 * never comes -- so this is never computed, only ever CE0. */
#define H2FMI_CE			0

/* FMI: the DMA/PIO side.  FMC: the NAND bus.  Offsets as iBEC uses them. */
#define FMI_CONFIG		0x00
#define FMI_CONTROL		0x04
#define FMI_STATUS		0x0c
#define FMI_INTEN		0x10
#define FMI_DATA		0x14	/* PIO FIFO; a read pops it */
#define FMI_DMA_STATUS		0x1c
#define FMI_PIO_CONFIG		0x34

#define FMC_ON			0x00
#define FMC_IF_CTRL		0x08
#define FMC_CE_CTRL		0x0c
#define FMC_RW_CTRL		0x10
#define FMC_CMD			0x14
#define FMC_ADDR0		0x18
#define FMC_ADDRNUM		0x20
#define FMC_DATANUM		0x24
#define FMC_INTMASK		0x40
#define FMC_STATUS		0x44
#define FMC_NAND_STATUS		0x48
#define FMC_STATUS_MASK		0x4c

/* Page status: 0x40 good, 0x42 good but due a refresh, 0x45 every page of a
 * retired block, 0x49 erased. */
#define PPN_ST_GOOD		0x40
#define PPN_ST_REFRESH		0x42
#define PPN_ST_RETIRED		0x45
#define PPN_ST_ERASED		0x49

/* row for the page-read command */
#define PPN_ROW(cau, blk, pg) \
	((u32)(pg) | (u32)(blk) << 8 | (u32)(cau) << 19)

/* The allowlist is the safety property, not a convenience: a sequence that is
 * not on it does not run.  Nothing here programs or erases. */
static const u8 h2fmi_read_only_ops[][2] = {
	{ 0xff, 0x00 },		/* reset */
	{ 0x70, 0x00 },		/* read status */
	{ 0x77, 0x7d },		/* PPN operation status */
	{ 0x77, 0x00 },		/* PPN end of operation */
	{ 0x7a, 0x00 },		/* PPN data out */
	{ 0x0a, 0x37 },		/* PPN page read */
};

struct h2fmi_bus {
	void __iomem *fmi;
	void __iomem *fmc;
	void __iomem *ecc;
	struct mutex lock;	/* one command sequence on this bus at a time */
	u8 *page;		/* one raw page; only touched under lock */
};

struct h2fmi_die {
	struct h2fmi *fmi;
	u8 bus;
	u8 cau;
};

struct h2fmi {
	struct device *dev;
	struct h2fmi_bus bus[H2FMI_BUSES];
	struct mtd_info mtd[H2FMI_DIES];
	struct h2fmi_die die[H2FMI_DIES];
};

/*
 * h2fmi_wait_done: poll until (reg & mask) == want, then write want back --
 * these bits are write-one-to-clear.  iBEC gives up after 100 ms and so do we.
 */
static int h2fmi_wait(struct h2fmi *fmi, void __iomem *base, u32 off,
		      u32 mask, u32 want)
{
	u32 v;
	int ret;

	ret = readl_poll_timeout(base + off, v, (v & mask) == want, 0, 100000);
	if (ret) {
		dev_err(fmi->dev, "timeout: reg +0x%x = 0x%08x, wanted 0x%x/0x%x\n",
			off, v, mask, want);
		return ret;
	}
	writel(want, base + off);
	return 0;
}

/*
 * FMC_CMD holds the first command byte, and the second one in bits 15:8.
 * Every sequence goes through here, which is where the allowlist bites.
 */
static int h2fmi_cmd_write(struct h2fmi *fmi, struct h2fmi_bus *b, u8 op, u8 op2)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(h2fmi_read_only_ops); i++)
		if (h2fmi_read_only_ops[i][0] == op &&
		    h2fmi_read_only_ops[i][1] == op2) {
			writel(op | op2 << 8, b->fmc + FMC_CMD);
			return 0;
		}

	dev_err(fmi->dev, "refusing NAND opcodes %02x %02x: not read-only\n", op, op2);
	return -EPERM;
}

static void h2fmi_clear_irqs(struct h2fmi_bus *b)
{
	writel(0, b->fmc + FMC_INTMASK);
	writel(0, b->fmi + FMI_INTEN);
	writel(0x31ffff, b->fmc + FMC_STATUS);
	writel(0xf, b->fmi + FMI_STATUS);
}

/* single command, FMC_RW_CTRL bit 0 */
static int h2fmi_cmd1(struct h2fmi *fmi, struct h2fmi_bus *b, u8 op)
{
	int ret = h2fmi_cmd_write(fmi, b, op, 0);

	if (ret)
		return ret;
	writel(1, b->fmc + FMC_RW_CTRL);
	return h2fmi_wait(fmi, b->fmc, FMC_STATUS, 1, 1);
}

/* command, address bytes, second command: RW_CTRL 0xb */
static int h2fmi_cmd_addr_cmd(struct h2fmi *fmi, struct h2fmi_bus *b,
			      u8 op, u8 op2, u32 addr, int naddr)
{
	int ret = h2fmi_cmd_write(fmi, b, op, op2);

	if (ret)
		return ret;
	writel(addr, b->fmc + FMC_ADDR0);
	writel((naddr + 7) & 7, b->fmc + FMC_ADDRNUM);
	writel(0xb, b->fmc + FMC_RW_CTRL);
	return h2fmi_wait(fmi, b->fmc, FMC_STATUS, 0xb, 0xb);
}

/*
 * h2fmi_ppn_get_operation_status + h2fmi_get_nand_status: 77 7D, then let the
 * FMC poll the status byte until it is ready.  iBEC waits on the interrupt;
 * this polls FMC_STATUS bit 5, as the userspace tool does.
 */
static int h2fmi_status(struct h2fmi *fmi, struct h2fmi_bus *b, u8 *st)
{
	int ret;
	u32 v;

	ret = h2fmi_cmd_write(fmi, b, 0x77, 0x7d);
	if (ret)
		return ret;
	writel(3, b->fmc + FMC_RW_CTRL);
	ret = h2fmi_wait(fmi, b->fmc, FMC_STATUS, 3, 3);
	if (ret)
		return ret;

	h2fmi_clear_irqs(b);
	writel(0x100, b->fmi + FMI_INTEN);
	writel(readl(b->fmc + FMC_IF_CTRL) & ~0x100000u, b->fmc + FMC_IF_CTRL);
	writel(0x4040, b->fmc + FMC_STATUS_MASK);
	writel(0, b->fmc + FMC_DATANUM);
	writel(0x20, b->fmc + FMC_INTMASK);
	writel(0x50, b->fmc + FMC_RW_CTRL);

	ret = readl_poll_timeout(b->fmc + FMC_STATUS, v, v & 0x20, 0, 2000000);
	if (ret) {
		dev_err(fmi->dev, "status timeout: FMC+0x44 0x%08x FMI+0xc 0x%08x byte 0x%02x\n",
			v, readl(b->fmi + FMI_STATUS),
			(u8)readl(b->fmc + FMC_NAND_STATUS));
		writel(0, b->fmc + FMC_RW_CTRL);
		h2fmi_clear_irqs(b);
		return ret;
	}

	*st = readl(b->fmc + FMC_NAND_STATUS);
	writel(0, b->fmc + FMC_RW_CTRL);
	h2fmi_clear_irqs(b);
	return 0;
}

static int h2fmi_pio_read(struct h2fmi *fmi, struct h2fmi_bus *b,
			  u32 *dst, u32 bytes)
{
	u32 v;
	int ret;
	u32 i;

	ret = readl_poll_timeout(b->fmi + FMI_DMA_STATUS, v, v & 0x18, 0, 100000);
	if (ret) {
		dev_err(fmi->dev, "PIO timeout: FMI+0x1c = 0x%08x\n", v);
		return ret;
	}
	for (i = 0; i < bytes / 4; i++)
		dst[i] = readl(b->fmi + FMI_DATA);
	return 0;
}

/*
 * h2fmi_ppn_read_data_out: 7A, then the bytes by PIO in runs of at most 0x400.
 * FMI_PIO_CONFIG is (bytes << 8) | sectors.
 */
static int h2fmi_data_out(struct h2fmi *fmi, struct h2fmi_bus *b,
			  u8 *buf, u32 len)
{
	u32 off;
	int ret;

	ret = h2fmi_cmd1(fmi, b, 0x7a);
	if (ret)
		return ret;

	for (off = 0; off < len; ) {
		u32 n = min_t(u32, len - off, 0x400);
		u32 nw = n < 4 ? 4 : n & ~3u;

		writel(5, b->fmi + FMI_CONFIG);
		writel((nw << 8 & 0x7fc00) | 1, b->fmi + FMI_PIO_CONFIG);
		writel(3, b->fmi + FMI_CONTROL);
		ret = h2fmi_pio_read(fmi, b, (u32 *)(buf + off), nw);
		if (ret)
			return ret;
		off += nw;
	}
	return 0;
}

/*
 * h2fmi_device_reset without the PMGR pulse: stop the FMI and put the FMC back
 * the way it was.  iBoot leaves it in DDR (FMC_ON = 5) and that has to survive
 * every sequence, because getting back to DDR afterwards needs a reboot.
 */
static void h2fmi_device_reset(struct h2fmi_bus *b, u32 on, u32 if_ctrl)
{
	writel(6, b->fmi + FMI_CONTROL);
	writel(on, b->fmc + FMC_ON);
	writel(if_ctrl, b->fmc + FMC_IF_CTRL);
}

/* One raw page, into b->page.  Caller holds b->lock. */
static int h2fmi_read_raw_page(struct h2fmi *fmi, struct h2fmi_bus *b,
			       u32 row, u8 *st)
{
	u32 on = readl(b->fmc + FMC_ON);
	u32 if_ctrl = readl(b->fmc + FMC_IF_CTRL);
	int ret;

	*st = 0;
	writel(1u << H2FMI_CE, b->fmc + FMC_CE_CTRL);
	h2fmi_clear_irqs(b);

	ret = h2fmi_cmd_addr_cmd(fmi, b, 0x0a, 0x37, row, 3);
	if (!ret)
		ret = h2fmi_status(fmi, b, st);
	if (!ret)
		ret = h2fmi_data_out(fmi, b, b->page, H2FMI_PAGE_RAW);

	h2fmi_cmd1(fmi, b, 0x77);
	writel(0, b->fmc + FMC_CE_CTRL);
	h2fmi_device_reset(b, on, if_ctrl);
	return ret;
}

/* Strip the FTL's metadata out of a raw page: 4 x (1024 data, 16 meta, 3072
 * data) becomes 16384 of data and, if asked, 64 of OOB. */
static void h2fmi_split_page(const u8 *raw, u8 *data, u8 *oob)
{
	int c;

	for (c = 0; c < H2FMI_CHUNKS; c++) {
		const u8 *src = raw + c * H2FMI_CHUNK_RAW;

		if (data) {
			u8 *dst = data + c * (H2FMI_CHUNK_LO + H2FMI_CHUNK_HI);

			memcpy(dst, src, H2FMI_CHUNK_LO);
			memcpy(dst + H2FMI_CHUNK_LO,
			       src + H2FMI_CHUNK_LO + H2FMI_CHUNK_META,
			       H2FMI_CHUNK_HI);
		}
		if (oob)
			memcpy(oob + c * H2FMI_CHUNK_META,
			       src + H2FMI_CHUNK_LO, H2FMI_CHUNK_META);
	}
}

/*
 * The one place that turns an MTD offset into a page and reads it.  Whole
 * pages are always read -- the PPN cannot hand over less than one -- and the
 * caller takes what it wants out of the bounce buffer.
 *
 * `from` must be page-aligned.  The page's status byte comes back in *stp; a
 * negative return means the bus failed, not that the data is bad.
 */
static int h2fmi_read_page_at(struct mtd_info *mtd, loff_t from,
			      u8 *data, u8 *oob, u8 *stp)
{
	struct h2fmi_die *die = mtd->priv;
	struct h2fmi *fmi = die->fmi;
	struct h2fmi_bus *b = &fmi->bus[die->bus];
	u64 page = (u64)from >> H2FMI_PAGE_SHIFT;
	u32 blk = (u32)(page >> H2FMI_PAGES_SHIFT);
	u32 pg = (u32)page & (H2FMI_PAGES - 1);
	u8 st = 0;
	int ret;

	if (blk >= H2FMI_BLOCKS)
		return -EINVAL;

	mutex_lock(&b->lock);
	ret = h2fmi_read_raw_page(fmi, b, PPN_ROW(die->cau, blk, pg), &st);
	if (!ret)
		h2fmi_split_page(b->page, data, oob);
	mutex_unlock(&b->lock);

	*stp = st;
	return ret;
}

/*
 * Turn the PPN's status byte into what MTD expects back from _read_oob: a
 * count of corrected bitflips, or a negative errno.
 *
 * The package corrects its own data and will not say how much it had to
 * correct, only whether the page wants rewriting -- so "wants rewriting"
 * becomes one bitflip, which is bitflip_threshold, which is what makes
 * mtd_read_oob() hand the caller -EUCLEAN.  That is the signal UBI scrubs on.
 * An erased page is not an error; a retired block is.
 */
static int h2fmi_bitflips(struct mtd_info *mtd, u8 st)
{
	switch (st) {
	case PPN_ST_GOOD:
	case PPN_ST_ERASED:
		return 0;
	case PPN_ST_REFRESH:
		return 1;
	default:
		dev_err_ratelimited(mtd->dev.parent,
				    "%s: page status 0x%02x\n", mtd->name, st);
		return -EBADMSG;
	}
}

/*
 * The whole read path.  mtd_read() builds an mtd_oob_ops and comes through
 * here too, so this has to cope with an unaligned start, any length, and
 * either buffer on its own -- a driver may implement _read or _read_oob but
 * not both, and _read_oob is the one that can carry the metadata.
 */
static int h2fmi_mtd_read_oob(struct mtd_info *mtd, loff_t from,
			      struct mtd_oob_ops *ops)
{
	size_t oobavail = mtd_oobavail(mtd, ops);
	size_t len = ops->len, ooblen = ops->ooblen;
	u8 *datbuf = ops->datbuf, *oobbuf = ops->oobbuf;
	u32 ooboffs = ops->ooboffs;
	u8 oob[H2FMI_PAGE_OOB];
	int maxbitflips = 0;
	u8 *page = NULL;
	int ret = 0;

	ops->retlen = 0;
	ops->oobretlen = 0;
	if (!len && !ooblen)
		return 0;

	while (len || ooblen) {
		loff_t base = from & ~(loff_t)H2FMI_PAGE_MASK;
		u32 skip = (u32)(from - base);
		bool direct = false;
		u8 *dst = NULL;
		u8 st = 0;
		int bf;

		/* A whole aligned page -- which is every page of a large read,
		 * and so nearly all of them -- is split straight into the
		 * caller's buffer.  The bounce is only for a partial page, and
		 * is only allocated if one turns up. */
		if (len) {
			if (!skip && len >= H2FMI_PAGE_DATA) {
				dst = datbuf;
				direct = true;
			} else {
				if (!page) {
					page = kmalloc(H2FMI_PAGE_DATA, GFP_KERNEL);
					if (!page) {
						ret = -ENOMEM;
						break;
					}
				}
				dst = page;
			}
		}

		ret = h2fmi_read_page_at(mtd, base, dst, oob, &st);
		if (ret)
			break;

		bf = h2fmi_bitflips(mtd, st);
		if (bf < 0) {
			ret = bf;
			break;
		}
		maxbitflips = max(maxbitflips, bf);

		if (len) {
			size_t n = min_t(size_t, len, H2FMI_PAGE_DATA - skip);

			if (!direct)
				memcpy(datbuf, page + skip, n);
			datbuf += n;
			len -= n;
			ops->retlen += n;
		}
		if (ooblen) {
			size_t n = min_t(size_t, ooblen, oobavail - ooboffs);

			memcpy(oobbuf, oob + ooboffs, n);
			oobbuf += n;
			ooblen -= n;
			ops->oobretlen += n;
			ooboffs = 0;
		}

		from = base + H2FMI_PAGE_DATA;
		if (from >= mtd->size)
			break;
	}

	kfree(page);
	return ret ? ret : maxbitflips;
}

/*
 * A retired block answers 0x45 on every page, which is the PPN saying so
 * directly -- there is no marker to interpret and no table to keep.
 */
static int h2fmi_mtd_block_isbad(struct mtd_info *mtd, loff_t ofs)
{
	u8 st = 0;
	int ret;

	ofs &= ~(loff_t)(mtd->erasesize - 1);
	ret = h2fmi_read_page_at(mtd, ofs, NULL, NULL, &st);
	if (ret)
		return ret;
	return st == PPN_ST_RETIRED;
}

static int h2fmi_setup_mtd(struct h2fmi *fmi, int idx)
{
	struct mtd_info *mtd = &fmi->mtd[idx];
	struct h2fmi_die *die = &fmi->die[idx];

	die->fmi = fmi;
	die->bus = idx / H2FMI_CAUS;
	die->cau = idx % H2FMI_CAUS;

	mtd->priv = die;
	mtd->dev.parent = fmi->dev;
	mtd->owner = THIS_MODULE;
	mtd->name = devm_kasprintf(fmi->dev, GFP_KERNEL, "apple-nand%d", idx);
	if (!mtd->name)
		return -ENOMEM;

	mtd->type = MTD_NANDFLASH;
	/* No MTD_WRITEABLE on purpose: this driver cannot write, and the NAND
	 * it is pointed at holds somebody's iOS.  MTD_NO_ERASE goes with it --
	 * add_mtd_device() rejects an erasesize with no ->_erase behind it
	 * unless the device says erasing is not a thing it does.  Both come off
	 * when the write path lands. */
	mtd->flags = MTD_NO_ERASE;
	mtd->size = (u64)H2FMI_BLOCKS * H2FMI_ERASESIZE;
	mtd->erasesize = H2FMI_ERASESIZE;
	mtd->writesize = H2FMI_PAGE_DATA;
	mtd->writebufsize = H2FMI_PAGE_DATA;
	mtd->oobsize = H2FMI_PAGE_OOB;
	mtd->oobavail = H2FMI_PAGE_OOB;
	/* The package corrects its own data and never says by how much, only
	 * whether the page wants rewriting.  So: one notional bit of strength,
	 * a threshold of one, and _read_oob returns 1 for "wants rewriting" --
	 * which mtd_read_oob() then reports to the caller as -EUCLEAN.  With
	 * ecc_strength left at 0 the core treats the device as having no ECC
	 * and throws that signal away. */
	mtd->ecc_strength = 1;
	mtd->bitflip_threshold = 1;

	/* _read_oob only: add_mtd_device() rejects a driver that has both. */
	mtd->_read_oob = h2fmi_mtd_read_oob;
	mtd->_block_isbad = h2fmi_mtd_block_isbad;

	return mtd_device_register(mtd, NULL, 0);
}

static int h2fmi_probe(struct platform_device *pdev)
{
	struct h2fmi *fmi;
	int i, ret;

	BUILD_BUG_ON(H2FMI_PAGE_DATA != (1 << H2FMI_PAGE_SHIFT));
	BUILD_BUG_ON(H2FMI_PAGES != (1 << H2FMI_PAGES_SHIFT));
	BUILD_BUG_ON(H2FMI_PAGE_RAW != 16448);

	fmi = devm_kzalloc(&pdev->dev, sizeof(*fmi), GFP_KERNEL);
	if (!fmi)
		return -ENOMEM;
	fmi->dev = &pdev->dev;

	/* Six regions, in the ADT's order: FMI, FMC, ECC for bus 0, then bus 1. */
	for (i = 0; i < H2FMI_BUSES; i++) {
		struct h2fmi_bus *b = &fmi->bus[i];

		b->fmi = devm_platform_ioremap_resource(pdev, i * 3 + 0);
		if (IS_ERR(b->fmi))
			return PTR_ERR(b->fmi);
		b->fmc = devm_platform_ioremap_resource(pdev, i * 3 + 1);
		if (IS_ERR(b->fmc))
			return PTR_ERR(b->fmc);
		b->ecc = devm_platform_ioremap_resource(pdev, i * 3 + 2);
		if (IS_ERR(b->ecc))
			return PTR_ERR(b->ecc);

		mutex_init(&b->lock);
		b->page = devm_kzalloc(&pdev->dev, H2FMI_PAGE_RAW, GFP_KERNEL);
		if (!b->page)
			return -ENOMEM;

		/* iBoot read the NAND to boot and leaves the block clocked and
		 * in DDR.  Nothing here turns it on: if iBoot did not, the
		 * state this driver would have to restore is not known. */
		dev_info(&pdev->dev, "bus %d: FMC_ON=0x%08x IF_CTRL=0x%08x\n",
			 i, readl(b->fmc + FMC_ON), readl(b->fmc + FMC_IF_CTRL));
	}

	for (i = 0; i < H2FMI_DIES; i++) {
		ret = h2fmi_setup_mtd(fmi, i);
		if (ret) {
			while (--i >= 0)
				mtd_device_unregister(&fmi->mtd[i]);
			return ret;
		}
	}

	platform_set_drvdata(pdev, fmi);
	dev_info(&pdev->dev, "%d dies, %u blocks of %u KiB each, read-only\n",
		 H2FMI_DIES, H2FMI_BLOCKS, H2FMI_ERASESIZE / 1024);
	return 0;
}

static void h2fmi_remove(struct platform_device *pdev)
{
	struct h2fmi *fmi = platform_get_drvdata(pdev);
	int i;

	for (i = 0; i < H2FMI_DIES; i++)
		mtd_device_unregister(&fmi->mtd[i]);
}

static const struct of_device_id h2fmi_of_match[] = {
	{ .compatible = "apple,s5l8940x-fmi" },
	{ }
};
MODULE_DEVICE_TABLE(of, h2fmi_of_match);

static struct platform_driver h2fmi_driver = {
	.probe = h2fmi_probe,
	.remove = h2fmi_remove,
	.driver = {
		.name = "apple-h2fmi",
		.of_match_table = h2fmi_of_match,
	},
};
module_platform_driver(h2fmi_driver);

MODULE_DESCRIPTION("Apple S5L H2FMI / Hynix PPN NAND, read-only");
MODULE_LICENSE("GPL");
