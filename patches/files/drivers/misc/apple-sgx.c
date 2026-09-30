// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple S5L8940X (A5) PowerVR SGX543MP2 -- power-on and register access.
 *
 * This is the first piece of a GPU driver and does nothing a GPU driver is
 * for: it switches the SGX on, runs the initialisation iOS runs, checks that
 * the master and every core answer, and shows their registers in debugfs
 * (apple-sgx/regs).  There is no memory management, no microkernel and no
 * command submission; those come next, on top of this.
 *
 * Power is two PMGR power states, GFX_SYS (0x3f101024) then GFX
 * (0x3f101028), taken as clocks from the gate driver in the order the DT
 * lists them.  Nothing else: the GFX clocks already run off PLL@0x18
 * (~102.6 and ~128 MHz in perf state 2), and HPERF-NRT is not needed.
 *
 * The register window is six 16 KiB banks (the DDK's sgx_mkif_km.h, and iOS
 * addresses the cores the same way):
 *
 *   bank 0       broadcast.  Written to reach every core at once; it has no
 *                read path, and a read HANGS THE BUS.
 *   bank 1       the master.
 *   bank 2 + n   core n.  Reads hang until MASTER_CLKGATECTL gives the core
 *                a clock.
 *
 * Every access goes through sgx_read()/sgx_write(), which refuse bank 0
 * reads and any core bank this SGX does not have, so neither mistake can be
 * made by accident.  Each freeze during bring-up was one or the other.
 *
 * The init sequence is iOS 8.4.1's IMGSGX543.kext SGXDriver543::initSGX
 * (kernelcache 0x80bf3918, helpers 0x80bf3700 and 0x80bf3754) up to the BIF
 * setup.  Register names come from TI's MIT/GPLv2 DDK (sgxmpdefs.h,
 * sgx544defs.h) and, for the master clock-gating registers the DDK leaves
 * out, from iOS's own register dump (0x80bf9230).
 * docs/research/p105-gpu.md is the notebook.
 */

#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>

#define SGX_BANK_SIZE			0x4000
#define SGX_BCAST			0x0000
#define SGX_MASTER			(1 * SGX_BANK_SIZE)
#define SGX_CORE(n)			((2 + (n)) * SGX_BANK_SIZE)
#define SGX_MAX_CORES			4

/* Per-core registers, at SGX_CORE(n) or, for writes, SGX_BCAST. */
#define SGX_CLKGATECTL			0x000
#define SGX_CLKGATECTL2			0x004
#define SGX_CLKGATESTATUS		0x008
#define SGX_CORE_ID			0x020
#define SGX_CORE_REVISION		0x024
#define SGX_SOFT_RESET			0x080
#define SGX_REG_310			0x310	/* iOS writes 1; not in the DDK */

/* Master registers, offsets into the whole window. */
#define SGX_MASTER_CORE			0x4000	/* enabled cores - 1 */
#define SGX_MASTER_CLKGATECTL		0x4004	/* 2 bits per core */
#define SGX_MASTER_CLKGATESTATUS	0x4008
#define SGX_MASTER_CORE_ID		0x4010
#define SGX_MASTER_CORE_REVISION	0x4014
#define SGX_MASTER_CLKGATECTL2		0x4020
#define SGX_MASTER_CLKGATESTATUS2	0x4024
#define SGX_MASTER_SOFT_RESET		0x4080
#define SGX_MASTER_BIF_CTRL		0x4c00
#define SGX_MASTER_SLC_CTRL		0x4d00
#define SGX_MASTER_SLC_CTRL_BYPASS	0x4d04

#define SGX_CORE_ID_ID(v)		((v) >> 16)
#define SGX_CORE_ID_CORES(v)		(((v) >> 8) & 0xf)
#define SGX_ID_543			0x0119

/* MASTER_SOFT_RESET: IPF, DPM, VDM, SLC, PTLA (0x4f0), BIF (0x100), and one
 * bit per core. */
#define SGX_MASTER_RESET_ALL		0x5f0

/* iOS's values.  The bypass word is BYP_CC (0x4000000) plus, on parts with
 * erratum 31195, the USE0-3 and TA requestors (0x1e40) -- the DDK's
 * FIX_HW_BRN_31195. */
#define SGX_SLC_CTRL_VAL		0x0044c000
#define SGX_SLC_BYPASS_VAL		0x04000000
#define SGX_SLC_BYPASS_BRN_31195	0x00001e40

/* A clock-gating mode, as iOS writes it into every 2-bit field: 1 keeps the
 * clock on, 2 lets the hardware gate it.  iOS uses 2 when auto clock gating
 * is enabled; bring-up wants 1. */
static unsigned int clock_mode = 1;
module_param(clock_mode, uint, 0444);
MODULE_PARM_DESC(clock_mode, "SGX clock gating: 1 = always on (default), 2 = automatic");

struct apple_sgx {
	struct device *dev;
	void __iomem *regs;
	resource_size_t size;
	struct clk_bulk_data *clks;
	int num_clks;
	unsigned int ncores;
	bool brn_31195;
	struct dentry *debugfs;
};

/* The only way to a register.  A read of bank 0 or of a core bank this SGX
 * does not have would hang the bus, so it is refused -- with a warning,
 * since it can only be a bug. */
static bool sgx_readable(struct apple_sgx *sgx, u32 off)
{
	if (off >= SGX_MASTER && off < SGX_CORE(0))
		return true;
	return off >= SGX_CORE(0) && off < SGX_CORE(sgx->ncores);
}

static u32 sgx_read(struct apple_sgx *sgx, u32 off)
{
	if (WARN_ONCE(!sgx_readable(sgx, off),
		      "apple-sgx: refused read at +0x%05x\n", off))
		return ~0u;
	return readl(sgx->regs + off);
}

static void sgx_write(struct apple_sgx *sgx, u32 off, u32 val)
{
	/* Bank 0 is write-only, which is what it is for. */
	if (WARN_ONCE(off >= SGX_CORE(sgx->ncores),
		      "apple-sgx: refused write at +0x%05x\n", off))
		return;
	writel(val, sgx->regs + off);
}

/* The same mode in every 2-bit field listed by shift. */
static u32 sgx_modes(unsigned int m, const u8 *shifts, int n)
{
	u32 v = 0;

	while (n--)
		v |= m << shifts[n];
	return v;
}

static void sgx_init(struct apple_sgx *sgx)
{
	/* CLKGATECTL: ISP, ISP2, TSP, TE, MTE, DPM, VDM, PDS, IDXFIFO, TA,
	 * BIF_CORE.  CLKGATECTL2: PBE, TCU_L2, UCACHEL2, USE0, ITR0, TEX0,
	 * USE1, ITR1, TEX1, DCU_L2, DCU1_L0L1, DCU0_L0L1 (sgx544defs.h). */
	static const u8 ctl[] = { 0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20 };
	static const u8 ctl2[] = { 0, 2, 4, 6, 8, 10, 14, 16, 18, 22, 24, 26 };
	static const u8 master2[] = { 2, 4, 6, 8 };
	unsigned int m = clock_mode, i;
	u32 cores = 0, bypass = SGX_SLC_BYPASS_VAL;

	for (i = 0; i < sgx->ncores; i++)
		cores |= m << (2 * i);
	if (sgx->brn_31195)
		bypass |= SGX_SLC_BYPASS_BRN_31195;

	/* Which cores exist, and the master's and each core's clocks.  The
	 * cores have no clock until MASTER_CLKGATECTL gives them one. */
	sgx_write(sgx, SGX_MASTER_CORE, sgx->ncores - 1);
	sgx_write(sgx, SGX_MASTER_CLKGATECTL2,
		  sgx_modes(m, master2, ARRAY_SIZE(master2)) | 1);
	sgx_write(sgx, SGX_MASTER_CLKGATECTL, cores);

	/* Reset, with the system-level cache set up while it is held. */
	sgx_write(sgx, SGX_MASTER_SOFT_RESET,
		  SGX_MASTER_RESET_ALL | GENMASK(sgx->ncores - 1, 0));
	sgx_write(sgx, SGX_MASTER_SLC_CTRL, SGX_SLC_CTRL_VAL);
	sgx_write(sgx, SGX_MASTER_SLC_CTRL_BYPASS, bypass);
	sgx_write(sgx, SGX_MASTER_SOFT_RESET, 0);
	/* The DDK waits 100 SGX clocks here: 1 us at ~100 MHz. */
	udelay(10);

	/* Every core's module clocks, through the broadcast bank.  Bit 28
	 * (SYSTEM_CLKG) goes with mode 1 only, as iOS has it. */
	sgx_write(sgx, SGX_BCAST + SGX_CLKGATECTL,
		  sgx_modes(m, ctl, ARRAY_SIZE(ctl)) |
		  (m == 1 ? BIT(28) : 0));
	sgx_write(sgx, SGX_BCAST + SGX_CLKGATECTL2,
		  sgx_modes(m, ctl2, ARRAY_SIZE(ctl2)));
	sgx_write(sgx, SGX_BCAST + SGX_REG_310, 1);
}

static const struct {
	u32 off;
	const char *name;
} sgx_master_regs[] = {
	{ SGX_MASTER_CORE,		"MASTER_CORE" },
	{ SGX_MASTER_CLKGATECTL,	"MASTER_CLKGATECTL" },
	{ SGX_MASTER_CLKGATESTATUS,	"MASTER_CLKGATESTATUS" },
	{ SGX_MASTER_CORE_ID,		"MASTER_CORE_ID" },
	{ SGX_MASTER_CORE_REVISION,	"MASTER_CORE_REVISION" },
	{ SGX_MASTER_CLKGATECTL2,	"MASTER_CLKGATECTL2" },
	{ SGX_MASTER_CLKGATESTATUS2,	"MASTER_CLKGATESTATUS2" },
	{ SGX_MASTER_SOFT_RESET,	"MASTER_SOFT_RESET" },
	{ SGX_MASTER_BIF_CTRL,		"MASTER_BIF_CTRL" },
	{ SGX_MASTER_SLC_CTRL,		"MASTER_SLC_CTRL" },
	{ SGX_MASTER_SLC_CTRL_BYPASS,	"MASTER_SLC_CTRL_BYPASS" },
}, sgx_core_regs[] = {
	{ SGX_CLKGATECTL,		"CLKGATECTL" },
	{ SGX_CLKGATECTL2,		"CLKGATECTL2" },
	{ SGX_CLKGATESTATUS,		"CLKGATESTATUS" },
	{ SGX_CORE_ID,			"CORE_ID" },
	{ SGX_CORE_REVISION,		"CORE_REVISION" },
	{ SGX_SOFT_RESET,		"SOFT_RESET" },
};

static int sgx_regs_show(struct seq_file *s, void *unused)
{
	struct apple_sgx *sgx = s->private;
	unsigned int i, n;

	for (i = 0; i < ARRAY_SIZE(sgx_master_regs); i++)
		seq_printf(s, "+0x%05x  %-24s 0x%08x\n", sgx_master_regs[i].off,
			   sgx_master_regs[i].name,
			   sgx_read(sgx, sgx_master_regs[i].off));
	for (n = 0; n < sgx->ncores; n++)
		for (i = 0; i < ARRAY_SIZE(sgx_core_regs); i++) {
			u32 off = SGX_CORE(n) + sgx_core_regs[i].off;

			seq_printf(s, "+0x%05x  core%u %-18s 0x%08x\n", off, n,
				   sgx_core_regs[i].name, sgx_read(sgx, off));
		}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(sgx_regs);

static void sgx_power_off(void *data)
{
	struct apple_sgx *sgx = data;

	clk_bulk_disable_unprepare(sgx->num_clks, sgx->clks);
}

static int apple_sgx_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct apple_sgx *sgx;
	struct resource *res;
	u32 id, rev, cid, crev;
	unsigned int n, cores;
	int ret;

	if (clock_mode != 1 && clock_mode != 2)
		return dev_err_probe(dev, -EINVAL, "clock_mode must be 1 or 2\n");

	sgx = devm_kzalloc(dev, sizeof(*sgx), GFP_KERNEL);
	if (!sgx)
		return -ENOMEM;
	sgx->dev = dev;
	sgx->brn_31195 = of_property_read_bool(dev->of_node, "apple,brn-31195");

	sgx->regs = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(sgx->regs))
		return PTR_ERR(sgx->regs);
	sgx->size = resource_size(res);
	if (sgx->size < SGX_CORE(1))
		return dev_err_probe(dev, -EINVAL, "register window too small\n");

	/* GFX_SYS, then GFX: clk_bulk enables in DT order. */
	sgx->num_clks = devm_clk_bulk_get_all(dev, &sgx->clks);
	if (sgx->num_clks < 0)
		return dev_err_probe(dev, sgx->num_clks, "no power-state clocks\n");
	ret = clk_bulk_prepare_enable(sgx->num_clks, sgx->clks);
	if (ret)
		return dev_err_probe(dev, ret, "cannot power the GPU on\n");
	ret = devm_add_action_or_reset(dev, sgx_power_off, sgx);
	if (ret)
		return ret;

	/* The master answers as soon as it has power.  ncores is still 0, so
	 * only master reads get through. */
	id = sgx_read(sgx, SGX_MASTER_CORE_ID);
	rev = sgx_read(sgx, SGX_MASTER_CORE_REVISION);
	cores = SGX_CORE_ID_CORES(id);
	if (SGX_CORE_ID_ID(id) != SGX_ID_543)
		return dev_err_probe(dev, -ENODEV,
				     "CORE_ID 0x%08x is not an SGX543 -- powered?\n", id);
	if (!cores || cores > SGX_MAX_CORES || SGX_CORE(cores) > sgx->size)
		return dev_err_probe(dev, -ENODEV,
				     "CORE_ID 0x%08x says %u cores\n", id, cores);
	sgx->ncores = cores;

	sgx_init(sgx);

	/* Now the cores have clocks and can be read.  Each reports the same
	 * ID and revision as the master, or something is wrong. */
	for (n = 0; n < sgx->ncores; n++) {
		cid = sgx_read(sgx, SGX_CORE(n) + SGX_CORE_ID);
		crev = sgx_read(sgx, SGX_CORE(n) + SGX_CORE_REVISION);
		if (cid != id || crev != rev)
			return dev_err_probe(dev, -EIO,
				"core %u: CORE_ID 0x%08x rev 0x%08x, master 0x%08x 0x%08x\n",
				n, cid, crev, id, rev);
	}

	dev_info(dev, "SGX543MP%u rev %u.%u.%u, %u core(s) up, clocks %s\n",
		 sgx->ncores, (rev >> 16) & 0xff, (rev >> 8) & 0xff, rev & 0xff,
		 sgx->ncores, clock_mode == 1 ? "on" : "auto");

	sgx->debugfs = debugfs_create_dir("apple-sgx", NULL);
	debugfs_create_file("regs", 0400, sgx->debugfs, sgx, &sgx_regs_fops);
	platform_set_drvdata(pdev, sgx);
	return 0;
}

static void apple_sgx_remove(struct platform_device *pdev)
{
	struct apple_sgx *sgx = platform_get_drvdata(pdev);

	debugfs_remove_recursive(sgx->debugfs);
}

static const struct of_device_id apple_sgx_of_match[] = {
	{ .compatible = "apple,s5l8940x-sgx" },
	{ }
};
MODULE_DEVICE_TABLE(of, apple_sgx_of_match);

static struct platform_driver apple_sgx_driver = {
	.probe = apple_sgx_probe,
	.remove = apple_sgx_remove,
	.driver = {
		.name = "apple-sgx",
		.of_match_table = apple_sgx_of_match,
	},
};
module_platform_driver(apple_sgx_driver);

MODULE_DESCRIPTION("Apple S5L8940X PowerVR SGX543MP2 power-on and register access");
MODULE_LICENSE("GPL");
