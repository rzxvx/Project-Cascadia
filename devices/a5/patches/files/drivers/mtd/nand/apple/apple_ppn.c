// SPDX-License-Identifier: GPL-2.0-only
/*
 * PPN NAND controller (FMI/FMC) on the Apple A5, PIO only.
 *
 * Written from the behavioural specification
 * devices/a5/docs/research/p105-nand-spec.md; no DMA, no sequencer.
 *
 * Credit: that the on-media format of this package is the same on the A5 as
 * on the iPhone 5, and the write path that first proved program and erase
 * possible on it, are the work of crystal-lized (iPhone 5, S5L8950X series,
 * 2026), who allows this work to build on theirs.
 */

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/unaligned.h>

#include "apple_ppn.h"
#include "sftl.h"

#define FMI0_BASE		0x31200000
#define FMI1_BASE		0x31300000
#define FMI_WINDOW		0x100000

/* FMI registers. */
#define F_CONFIG		0x00
#define F_CONTROL		0x04
#define F_STATUS		0x0c
#define F_INT_EN		0x10
#define F_DATA			0x14
#define F_DMA_STATUS		0x1c
#define F_PIO			0x34
#define F_ECC_INIT		0x80014

#define CONTROL_RX		3
#define CONTROL_TX		5
#define CONTROL_STOP		6
#define PIO_CONFIG_DATA		5
#define PIO_CONFIG_ID		0
#define DMA_READY_BITS		0x18
#define PIO_MAX_BURST		1024u
#define STATUS_DONE		BIT(1)
#define STATUS_POLL_DONE	BIT(8)
#define FMI_ALL_EVENTS		0x10f
#define FMC_ALL_EVENTS		0x31ffff

/* FMC registers. */
#define FMC_OFF			0x40000
#define C_ON			(FMC_OFF + 0x00)
#define C_IF_CTRL		(FMC_OFF + 0x08)
#define C_CE_CTRL		(FMC_OFF + 0x0c)
#define C_RW_CTRL		(FMC_OFF + 0x10)
#define C_CMD			(FMC_OFF + 0x14)
#define C_ADDR0			(FMC_OFF + 0x18)
#define C_ADDR1			(FMC_OFF + 0x1c)
#define C_ADDRNUM		(FMC_OFF + 0x20)
#define C_DATANUM		(FMC_OFF + 0x24)
#define C_INT_MASK		(FMC_OFF + 0x40)
#define C_STATUS		(FMC_OFF + 0x44)
#define C_NAND_STATUS		(FMC_OFF + 0x48)
#define C_STATUS_MASK		(FMC_OFF + 0x4c)

#define RW_CMD1			0x01
#define RW_CMD2			0x02
#define RW_ADDR			0x08
#define RW_POLL			0x50
#define ST_MATCH		0x20

#define ON_SDR			1
#define IF_CTRL_TIMING		0x23210
#define IF_CTRL_LOW_POWER	0xffff
#define IF_CTRL_POLL_CLEAR	0x100000

/* NAND opcodes. */
#define OP_RESET		0xff
#define OP_READ_ID		0x90
#define OP_GET_FEAT		0xee
#define OP_SET_FEAT		0xef
#define OP_FEAT_CONFIRM		0xe7
#define OP_PARAM		0x92
#define OP_PARAM_CONFIRM	0x97
#define OP_READ			0x0a
#define OP_READ_CONFIRM		0x37
#define OP_DATA_OUT		0x7a
#define OP_END			0x77
#define OP_PROGRAM		0x8a
#define OP_PROG_CONFIRM		0x17
#define OP_ERASE		0x6a
#define OP_ERASE_CONFIRM	0xd7
#define OP_STATUS_WAIT		0x79
#define OP_RAW_STATUS		0x7d77

#define ST_READY		0x40
#define ST_GENERAL_ERR		0x10
#define ST_FAIL			0x01

#define FEAT_FIRMWARE		0x9080
#define FEAT_BAD_BLOCK		0x84
#define FEAT_POWER_STATE	0x0180
#define POWER_STATE_ASYNC	2
#define BAD_BLOCK_BYTES		133u

#define NBUS			2
#define NCAU			2
#define BUS_CE			0		/* the one populated CE on each bus */
#define ROW_ADDR_BYTES		4		/* program and erase, PPN 1.5 and later */
#define FULL_ADDR_BYTES		8		/* read: byte count, column, row */
#define RAW_PAGE_BYTES		16448u
#define UNIT_SECTOR_BYTES	1028u
#define UNIT_SECTORS		4u
#define RAW_UNIT_DATA1		1024u
#define RAW_UNIT_META		16u
#define RAW_UNIT_DATA2		3072u

#define WAIT_PHASE_US		100000
#define WAIT_STATUS_US		2000000
#define WAIT_ERASE_US		2000000
#define WAIT_RESET_MS		50

static bool hold_probe;
module_param(hold_probe, bool, 0644);
MODULE_PARM_DESC(hold_probe, "Defer probing the hardware until cleared and bound");

static bool allow_write;
module_param(allow_write, bool, 0444);
MODULE_PARM_DESC(allow_write, "Permit program and erase operations");

#ifndef SFTL_NO_WRITE
/* Test only: make the media look as if a program or an erase failed. */
static int inject_program = -1;
module_param(inject_program, int, 0644);
MODULE_PARM_DESC(inject_program, "Test only: the Nth page program from now fails (-1 off)");
static int inject_mode = 1;
module_param(inject_mode, int, 0644);
MODULE_PARM_DESC(inject_mode, "Test only: 1 = the failing page is not programmed, 2 = programmed then reported failed");
static int inject_erase = -1;
module_param(inject_erase, int, 0644);
MODULE_PARM_DESC(inject_erase, "Test only: the Nth block erase from now fails (-1 off)");
#endif

struct ppn_bus {
	void __iomem *fmi;
	u32 if_ctrl;			/* what a software reset writes back */
};

struct apple_ppn {
	struct device *dev;
	struct ppn_bus bus[NBUS];
	struct mutex lock;
	bool gone;
};

static struct apple_ppn *ppn_singleton;
static DEFINE_MUTEX(ppn_global_lock);

static inline u32 rd(const struct ppn_bus *b, unsigned long reg)
{
	return readl(b->fmi + reg);
}

static inline void wr(const struct ppn_bus *b, unsigned long reg, u32 v)
{
	writel(v, b->fmi + reg);
}

static int wait_phase(const struct ppn_bus *b, u32 bits)
{
	u32 v;
	int ret;

	ret = readl_poll_timeout(b->fmi + C_STATUS, v,
				 (v & bits) == bits, 1, WAIT_PHASE_US);
	if (ret)
		return ret;
	wr(b, C_STATUS, bits);
	return 0;
}

static void clear_events(const struct ppn_bus *b)
{
	wr(b, C_INT_MASK, 0);
	wr(b, F_INT_EN, 0);
	wr(b, C_STATUS, FMC_ALL_EVENTS);
	wr(b, F_STATUS, FMI_ALL_EVENTS);
}

/* Software reset of a bus: no NAND reset. */
static void bus_soft_reset(const struct ppn_bus *b)
{
	wr(b, F_CONTROL, CONTROL_STOP);
	wr(b, C_ON, ON_SDR);
	wr(b, C_IF_CTRL, b->if_ctrl);
}

static void ce_select(const struct ppn_bus *b)
{
	wr(b, C_CE_CTRL, BIT(BUS_CE));
}

static void ce_deselect(const struct ppn_bus *b)
{
	wr(b, C_CE_CTRL, 0);
}

static int cmd_only(const struct ppn_bus *b, u32 op)
{
	wr(b, C_CMD, op);
	wr(b, C_RW_CTRL, RW_CMD1);
	return wait_phase(b, RW_CMD1);
}

static void set_addr(const struct ppn_bus *b, u32 addr, unsigned int nbytes)
{
	wr(b, C_ADDR0, addr);
	wr(b, C_ADDRNUM, (nbytes + 7) & 7);
}

static int cmd_addr_cmd(const struct ppn_bus *b, u32 op1, u32 op2, u32 addr,
			unsigned int nbytes)
{
	wr(b, C_CMD, op1 | op2 << 8);
	set_addr(b, addr, nbytes);
	wr(b, C_RW_CTRL, RW_CMD1 | RW_CMD2 | RW_ADDR);
	return wait_phase(b, RW_CMD1 | RW_CMD2 | RW_ADDR);
}

static int cmd_addr(const struct ppn_bus *b, u32 op, u32 addr, unsigned int nbytes)
{
	wr(b, C_CMD, op);
	set_addr(b, addr, nbytes);
	wr(b, C_RW_CTRL, RW_CMD1 | RW_ADDR);
	return wait_phase(b, RW_CMD1 | RW_ADDR);
}

/* Read command with the full address: byte count, byte column, row. */
static int cmd_read_full(const struct ppn_bus *b, u32 row)
{
	wr(b, C_CMD, OP_READ | OP_READ_CONFIRM << 8);
	wr(b, C_ADDR0, RAW_PAGE_BYTES);
	wr(b, C_ADDR1, row);
	wr(b, C_ADDRNUM, (FULL_ADDR_BYTES + 7) & 7);
	wr(b, C_RW_CTRL, RW_CMD1 | RW_CMD2 | RW_ADDR);
	return wait_phase(b, RW_CMD1 | RW_CMD2 | RW_ADDR);
}

/*
 * Arm a status poll on the bus.  Separated from waiting for it so the program
 * path can have one poll running per bus.
 */
static void poll_start(const struct ppn_bus *b)
{
	clear_events(b);
	wr(b, C_IF_CTRL, b->if_ctrl & ~IF_CTRL_POLL_CLEAR);
	wr(b, C_STATUS_MASK, 0x4040);
	wr(b, C_DATANUM, 0);
	wr(b, C_INT_MASK, ST_MATCH);
	wr(b, C_RW_CTRL, RW_POLL);
}

static bool poll_ready(const struct ppn_bus *b)
{
	return rd(b, F_STATUS) & STATUS_POLL_DONE;
}

static u8 poll_finish(const struct ppn_bus *b)
{
	u8 status = (u8)rd(b, C_NAND_STATUS);

	wr(b, F_STATUS, STATUS_POLL_DONE);
	wr(b, C_RW_CTRL, 0);
	wr(b, C_IF_CTRL, b->if_ctrl);
	return status;
}

static void poll_abort(const struct ppn_bus *b)
{
	wr(b, C_RW_CTRL, 0);
	wr(b, C_IF_CTRL, b->if_ctrl);
}

static int poll_wait(const struct ppn_bus *b, unsigned int timeout_us, u8 *status)
{
	ktime_t end = ktime_add_us(ktime_get(), timeout_us);

	poll_start(b);
	while (!poll_ready(b)) {
		if (ktime_after(ktime_get(), end)) {
			poll_abort(b);
			return -ETIMEDOUT;
		}
		cpu_relax();
	}
	*status = poll_finish(b);
	return 0;
}

/* Operation status: 0x77 0x7d, then poll. */
static int op_status(const struct ppn_bus *b, u8 *status)
{
	int ret;

	wr(b, C_CMD, OP_RAW_STATUS);
	wr(b, C_RW_CTRL, RW_CMD1 | RW_CMD2);
	ret = wait_phase(b, RW_CMD1 | RW_CMD2);
	if (ret)
		return ret;
	return poll_wait(b, WAIT_STATUS_US, status);
}

static int wait_fifo(const struct ppn_bus *b)
{
	u32 v;

	return readl_poll_timeout(b->fmi + F_DMA_STATUS, v, v & DMA_READY_BITS,
				  1, WAIT_PHASE_US);
}

/* One PIO burst, n bytes (multiple of 4, at most PIO_MAX_BURST). */
static int rx_burst(const struct ppn_bus *b, u8 *buf, unsigned int n)
{
	int ret;

	wr(b, F_CONFIG, PIO_CONFIG_DATA);
	wr(b, F_PIO, ((n << 8) & 0x7fc00) | 1);
	wr(b, F_CONTROL, CONTROL_RX);
	ret = wait_fifo(b);
	if (ret)
		return ret;
	ioread32_rep(b->fmi + F_DATA, buf, n / 4);
	return 0;
}

static int wait_tx_done(const struct ppn_bus *b)
{
	u32 v;
	int ret;

	ret = readl_poll_timeout(b->fmi + F_STATUS, v, v & STATUS_DONE, 1, WAIT_PHASE_US);
	if (!ret)
		wr(b, F_STATUS, STATUS_DONE);
	return ret;
}

static int tx_burst(const struct ppn_bus *b, const u8 *buf, unsigned int n)
{
	int ret;

	wr(b, F_CONFIG, PIO_CONFIG_DATA);
	wr(b, F_PIO, ((n << 8) & 0x7fc00) | 1);
	wr(b, F_CONTROL, CONTROL_TX);
	ret = wait_fifo(b);
	if (ret)
		return ret;
	iowrite32_rep(b->fmi + F_DATA, buf, n / 4);
	return wait_tx_done(b);
}

/*
 * One 4112-byte unit, one start, four sectors of 1028 bytes.  The stream is
 * 1024 data, 16 metadata, 3072 data, so the metadata straddles sectors 0 and 1.
 */
static int rx_unit(const struct ppn_bus *b, u8 *data, u8 *meta)
{
	void __iomem *fifo = b->fmi + F_DATA;
	int ret;

	clear_events(b);
	wr(b, F_CONFIG, PIO_CONFIG_DATA);
	wr(b, F_PIO, (UNIT_SECTOR_BYTES << 8) | UNIT_SECTORS);
	wr(b, F_CONTROL, CONTROL_RX);

	ret = wait_fifo(b);
	if (ret)
		return ret;
	ioread32_rep(fifo, data, 256);
	ioread32_rep(fifo, meta, 1);
	ret = wait_fifo(b);
	if (ret)
		return ret;
	ioread32_rep(fifo, meta + 4, 3);
	ioread32_rep(fifo, data + 1024, 254);
	ret = wait_fifo(b);
	if (ret)
		return ret;
	ioread32_rep(fifo, data + 2040, 257);
	ret = wait_fifo(b);
	if (ret)
		return ret;
	ioread32_rep(fifo, data + 3068, 257);
	return 0;
}

#ifndef SFTL_NO_WRITE
/*
 * Transmit one 4112-byte unit the way a unit is received: one start, four
 * sectors of 1028 bytes, the 16 metadata bytes 1024 bytes into the stream.
 * The end of the unit is the done bit, as on the set-feature transmit.
 */
static int tx_unit(const struct ppn_bus *b, const u8 *data, const u8 *meta)
{
	void __iomem *fifo = b->fmi + F_DATA;
	int ret;

	clear_events(b);
	wr(b, F_CONFIG, PIO_CONFIG_DATA);
	wr(b, F_PIO, (UNIT_SECTOR_BYTES << 8) | UNIT_SECTORS);
	wr(b, F_CONTROL, CONTROL_TX);

	ret = wait_fifo(b);
	if (ret)
		return ret;
	iowrite32_rep(fifo, data, 256);
	iowrite32_rep(fifo, meta, 1);
	ret = wait_fifo(b);
	if (ret)
		return ret;
	iowrite32_rep(fifo, meta + 4, 3);
	iowrite32_rep(fifo, data + 1024, 254);
	ret = wait_fifo(b);
	if (ret)
		return ret;
	iowrite32_rep(fifo, data + 2040, 257);
	ret = wait_fifo(b);
	if (ret)
		return ret;
	iowrite32_rep(fifo, data + 3068, 257);
	return wait_tx_done(b);
}

static unsigned int tx_shape;
module_param(tx_shape, uint, 0644);
MODULE_PARM_DESC(tx_shape, "Page transmit: 0 = one start per 4112-byte unit, 1 = bursts of up to 1024 bytes");

/* The burst form: one start per piece of at most 1024 bytes. */
static int tx_unit_bursts(const struct ppn_bus *b, const u8 *data, const u8 *meta)
{
	unsigned int off;
	int ret;

	ret = tx_burst(b, data, RAW_UNIT_DATA1);
	if (ret)
		return ret;
	ret = tx_burst(b, meta, RAW_UNIT_META);
	for (off = 0; !ret && off < RAW_UNIT_DATA2; off += PIO_MAX_BURST)
		ret = tx_burst(b, data + RAW_UNIT_DATA1 + off,
			       min(RAW_UNIT_DATA2 - off, PIO_MAX_BURST));
	return ret;
}

static int tx_page(const struct ppn_bus *b, const u8 *data, const u8 *meta)
{
	unsigned int u;
	int ret = 0;

	for (u = 0; !ret && u < SFTL_LBAS_PER_PAGE; u++) {
		const u8 *d = data + u * SFTL_LBA_SIZE, *m = meta + u * SFTL_META_LBA;

		ret = tx_shape ? tx_unit_bursts(b, d, m) : tx_unit(b, d, m);
	}
	return ret;
}
#endif

static u32 make_row(unsigned int cau, unsigned int block, unsigned int page, bool slc)
{
	return page | block << 8 | cau << 19 | (slc ? BIT(23) : 0);
}

static int nand_reset(const struct ppn_bus *b)
{
	int ret;

	ce_select(b);
	udelay(10);
	ret = cmd_only(b, OP_RESET);
	ce_deselect(b);
	return ret;
}

static int read_id(const struct ppn_bus *b, u8 *id)
{
	u32 words[2], v;
	int ret;

	ce_select(b);
	clear_events(b);
	wr(b, C_CMD, OP_READ_ID);
	set_addr(b, 0, 1);
	wr(b, C_RW_CTRL, RW_CMD1 | RW_ADDR);
	ret = wait_phase(b, RW_CMD1 | RW_ADDR);
	if (ret)
		goto out;
	wr(b, F_CONFIG, PIO_CONFIG_ID);
	wr(b, F_PIO, (8 << 8) | 1);
	wr(b, F_CONTROL, CONTROL_RX);
	ret = readl_poll_timeout(b->fmi + F_STATUS, v, v & STATUS_DONE, 1, WAIT_PHASE_US);
	if (ret)
		goto out;
	words[0] = rd(b, F_DATA);
	words[1] = rd(b, F_DATA);
	memcpy(id, words, 8);
out:
	ce_deselect(b);
	bus_soft_reset(b);
	return ret;
}

/*
 * Get feature: data in bursts of up to 1024 bytes; a tail shorter than a word
 * goes as one last 4-byte burst.
 */
static int get_feature(const struct ppn_bus *b, u32 feature, u8 *buf, unsigned int n)
{
	unsigned int done = 0;
	u8 st;
	int ret;

	ce_select(b);
	clear_events(b);
	ret = cmd_addr_cmd(b, OP_GET_FEAT, OP_FEAT_CONFIRM, feature, 2);
	if (!ret)
		ret = op_status(b, &st);
	if (!ret && st != ST_READY)
		ret = -EIO;
	if (!ret)
		ret = cmd_only(b, OP_DATA_OUT);
	while (!ret && done < n) {
		unsigned int left = n - done;

		if (left >= 4) {
			unsigned int chunk = min(left, PIO_MAX_BURST) & ~3u;

			ret = rx_burst(b, buf + done, chunk);
			done += chunk;
		} else {
			u8 tail[4];

			ret = rx_burst(b, tail, 4);
			memcpy(buf + done, tail, left);
			done = n;
		}
	}
	wr(b, F_CONTROL, CONTROL_STOP);
	if (!ret)
		ret = cmd_only(b, OP_END);
	ce_deselect(b);
	return ret;
}

/* Set feature: one 4-byte word out. */
static int set_feature(const struct ppn_bus *b, u32 feature, u32 value)
{
	__le32 v = cpu_to_le32(value);
	u8 st;
	int ret;

	ce_select(b);
	wr(b, F_CONTROL, CONTROL_STOP);
	ret = cmd_addr(b, OP_SET_FEAT, feature, 2);
	if (!ret) {
		clear_events(b);
		ret = tx_burst(b, (const u8 *)&v, 4);
	}
	if (!ret)
		ret = cmd_only(b, OP_FEAT_CONFIRM);
	if (!ret)
		ret = op_status(b, &st);
	if (!ret && st != ST_READY)
		ret = -EIO;
	if (!ret)
		ret = cmd_only(b, OP_END);
	wr(b, F_CONTROL, CONTROL_STOP);
	ce_deselect(b);
	bus_soft_reset(b);
	return ret;
}

static int read_params(const struct ppn_bus *b, u8 *buf)
{
	u8 st;
	int ret;

	ce_select(b);
	clear_events(b);
	ret = cmd_addr_cmd(b, OP_PARAM, OP_PARAM_CONFIRM, 0, 1);
	if (!ret)
		ret = op_status(b, &st);
	if (!ret && st != ST_READY)
		ret = -EIO;
	if (!ret)
		ret = cmd_only(b, OP_DATA_OUT);
	if (!ret)
		ret = rx_burst(b, buf, 512);
	if (!ret)
		ret = cmd_only(b, OP_END);
	ce_deselect(b);
	return ret;
}

static int check_params(struct apple_ppn *p, const u8 *buf)
{
	if (memcmp(buf, "PPN Device Info", 15) ||
	    get_unaligned_le32(buf + 0x10) != NCAU ||
	    get_unaligned_le32(buf + 0x18) != SFTL_BLOCKS ||
	    get_unaligned_le32(buf + 0x20) != SFTL_MLC_PAGES ||
	    get_unaligned_le32(buf + 0x24) != SFTL_SLC_PAGES ||
	    get_unaligned_le32(buf + 0x34) != SFTL_PAGE_SIZE + SFTL_META_PAGE) {
		dev_err(p->dev, "unexpected device parameters: %*ph\n", 64, buf);
		return -ENODEV;
	}
	return 0;
}

/*
 * Full bring-up of both buses, used at probe and to recover from a general
 * error.  The order is the one that works on this package; only the populated
 * chip enable of each bus is touched.
 */
static int ppn_bringup(struct apple_ppn *p)
{
	u8 *buf;
	u8 fw[16], bbt[BAD_BLOCK_BYTES], id[8];
	unsigned int n, c;
	int ret;

	buf = kmalloc(512, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	for (n = 0; n < NBUS; n++) {
		struct ppn_bus *b = &p->bus[n];

		b->if_ctrl = 0;
		bus_soft_reset(b);
		wr(b, F_ECC_INIT, 0);
	}

	for (n = 0; n < NBUS; n++) {
		struct ppn_bus *b = &p->bus[n];

		ret = nand_reset(b);
		if (ret) {
			dev_err(p->dev, "bus %u: reset command timed out\n", n);
			goto out;
		}
		msleep(WAIT_RESET_MS);
		b->if_ctrl = IF_CTRL_LOW_POWER;
		wr(b, C_IF_CTRL, b->if_ctrl);

		ret = read_id(b, id);
		if (ret || memcmp(id, "PPN", 3) || id[3] != 1 || id[4] != 5 || id[5] != 5) {
			dev_err(p->dev, "bus %u: id %*ph (ret %d)\n", n, 8, id, ret);
			ret = ret ? ret : -ENODEV;
			goto out;
		}
	}

	ret = read_params(&p->bus[0], buf);
	if (!ret)
		ret = check_params(p, buf);
	if (ret)
		goto out;

	for (n = 0; n < NBUS; n++) {
		ret = set_feature(&p->bus[n], FEAT_POWER_STATE, POWER_STATE_ASYNC);
		if (ret) {
			dev_err(p->dev, "bus %u: power state: %d\n", n, ret);
			goto out;
		}
	}

	for (n = 0; n < NBUS; n++) {
		p->bus[n].if_ctrl = IF_CTRL_TIMING;
		wr(&p->bus[n], C_IF_CTRL, IF_CTRL_TIMING);
	}

	ret = get_feature(&p->bus[0], FEAT_FIRMWARE, fw, sizeof(fw));
	if (ret)
		goto out;
	if (memcmp(fw, "040472P_HYNIX_B_", 16)) {
		dev_err(p->dev, "firmware %*ph not tested, refusing\n", 16, fw);
		ret = -ENODEV;
		goto out;
	}

	/* The bad-block tables are not used, but are always read here. */
	for (n = 0; n < NBUS; n++) {
		for (c = 0; c < NCAU; c++) {
			ret = get_feature(&p->bus[n], FEAT_BAD_BLOCK | c << 8, bbt, sizeof(bbt));
			if (ret) {
				dev_err(p->dev, "bus %u cau %u: bad-block table: %d\n", n, c, ret);
				goto out;
			}
		}
	}
out:
	kfree(buf);
	return ret;
}

static int read_page_once(struct apple_ppn *p, unsigned int bank, unsigned int block,
			  unsigned int page, bool slc, u8 *data, u8 *meta, u8 *status)
{
	const struct ppn_bus *b = &p->bus[bank & 1];
	u8 st;
	int ret;

	bus_soft_reset(b);
	ce_select(b);
	clear_events(b);
	ret = cmd_read_full(b, make_row(bank >> 1, block, page, slc));
	if (!ret)
		ret = op_status(b, &st);
	if (ret)
		goto out;
	*status = st;
	/* The package has committed to the data; a general error ends it here. */
	if (!(st & ST_GENERAL_ERR)) {
		unsigned int u;

		ret = cmd_only(b, OP_DATA_OUT);
		for (u = 0; !ret && u < SFTL_LBAS_PER_PAGE; u++)
			ret = rx_unit(b, data + u * SFTL_LBA_SIZE, meta + u * SFTL_META_LBA);
		if (!ret)
			ret = cmd_only(b, OP_END);
	}
out:
	ce_deselect(b);
	bus_soft_reset(b);
	return ret;
}

int apple_ppn_read_page(struct apple_ppn *p, unsigned int bank, unsigned int block,
			unsigned int page, bool slc, u8 *data, u8 *meta, u8 *status)
{
	int ret, attempt;

	mutex_lock(&p->lock);
	if (p->gone) {
		ret = -ENODEV;
		goto out;
	}
	for (attempt = 0; attempt < 2; attempt++) {
		*status = 0;
		ret = read_page_once(p, bank, block, page, slc, data, meta, status);
		if (!ret && !(*status & ST_GENERAL_ERR))
			break;
		dev_warn(p->dev, "read bank %u block %u page %u slc %d: ret %d status %#x\n",
			 bank, block, page, slc, ret, *status);
		if (attempt == 0 && ppn_bringup(p)) {
			ret = -EIO;
			break;
		}
	}
	if (!ret && (*status & ST_GENERAL_ERR))
		ret = -EIO;
out:
	mutex_unlock(&p->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(apple_ppn_read_page);

#ifdef SFTL_NO_WRITE
/* The read-only build carries no program or erase sequence at all. */
int apple_ppn_erase(struct apple_ppn *p, unsigned int bank, unsigned int block)
{
	return -EPERM;
}
EXPORT_SYMBOL_GPL(apple_ppn_erase);

int apple_ppn_program(struct apple_ppn *p, const struct sftl_prog *pages, unsigned int n)
{
	return -EPERM;
}
EXPORT_SYMBOL_GPL(apple_ppn_program);

#else
int apple_ppn_erase(struct apple_ppn *p, unsigned int bank, unsigned int block)
{
	const struct ppn_bus *b = &p->bus[bank & 1];
	u8 st;
	int ret;

	if (!allow_write)
		return -EPERM;
	if (inject_erase >= 0 && inject_erase-- == 0) {
		dev_warn(p->dev, "erase bank %u block %u: injected failure\n", bank, block);
		return -EIO;
	}
	mutex_lock(&p->lock);
	if (p->gone) {
		ret = -ENODEV;
		goto out;
	}
	bus_soft_reset(b);
	ce_select(b);
	clear_events(b);
	ret = cmd_addr_cmd(b, OP_ERASE, OP_ERASE_CONFIRM,
			   make_row(bank >> 1, block, 0, false), ROW_ADDR_BYTES);
	if (!ret)
		ret = cmd_only(b, OP_STATUS_WAIT);
	if (!ret)
		ret = poll_wait(b, WAIT_ERASE_US, &st);
	if (!ret)
		ret = op_status(b, &st);
	if (!ret)
		ret = cmd_only(b, OP_END);
	ce_deselect(b);
	bus_soft_reset(b);
	if (!ret && st != ST_READY) {
		dev_err(p->dev, "erase bank %u block %u status %#x\n", bank, block, st);
		ret = -EIO;
	}
out:
	mutex_unlock(&p->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(apple_ppn_erase);

/* Issue a program on the bus up to its confirm, then arm the status poll. */
static int program_start(const struct ppn_bus *b, const struct sftl_prog *pg)
{
	int ret;

	bus_soft_reset(b);
	ce_select(b);
	clear_events(b);
	ret = cmd_addr(b, OP_PROGRAM, make_row(pg->bank >> 1, pg->block, pg->page, pg->slc),
		       ROW_ADDR_BYTES);
	if (ret)
		return ret;
	/* As on the set-feature transmit, the confirm follows with no stop. */
	ret = tx_page(b, pg->data, pg->meta);
	if (!ret)
		ret = cmd_only(b, OP_PROG_CONFIRM);
	if (!ret)
		ret = cmd_only(b, OP_STATUS_WAIT);
	if (ret)
		return ret;
	poll_start(b);
	return 0;
}

/*
 * Program a batch.  Pages are programmed one per CE at a time; the two buses
 * overlap, which is what gives the write speed.
 */
int apple_ppn_program(struct apple_ppn *p, const struct sftl_prog *pages, unsigned int n)
{
	unsigned int i, next[NBUS] = { 0, 0 }, count[NBUS] = { 0, 0 };
	const struct sftl_prog *busy[NBUS] = { NULL, NULL };
	bool inject_fail[NBUS] = { false, false };
	ktime_t started[NBUS] = { 0, 0 };
	int ret = 0;

	if (!allow_write)
		return -EPERM;
	for (i = 0; i < n; i++)
		count[pages[i].bank & 1]++;

	mutex_lock(&p->lock);
	if (p->gone) {
		ret = -ENODEV;
		goto out;
	}
	while (!ret && (next[0] < count[0] || next[1] < count[1] || busy[0] || busy[1])) {
		for (i = 0; i < NBUS; i++) {
			const struct ppn_bus *b = &p->bus[i];

			if (busy[i]) {
				u8 st;

				if (!poll_ready(b)) {
					if (ktime_us_delta(ktime_get(), started[i]) >
					    WAIT_STATUS_US) {
						dev_err(p->dev, "program timeout bus %u\n", i);
						ret = -ETIMEDOUT;
					}
					continue;
				}
				st = poll_finish(b);
				ce_deselect(b);
				bus_soft_reset(b);
				if (inject_fail[i]) {
					dev_warn(p->dev, "program bank %u block %u page %u: injected failure (programmed)\n",
						 busy[i]->bank, busy[i]->block, busy[i]->page);
					st |= ST_FAIL;
					inject_fail[i] = false;
				}
				if (st & (ST_GENERAL_ERR | ST_FAIL)) {
					dev_err(p->dev, "program bank %u block %u page %u: status %#x\n",
						busy[i]->bank, busy[i]->block, busy[i]->page, st);
					ret = -EIO;
				}
				busy[i] = NULL;
			}
			if (!ret && !busy[i] && next[i] < count[i]) {
				/* This bus's next page, in batch order. */
				unsigned int k, seen = 0;

				for (k = 0; k < n; k++) {
					if ((pages[k].bank & 1) != i)
						continue;
					if (seen++ == next[i])
						break;
				}
				if (inject_program >= 0 && inject_program-- == 0) {
					if (inject_mode != 2) {
						dev_warn(p->dev, "program bank %u block %u page %u: injected failure (not programmed)\n",
							 pages[k].bank, pages[k].block, pages[k].page);
						ret = -EIO;
						break;
					}
					inject_fail[i] = true;
				}
				ret = program_start(b, &pages[k]);
				if (ret) {
					dev_err(p->dev, "program start bus %u: %d\n", i, ret);
					break;
				}
				busy[i] = &pages[k];
				started[i] = ktime_get();
				next[i]++;
			}
		}
		cpu_relax();
	}
	/* Leave nothing armed behind us. */
	for (i = 0; i < NBUS; i++) {
		if (busy[i]) {
			poll_abort(&p->bus[i]);
			ce_deselect(&p->bus[i]);
			bus_soft_reset(&p->bus[i]);
		}
	}
	if (ret == -EIO) {
		int r = ppn_bringup(p);

		if (r)
			dev_err(p->dev, "recovery failed: %d\n", r);
	}
out:
	mutex_unlock(&p->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(apple_ppn_program);
#endif

bool apple_ppn_write_allowed(struct apple_ppn *p)
{
	return allow_write;
}
EXPORT_SYMBOL_GPL(apple_ppn_write_allowed);

struct apple_ppn *apple_ppn_get(void)
{
	struct apple_ppn *p;

	mutex_lock(&ppn_global_lock);
	p = ppn_singleton;
	if (p && !try_module_get(THIS_MODULE))
		p = NULL;
	if (p)
		get_device(p->dev);
	mutex_unlock(&ppn_global_lock);
	return p;
}
EXPORT_SYMBOL_GPL(apple_ppn_get);

void apple_ppn_put(struct apple_ppn *p)
{
	put_device(p->dev);
	module_put(THIS_MODULE);
}
EXPORT_SYMBOL_GPL(apple_ppn_put);

static int apple_ppn_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct apple_ppn *p;
	int ret;

	if (hold_probe)
		return -ENXIO;

	p = devm_kzalloc(dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	p->dev = dev;
	mutex_init(&p->lock);
	p->bus[0].fmi = devm_ioremap(dev, FMI0_BASE, FMI_WINDOW);
	p->bus[1].fmi = devm_ioremap(dev, FMI1_BASE, FMI_WINDOW);
	if (!p->bus[0].fmi || !p->bus[1].fmi)
		return -ENOMEM;

	ret = ppn_bringup(p);
	if (ret)
		return ret;

	mutex_lock(&ppn_global_lock);
	ppn_singleton = p;
	mutex_unlock(&ppn_global_lock);
	platform_set_drvdata(pdev, p);
	dev_info(dev, "PPN controller ready (PIO%s)\n",
		 allow_write ? ", program/erase enabled" : ", read-only");
	return 0;
}

static void apple_ppn_remove(struct platform_device *pdev)
{
	struct apple_ppn *p = platform_get_drvdata(pdev);

	mutex_lock(&ppn_global_lock);
	ppn_singleton = NULL;
	mutex_unlock(&ppn_global_lock);
	mutex_lock(&p->lock);
	p->gone = true;
	mutex_unlock(&p->lock);
}

static const struct of_device_id apple_ppn_of_match[] = {
	{ .compatible = "apple,s5l8942-ppn" },
	{ .compatible = "apple,s5l8940x-ppn" },
	{ }
};
MODULE_DEVICE_TABLE(of, apple_ppn_of_match);

static struct platform_driver apple_ppn_driver = {
	.probe = apple_ppn_probe,
	.remove_new = apple_ppn_remove,
	.driver = {
		.name = "apple-ppn",
		.of_match_table = apple_ppn_of_match,
	},
};
module_platform_driver(apple_ppn_driver);

MODULE_DESCRIPTION("Apple A5 PPN NAND controller (PIO)");
MODULE_AUTHOR("Project Cascadia; hardware findings by crystal-lized");
MODULE_LICENSE("GPL");
