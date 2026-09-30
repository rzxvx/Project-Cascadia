// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple S5L8940X (A5) PowerVR SGX543MP2 -- power-on, register access, and
 * the first start of the microkernel.
 *
 * This is the first piece of a GPU driver.  At probe it switches the SGX on,
 * runs the initialisation iOS runs, checks that the master and every core
 * answer, and shows their registers in debugfs (apple-sgx/regs).  Writing to
 * apple-sgx/boot then builds the GPU's page tables and buffers, loads the
 * microkernel the way iOS does, kicks it, and waits for it to answer.  There
 * is no command submission yet.
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
 * Everything here is iOS 8.4.1's IMGSGX543.kext, class SGXDriver543:
 * initSGX (kernelcache 0x80bf3918) with its clock helpers (0x80bf3700,
 * 0x80bf3754), the microkernel loader and setup (0x80bf9754, 0x80bfa080) and
 * the state it fills in (0x80bfa884).  Buffers are named after the offset in
 * SGXDriver543 that iOS keeps them at, so each line can be checked against
 * the disassembly.  Register names come from TI's MIT/GPLv2 DDK (sgxmpdefs.h,
 * sgx544defs.h, sgxmmu.h) and, where the DDK has none, from iOS's register
 * dump (0x80bf9230).  docs/research/p105-gpu.md is the notebook.
 *
 * The microkernel is Apple's and Imagination's.  It is not in this tree: the
 * build takes it out of the user's own IPSW (scripts/extract-sgx-firmware.py)
 * into /lib/firmware/apple/sgx543.fw, and the offsets below are for that
 * exact 12H321 build, which the script checks by hash.
 */

#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/string.h>

#define SGX_BANK_SIZE			0x4000
#define SGX_BCAST			0x0000
#define SGX_MASTER			(1 * SGX_BANK_SIZE)
#define SGX_CORE(n)			((2 + (n)) * SGX_BANK_SIZE)
#define SGX_MAX_CORES			4

/* Per-core registers, at SGX_CORE(n) or, for writes, SGX_BCAST. */
#define SGX_CLKGATECTL			0x000
#define SGX_CLKGATECTL2			0x004
#define SGX_CLKGATESTATUS		0x008
#define SGX_POWER			0x01c
#define SGX_CORE_ID			0x020
#define SGX_CORE_REVISION		0x024
#define SGX_SOFT_RESET			0x080
#define SGX_EVENT_HOST_ENABLE2		0x110
#define SGX_EVENT_HOST_CLEAR2		0x114
#define SGX_EVENT_STATUS2		0x118
#define SGX_EVENT_STATUS		0x12c
#define SGX_EVENT_HOST_ENABLE		0x130
#define SGX_EVENT_HOST_CLEAR		0x134
#define SGX_REG_13C			0x13c
#define SGX_REG_140			0x140
#define SGX_REG_310			0x310	/* iOS writes 1; not in the DDK */
#define SGX_USE_CODE_BASE(i)		(0xa0c + 4 * (i))
#define SGX_EVENT_PDS_EXEC		0xa68	/* names guessed: exec, data, info */
#define SGX_EVENT_PDS_DATA		0xa6c
#define SGX_EVENT_PDS_INFO		0xa70
#define SGX_EVENT_KICKER		0xac4
#define SGX_EVENT_KICK			0xac8
#define SGX_EVENT_TIMER			0xacc
#define SGX_BOOT_ENTRY			0xba0	/* entry into the boot program / 8 */
#define SGX_BIF_CTRL			0xc00
#define SGX_BIF_INT_STAT		0xc04
#define SGX_BIF_FAULT			0xc08
#define SGX_BIF_TILE1			0xc10
#define SGX_BIF_TILE2			0xc14
#define SGX_BIF_CTRL_INVAL		0xc34
#define SGX_BIF_DIR_LIST_BASE(i)	((i) ? 0xc34 + 4 * (i) : 0xc84)
#define SGX_BIF_BANK_SET		0xc74
#define SGX_BIF_BANK0			0xc78
#define SGX_BIF_BANK1			0xc7c
#define SGX_BIF_MEM_REQ_STAT		0xca8
#define SGX_BIF_MMU_CTRL		0xcd0

/* Master registers, offsets into the whole window. */
#define SGX_MASTER_CORE			0x4000	/* enabled cores - 1 */
#define SGX_MASTER_CLKGATECTL		0x4004	/* 2 bits per core */
#define SGX_MASTER_CLKGATESTATUS	0x4008
#define SGX_MASTER_CORE_ID		0x4010
#define SGX_MASTER_CORE_REVISION	0x4014
#define SGX_MASTER_CLKGATECTL2		0x4020
#define SGX_MASTER_CLKGATESTATUS2	0x4024
#define SGX_MASTER_SOFT_RESET		0x4080
#define SGX_MASTER_EVENT_STATUS2	0x4118
#define SGX_MASTER_EVENT_STATUS		0x412c
#define SGX_MASTER_REG_4144		0x4144
#define SGX_MASTER_REG_414C		0x414c
#define SGX_MASTER_REG_4808		0x4808
#define SGX_MASTER_REG_4A58		0x4a58
#define SGX_MASTER_BIF_CTRL		0x4c00
#define SGX_MASTER_BIF_INT_STAT		0x4c04
#define SGX_MASTER_BIF_FAULT		0x4c08
#define SGX_MASTER_BIF_MEM_REQ_STAT	0x4ca8
#define SGX_MASTER_BIF_MMU_CTRL		0x4cd0
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

/* ---- the GPU's MMU (sgxmmu.h; iOS's PTE writer is 0x80bf4dc4) ---------- */

#define SGX_PAGE_SIZE			0x1000
#define SGX_PD_ENTRIES			1024
#define SGX_PDE_VALID			BIT(0)
#define SGX_PTE_VALID			BIT(0)

/* Where the buffers go in the GPU's address space.  The microkernel carries
 * no address of its own (every one is patched in or handed over in a
 * register), so the layout is ours; 0x80000000 is where iOS's "GART" starts.
 * Everything stays within 8 MiB of the code, because some addresses are
 * given as offsets from it in 20-bit fields. */
#define SGX_VA_BASE			0x80000000u

/* ---- the firmware file (scripts/extract-sgx-firmware.py) --------------- */

#define SGX_FW_NAME			"apple/sgx543.fw"
#define SGX_FW_DATA_SIZE		0x16094		/* kext __DATA,__data */
#define SGX_FW_CONST_SIZE		0x730		/* kext __TEXT,__const */

struct sgx_fw_header {
	char magic[8];			/* "SGX543FW" */
	__le32 version;			/* 1 */
	__le32 data_off, data_size;
	__le32 const_off, const_size;
	__le32 data_va, const_va;
	char build[16];			/* "12H321" */
};

/* In __data: the microkernel, its boot program, and tables. */
#define UK_CODE_OFF			0x00060
#define UK_CODE_SIZE			0x11f88
#define UK_CODE2_OFF			0x11fe8		/* more USSE code */
#define UK_CODE2_SIZE			0x3b58
#define UK_BOOT_OFF			0x15b40
#define UK_BOOT_SIZE			0x248
#define UK_TAB_BASE			0x15d90

/* The code buffer: the boot program at 0, the microkernel at 0x1000 (iOS's
 * this+0x6d0), and the entry points into the boot program for core 0 and for
 * the others (0x80bfa068, 0x80bfa06c). */
#define UK_START			0x1000
#define UK_ENTRY_CORE0			0x40
#define UK_ENTRY_OTHERS			0x240

enum { SRC_ZERO, SRC_DATA, SRC_CONST };

enum sgx_buf_id {
	B_CODE,		/* 0x6d4 */
	B_PDS_6D8, B_PDS_6E0, B_PDS_6E8, B_PDS_6F0, B_PDS_6F8,
	B_CODE2,	/* 0x700 */
	B_PDS_704,
	B_TAB_784, B_TAB_78C,
	B_740, B_HOST, B_750, B_758, B_KICKER, B_768,
	B_TAB_774, B_IDX_77C,
	B_794, B_798, B_79C, B_7A0,
	B_NUM
};

static const struct sgx_buf_desc {
	const char *name;
	u32 size, align;
	u8 src;
	u32 off;		/* into __data or __const */
} sgx_buf_descs[B_NUM] = {
	/* 0x80bf9754, the loader */
	[B_CODE]	= { "code 6d4",	UK_START + UK_CODE_SIZE, 0x1000, SRC_ZERO },
	[B_PDS_6D8]	= { "pds 6d8",	0x18,	0x1000, SRC_CONST, 0x084 },
	[B_PDS_6E0]	= { "pds 6e0",	0x38,	0x1000, SRC_CONST, 0x09c },
	[B_PDS_6E8]	= { "pds 6e8",	0x18,	0x1000, SRC_CONST, 0x0d4 },
	[B_PDS_6F0]	= { "pds 6f0",	0x350,	0x1000, SRC_CONST, 0x0ec },
	[B_PDS_6F8]	= { "pds 6f8",	0x9c,	0x1000, SRC_CONST, 0x43c },
	[B_CODE2]	= { "code 700",	UK_CODE2_SIZE, 0x1000, SRC_DATA, UK_CODE2_OFF },
	[B_PDS_704]	= { "pds 704",	0x258,	0x1000, SRC_CONST, 0x4d8 },
	[B_TAB_784]	= { "tab 784",	0xf0,	0x1000, SRC_DATA, UK_TAB_BASE + 0x28 },
	[B_TAB_78C]	= { "tab 78c",	0x140,	0x1000, SRC_DATA, UK_TAB_BASE + 0x118 },
	/* 0x80bfa080, the setup */
	[B_740]		= { "ctl 740",	0x1a4,	0x1000, SRC_ZERO },
	[B_HOST]	= { "host 748",	0xec,	0x1000, SRC_ZERO },
	[B_750]		= { "750",	0x2000,	0x1000, SRC_ZERO },
	[B_758]		= { "758",	8,	0x1000, SRC_ZERO },
	[B_KICKER]	= { "kicker 760", 0x1000, 0x1000, SRC_ZERO },
	[B_768]		= { "768",	0x1500c, 0x1000, SRC_ZERO },
	[B_TAB_774]	= { "tab 774",	0x28,	0x1000, SRC_DATA, UK_TAB_BASE },
	[B_IDX_77C]	= { "idx 77c",	0x60,	0x1000, SRC_ZERO },
	[B_794]		= { "794",	0x14000, 0x100000, SRC_ZERO },
	[B_798]		= { "798",	0x1000,	0x1000, SRC_ZERO },
	[B_79C]		= { "79c",	0x8000,	0x1000, SRC_ZERO },
	[B_7A0]		= { "7a0",	0x28000, 0x1000, SRC_ZERO },
};

struct sgx_buf {
	u32 *cpu;
	dma_addr_t dma;
	u32 va;
	size_t size;
};

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
	struct mutex lock;

	/* the MMU */
	u32 *pd;
	dma_addr_t pd_dma;
	u32 *pt[SGX_PD_ENTRIES];
	dma_addr_t pt_dma[SGX_PD_ENTRIES];
	u32 va_next;

	struct sgx_buf buf[B_NUM];
	bool bufs_ready;
	int boot_result;	/* 0 never tried, 1 acknowledged, -errno */
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

/* ---- the MMU ------------------------------------------------------------ */

static int sgx_mmu_map(struct apple_sgx *sgx, u32 va, dma_addr_t pa, size_t size)
{
	size_t done;

	for (done = 0; done < size; done += SGX_PAGE_SIZE) {
		u32 v = va + done, pde = v >> 22, pte = (v >> 12) & 0x3ff;

		if (!sgx->pt[pde]) {
			sgx->pt[pde] = dmam_alloc_coherent(sgx->dev, SGX_PAGE_SIZE,
							   &sgx->pt_dma[pde], GFP_KERNEL);
			if (!sgx->pt[pde])
				return -ENOMEM;
			sgx->pd[pde] = lower_32_bits(sgx->pt_dma[pde]) | SGX_PDE_VALID;
		}
		sgx->pt[pde][pte] = lower_32_bits(pa + done) | SGX_PTE_VALID;
	}
	return 0;
}

static int sgx_buf_alloc(struct apple_sgx *sgx, enum sgx_buf_id id)
{
	const struct sgx_buf_desc *d = &sgx_buf_descs[id];
	struct sgx_buf *b = &sgx->buf[id];
	int ret;

	b->size = ALIGN(d->size, SGX_PAGE_SIZE);
	b->cpu = dmam_alloc_coherent(sgx->dev, b->size, &b->dma, GFP_KERNEL);
	if (!b->cpu)
		return -ENOMEM;
	b->va = ALIGN(sgx->va_next, d->align);
	sgx->va_next = b->va + b->size;
	ret = sgx_mmu_map(sgx, b->va, b->dma, b->size);
	if (ret)
		return ret;
	dev_dbg(sgx->dev, "%-10s va 0x%08x pa %pad size 0x%zx\n",
		d->name, b->va, &b->dma, b->size);
	return 0;
}

static int sgx_bufs_alloc(struct apple_sgx *sgx)
{
	int i, ret;

	if (sgx->bufs_ready)
		return 0;
	sgx->pd = dmam_alloc_coherent(sgx->dev, SGX_PAGE_SIZE, &sgx->pd_dma,
				      GFP_KERNEL);
	if (!sgx->pd)
		return -ENOMEM;
	sgx->va_next = SGX_VA_BASE;
	for (i = 0; i < B_NUM; i++) {
		ret = sgx_buf_alloc(sgx, i);
		if (ret)
			return ret;
	}
	if (sgx->va_next - SGX_VA_BASE > SZ_8M)
		return -E2BIG;
	sgx->bufs_ready = true;
	return 0;
}

/* ---- the microkernel -------------------------------------------------- */

/* A LIMM's 32-bit immediate lives in three fields: bits 20:0 of the low
 * word, and imm[31:26] / imm[25:21] at bits 17:12 / 8:4 of the high word
 * (iOS: 0x80bfa4e0 and on). */
static void uk_set_limm(u32 *insn, u32 imm)
{
	insn[0] = (insn[0] & ~GENMASK(20, 0)) | (imm & GENMASK(20, 0));
	insn[1] = (insn[1] & 0xfffc0e0f) | ((imm >> 14) & 0x3f000) |
		  ((imm >> 17) & 0x1f0);
}

/* A USE code address as the PDS programs take it: relative to the code
 * buffer, in 8-byte units, shifted (0x80bfaa82). */
static u32 uk_use(struct apple_sgx *sgx, u32 va)
{
	return ((va - sgx->buf[B_CODE].va) << 1) & 0xfffff0;
}

static u32 uk_at(struct apple_sgx *sgx, u32 off)
{
	return sgx->buf[B_CODE].va + UK_START + off;
}

static void sgx_uk_fill(struct apple_sgx *sgx, const u8 *data, const u8 *cnst)
{
	struct sgx_buf *b = sgx->buf;
	u32 *code = b[B_CODE].cpu, *uk = code + UK_START / 4;
	u32 *pds, *t, i;
	static const struct { u32 off; u32 imm; } consts[] = {
		{ 0x0000, 0x01001000 }, { 0x0058, 0x00800900 }, { 0x00f0, 7 },
		{ 0x7f98, 0x02800900 }, { 0x86c8, 0x02800900 },
	};

	/* Contents, as the loader and setup leave them. */
	for (i = 0; i < B_NUM; i++) {
		const struct sgx_buf_desc *d = &sgx_buf_descs[i];

		memset(b[i].cpu, 0, b[i].size);
		if (d->src == SRC_DATA)
			memcpy(b[i].cpu, data + d->off, d->size);
		else if (d->src == SRC_CONST)
			memcpy(b[i].cpu, cnst + d->off, d->size);
	}
	memcpy(code, data + UK_BOOT_OFF, UK_BOOT_SIZE);
	memcpy(uk, data + UK_CODE_OFF, UK_CODE_SIZE);
	for (i = 0; i < 24; i++)
		b[B_IDX_77C].cpu[i] = i;

	/* The microkernel's LIMMs that carry addresses and constants
	 * (0x80bfa4e0-0x80bfa72c, 0x80bfac20-0x80bfad1c, 0x80bfae48-). */
	for (i = 0; i < ARRAY_SIZE(consts); i++)
		uk_set_limm(uk + consts[i].off / 4, consts[i].imm);
	uk_set_limm(uk + 0x010 / 4, b[B_PDS_6E0].va >> 4);
	uk_set_limm(uk + 0x018 / 4, b[B_740].va);
	uk_set_limm(uk + 0x060 / 4, b[B_PDS_6E8].va >> 4);
	uk_set_limm(uk + 0x0e0 / 4, b[B_PDS_6F0].va & ~0xfu);
	uk_set_limm(uk + 0x2b0 / 4, b[B_7A0].va);
	uk_set_limm(uk + 0x340 / 4, b[B_79C].va);
	uk_set_limm(uk + 0x390 / 4, b[B_794].va);
	uk_set_limm(uk + 0x3b0 / 4, b[B_798].va);
	uk_set_limm(uk + 0x588 / 4, b[B_TAB_774].va);
	uk_set_limm(uk + 0x7fb0 / 4, b[B_PDS_6F8].va >> 4);
	uk_set_limm(uk + 0x86e0 / 4, b[B_PDS_6F8].va >> 4);

	/* The boot program: where core 0 continues in the microkernel, and
	 * where the others go into the second code buffer. */
	code[0x70 / 4] |= ((UK_START + 0x9d40) >> 3) & 0xfffff;
	code[0x240 / 4] |= ((b[B_CODE2].va + 0x70 - b[B_CODE].va) >> 3) & 0xfffff;

	/* The PDS programs, pointed at microkernel entry points (0x80bfaa6a-). */
	pds = b[B_PDS_6D8].cpu;
	pds[0] = uk_use(sgx, uk_at(sgx, 0));
	pds[1] = 1;
	pds[2] = 0;
	pds = b[B_PDS_6E0].cpu;
	pds[0] = b[B_740].va;
	pds[1] = 0x3f;
	pds[2] = 0;
	pds[4] = uk_use(sgx, uk_at(sgx, 0x90));
	pds[5] = 1;
	pds = b[B_PDS_6E8].cpu;
	pds[0] = uk_use(sgx, uk_at(sgx, 0xc0));
	pds[1] = 1;
	pds[2] = 0;
	pds = b[B_PDS_6F0].cpu;
	pds[0] = 0;
	pds[1] = 0x100;
	pds[2] = uk_use(sgx, uk_at(sgx, 0x10d20));
	pds[3] = uk_use(sgx, uk_at(sgx, 0xd0d0));
	pds[4] = 0x200;
	pds[6] = uk_use(sgx, uk_at(sgx, 0x4340));
	pds[7] = uk_use(sgx, uk_at(sgx, 0xac50));
	pds[8] = 1;
	pds[9] = 0;
	pds[10] = uk_use(sgx, uk_at(sgx, 0x1470));
	pds = b[B_PDS_6F8].cpu;
	pds[0] = 0;
	pds[1] = 0x200;
	pds[2] = 0;
	pds[3] = 0x8000f;
	pds[4] = uk_use(sgx, uk_at(sgx, 0xd3f0));
	pds[5] = 1;
	pds[6] = 0x240100;
	pds[7] = 0x420000;
	pds[8] = uk_use(sgx, uk_at(sgx, 0x4510));
	pds[9] = 1;
	pds[12] = uk_use(sgx, uk_at(sgx, 0x11190));
	pds[13] = 1;
	pds[16] = uk_use(sgx, uk_at(sgx, 0xad20));
	pds[17] = 1;
	pds = b[B_PDS_704].cpu;
	pds[0] = 0;
	pds[1] = 0x100;
	pds[2] = 0;
	pds[4] = b[B_740].va;
	pds[5] = 0x200;
	pds[8] = uk_use(sgx, b[B_CODE2].va + 0x30) | 0x8000000;
	pds[9] = 1;

	/* The blob's tables, linked up (0x80bfadde-0x80bfae20). */
	t = b[B_TAB_774].cpu;
	t[0] |= b[B_TAB_784].va >> 4;
	t[5] |= (b[B_TAB_784].va + 0x50) >> 4;
	t[7] |= (b[B_TAB_784].va + 0xa0) >> 4;
	t[3] |= b[B_IDX_77C].va;
	t = b[B_TAB_784].cpu;
	t[0x20 / 4] = 0x1000001;
	t[0x70 / 4] = 0x1000101;
	t[0xc0 / 4] = 0x1000231;

	/* The block the microkernel finds through the LIMM at +0x18: the other
	 * buffers and the page directory (0x80bfa898-).  The timing and duty
	 * cycle parameters iOS puts at +0xb0..+0xd8 come from ADT properties;
	 * they stay zero here, but for the 0.8 iOS always writes. */
	t = b[B_740].cpu;
	t[0] = b[B_740].va;
	t[1] = b[B_HOST].va;
	t[2] = b[B_758].va;
	t[0x14 / 4] = 2;
	t[0xcc / 4] = 0x3f4ccccd;
	t[0xe0 / 4] = 0x10;
	t[0xf4 / 4] = b[B_750].va;
	t[0xf8 / 4] = lower_32_bits(sgx->pd_dma);
	t[0x154 / 4] = b[B_768].va;

	/* Everything above is in write-combined memory; it has to be out
	 * before the GPU is told to look. */
	wmb();
}

static void sgx_uk_regs(struct apple_sgx *sgx)
{
	struct sgx_buf *b = sgx->buf;
	unsigned int n;
	int i;

	/* The BIF: one page directory for every requestor, then the MMU on
	 * (initSGX 0x80bf3ac6-0x80bf3b7a). */
	sgx_write(sgx, SGX_BCAST + SGX_BIF_CTRL, 0);
	udelay(10);
	sgx_write(sgx, SGX_BCAST + SGX_BIF_BANK0, 0x77077);
	sgx_write(sgx, SGX_BCAST + SGX_BIF_BANK1, 0);
	sgx_write(sgx, SGX_BCAST + SGX_BIF_BANK_SET, 0);
	for (i = 0; i < 8; i++)
		sgx_write(sgx, SGX_BCAST + SGX_BIF_DIR_LIST_BASE(i),
			  lower_32_bits(sgx->pd_dma));
	sgx_write(sgx, SGX_BCAST + SGX_BIF_TILE1, 0xbeffe00);
	sgx_write(sgx, SGX_BCAST + SGX_BIF_TILE2, 0xcffff00);
	sgx_write(sgx, SGX_BCAST + SGX_BIF_CTRL_INVAL, 8);
	sgx_write(sgx, SGX_MASTER_BIF_MMU_CTRL, 2);
	for (n = 0; n < sgx->ncores; n++)
		sgx_write(sgx, SGX_CORE(n) + SGX_BIF_MMU_CTRL, 2);
	sgx_write(sgx, SGX_MASTER_BIF_CTRL, 0);
	udelay(10);

	/* The rest, in iOS's order (0x80bf3b7e-0x80bf3c6a); names unknown
	 * for most. */
	sgx_write(sgx, SGX_BCAST + 0xa58, 0);
	sgx_write(sgx, SGX_BCAST + SGX_EVENT_TIMER, 0);
	sgx_write(sgx, SGX_BCAST + SGX_POWER, 0);
	sgx_write(sgx, SGX_BCAST + 0xa7c, 0);
	sgx_write(sgx, SGX_BCAST + 0xa80, 0);
	sgx_write(sgx, SGX_BCAST + 0xa00, 0x7c000);
	sgx_write(sgx, SGX_BCAST + 0xabc, 0x4c);
	sgx_write(sgx, SGX_BCAST + 0xaa0, 0x7fffffff);
	sgx_write(sgx, SGX_BCAST + 0x818, 0);
	sgx_write(sgx, SGX_BCAST + 0x804, 0x5e0);
	sgx_write(sgx, SGX_BCAST + 0x814, 0xffff);
	sgx_write(sgx, SGX_BCAST + 0xa74, 0x0da08200);
	sgx_write(sgx, SGX_BCAST + 0xb30, 0x100);
	sgx_write(sgx, SGX_MASTER_REG_4808, 4);
	sgx_write(sgx, SGX_MASTER_REG_4144, 1000);
	sgx_write(sgx, SGX_MASTER_REG_414C, 0);
	for (n = 0; n < sgx->ncores; n++) {
		sgx_write(sgx, SGX_CORE(n) + SGX_BOOT_ENTRY,
			  (n ? UK_ENTRY_OTHERS : UK_ENTRY_CORE0) >> 3);
		sgx_write(sgx, SGX_CORE(n) + SGX_BOOT_ENTRY + 0x14, 0);
	}

	/* Events, code bases, the event PDS programs (0x80bf3d98-0x80bf3eac). */
	sgx_write(sgx, SGX_BCAST + SGX_EVENT_HOST_CLEAR, ~0u);
	sgx_write(sgx, SGX_BCAST + SGX_EVENT_HOST_CLEAR2, ~0u);
	sgx_write(sgx, SGX_BCAST + SGX_REG_140, ~0u);
	sgx_write(sgx, SGX_BCAST + SGX_EVENT_HOST_ENABLE, 0x4000);
	sgx_write(sgx, SGX_BCAST + SGX_EVENT_HOST_ENABLE2, 0);
	sgx_write(sgx, SGX_BCAST + SGX_REG_13C, 0);
	for (i = 2; i <= 14; i++)
		sgx_write(sgx, SGX_BCAST + SGX_USE_CODE_BASE(i), 0);
	sgx_write(sgx, SGX_BCAST + SGX_USE_CODE_BASE(0),
		  0x0c000000 | (b[B_CODE].va >> 6));
	sgx_write(sgx, SGX_BCAST + SGX_EVENT_PDS_EXEC, b[B_PDS_704].va & ~0xfu);
	sgx_write(sgx, SGX_BCAST + SGX_EVENT_PDS_DATA, 3);
	sgx_write(sgx, SGX_BCAST + SGX_EVENT_PDS_INFO, 0x12001);
	sgx_write(sgx, SGX_CORE(0) + SGX_EVENT_PDS_EXEC, b[B_PDS_6D8].va & ~0xfu);
	sgx_write(sgx, SGX_CORE(0) + SGX_EVENT_PDS_DATA, 1);
	sgx_write(sgx, SGX_CORE(0) + SGX_EVENT_PDS_INFO, 0x12001);
	sgx_write(sgx, SGX_BCAST + SGX_USE_CODE_BASE(1), b[B_TAB_78C].va >> 6);
	sgx_write(sgx, SGX_BCAST + SGX_EVENT_KICKER, b[B_KICKER].va);
	sgx_write(sgx, SGX_BCAST + 0x630, 2);
	sgx_write(sgx, SGX_CORE(0) + 0xa58, 0x2200c000);
	sgx_write(sgx, SGX_MASTER_REG_4A58, 0x0804300f);
}

static int sgx_boot_ukernel(struct apple_sgx *sgx)
{
	const struct firmware *fw;
	const struct sgx_fw_header *h;
	u32 *host, val;
	unsigned int n;
	int ret;

	ret = request_firmware(&fw, SGX_FW_NAME, sgx->dev);
	if (ret) {
		dev_err(sgx->dev, "no %s (./cascadia firmware): %d\n",
			SGX_FW_NAME, ret);
		return ret;
	}
	h = (const void *)fw->data;
	if (fw->size < sizeof(*h) || memcmp(h->magic, "SGX543FW", 8) ||
	    le32_to_cpu(h->version) != 1 ||
	    le32_to_cpu(h->data_size) != SGX_FW_DATA_SIZE ||
	    le32_to_cpu(h->const_size) != SGX_FW_CONST_SIZE ||
	    le32_to_cpu(h->data_off) + SGX_FW_DATA_SIZE > fw->size ||
	    le32_to_cpu(h->const_off) + SGX_FW_CONST_SIZE > fw->size ||
	    strncmp(h->build, "12H321", sizeof(h->build))) {
		dev_err(sgx->dev, "%s is not the 12H321 microkernel\n", SGX_FW_NAME);
		ret = -EINVAL;
		goto out;
	}

	ret = sgx_bufs_alloc(sgx);
	if (ret) {
		dev_err(sgx->dev, "GPU buffers: %d\n", ret);
		goto out;
	}

	/* From a clean GPU: clocks and master reset again, then load. */
	sgx_init(sgx);
	sgx_uk_fill(sgx, fw->data + le32_to_cpu(h->data_off),
		    fw->data + le32_to_cpu(h->const_off));
	sgx_uk_regs(sgx);

	/* The buffers sit at GPU addresses from 0x80000000, which is also where
	 * this machine's RAM is: a requestor left in MMU bypass (the reset value
	 * of MASTER_BIF_CTRL has VDM, IPF and DPM in bypass, bits 17-19) would
	 * write straight into the kernel.  So nothing is kicked unless the MMU
	 * is on for everyone. */
	for (n = 0; n <= sgx->ncores; n++) {
		u32 off = n ? SGX_CORE(n - 1) + SGX_BIF_CTRL : SGX_MASTER_BIF_CTRL;

		val = sgx_read(sgx, off);
		if (val & GENMASK(31, 16)) {
			dev_err(sgx->dev, "MMU bypass still on (+0x%05x = 0x%08x), not starting\n",
				off, val);
			ret = -EIO;
			goto out;
		}
	}
	dev_info(sgx->dev, "starting the microkernel: code at GPU 0x%08x, page directory %pad\n",
		 sgx->buf[B_CODE].va, &sgx->pd_dma);

	/* The handshake: host-control word 0 cleared, the kick count bumped,
	 * core 0 kicked; the microkernel sets bit 0 when it is up
	 * (0x80bf3eae-0x80bf3f08). */
	host = sgx->buf[B_HOST].cpu;
	WRITE_ONCE(host[0], 0);
	WRITE_ONCE(sgx->buf[B_KICKER].cpu[0], 1);
	wmb();
	sgx_write(sgx, SGX_CORE(0) + SGX_EVENT_KICK, 1);

	ret = read_poll_timeout(READ_ONCE, val, val & 1, 100, 500000, false,
				host[0]);
	if (ret) {
		dev_err(sgx->dev, "microkernel did not answer: host[0] 0x%08x, "
			"core0 EVENT_STATUS 0x%08x BIF_INT_STAT 0x%08x BIF_FAULT 0x%08x, "
			"master BIF_INT_STAT 0x%08x BIF_FAULT 0x%08x\n",
			READ_ONCE(host[0]),
			sgx_read(sgx, SGX_CORE(0) + SGX_EVENT_STATUS),
			sgx_read(sgx, SGX_CORE(0) + SGX_BIF_INT_STAT),
			sgx_read(sgx, SGX_CORE(0) + SGX_BIF_FAULT),
			sgx_read(sgx, SGX_MASTER_BIF_INT_STAT),
			sgx_read(sgx, SGX_MASTER_BIF_FAULT));
		ret = -ETIMEDOUT;
	} else {
		dev_info(sgx->dev, "microkernel is up: host[0] 0x%08x\n",
			 READ_ONCE(host[0]));
	}
out:
	release_firmware(fw);
	return ret;
}

/* ---- debugfs -------------------------------------------------------------- */

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
	{ SGX_MASTER_EVENT_STATUS,	"MASTER_EVENT_STATUS" },
	{ SGX_MASTER_EVENT_STATUS2,	"MASTER_EVENT_STATUS2" },
	{ SGX_MASTER_BIF_CTRL,		"MASTER_BIF_CTRL" },
	{ SGX_MASTER_BIF_INT_STAT,	"MASTER_BIF_INT_STAT" },
	{ SGX_MASTER_BIF_FAULT,		"MASTER_BIF_FAULT" },
	{ SGX_MASTER_BIF_MEM_REQ_STAT,	"MASTER_BIF_MEM_REQ_STAT" },
	{ SGX_MASTER_SLC_CTRL,		"MASTER_SLC_CTRL" },
	{ SGX_MASTER_SLC_CTRL_BYPASS,	"MASTER_SLC_CTRL_BYPASS" },
}, sgx_core_regs[] = {
	{ SGX_CLKGATECTL,		"CLKGATECTL" },
	{ SGX_CLKGATECTL2,		"CLKGATECTL2" },
	{ SGX_CLKGATESTATUS,		"CLKGATESTATUS" },
	{ SGX_CORE_ID,			"CORE_ID" },
	{ SGX_CORE_REVISION,		"CORE_REVISION" },
	{ SGX_SOFT_RESET,		"SOFT_RESET" },
	{ SGX_EVENT_STATUS,		"EVENT_STATUS" },
	{ SGX_EVENT_STATUS2,		"EVENT_STATUS2" },
	{ SGX_BIF_CTRL,			"BIF_CTRL" },
	{ SGX_BIF_INT_STAT,		"BIF_INT_STAT" },
	{ SGX_BIF_FAULT,		"BIF_FAULT" },
	{ SGX_BIF_MEM_REQ_STAT,		"BIF_MEM_REQ_STAT" },
};

static int sgx_regs_show(struct seq_file *s, void *unused)
{
	struct apple_sgx *sgx = s->private;
	unsigned int i, n;

	mutex_lock(&sgx->lock);
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
	if (sgx->bufs_ready) {
		seq_printf(s, "page directory pa %pad\n", &sgx->pd_dma);
		seq_printf(s, "host control word 0: 0x%08x\n",
			   READ_ONCE(sgx->buf[B_HOST].cpu[0]));
		for (i = 0; i < B_NUM; i++)
			seq_printf(s, "buffer %-10s va 0x%08x pa %pad size 0x%zx\n",
				   sgx_buf_descs[i].name, sgx->buf[i].va,
				   &sgx->buf[i].dma, sgx->buf[i].size);
	}
	seq_printf(s, "microkernel: %s (%d)\n",
		   sgx->boot_result == 1 ? "up" :
		   sgx->boot_result ? "failed" : "not started", sgx->boot_result);
	mutex_unlock(&sgx->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(sgx_regs);

/* Any write starts the microkernel, from a reset GPU each time. */
static ssize_t sgx_boot_write(struct file *file, const char __user *ubuf,
			      size_t len, loff_t *ppos)
{
	struct apple_sgx *sgx = file->private_data;
	int ret;

	mutex_lock(&sgx->lock);
	ret = sgx_boot_ukernel(sgx);
	sgx->boot_result = ret ? ret : 1;
	mutex_unlock(&sgx->lock);
	return ret ? ret : len;
}

static const struct file_operations sgx_boot_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = sgx_boot_write,
	.llseek = noop_llseek,
};

/* ---- the device ----------------------------------------------------------- */

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
	mutex_init(&sgx->lock);
	sgx->brn_31195 = of_property_read_bool(dev->of_node, "apple,brn-31195");

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

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
	debugfs_create_file("boot", 0200, sgx->debugfs, sgx, &sgx_boot_fops);
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

MODULE_DESCRIPTION("Apple S5L8940X PowerVR SGX543MP2 power-on and microkernel start");
MODULE_FIRMWARE(SGX_FW_NAME);
MODULE_LICENSE("GPL");
