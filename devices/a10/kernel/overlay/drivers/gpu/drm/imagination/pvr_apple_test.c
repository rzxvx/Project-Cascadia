// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple A10 GPU: self-tests of the firmware layer (Cascadia G7). The
 * firmware is booted once (on the first command) and stays up; every test
 * submits jobs through queues and waits on their fences, then checks the
 * result in memory:
 *
 *   echo store [N]  > /sys/kernel/debug/pvr-apple/selftest  # G5's store, N jobs one after the other
 *   echo storeq [N] > /sys/kernel/debug/pvr-apple/selftest  # N store jobs in flight at once (N <= 64)
 *   echo render [N] > /sys/kernel/debug/pvr-apple/selftest  # G6's clear + triangle, 64x64, N times
 *   echo stop       > /sys/kernel/debug/pvr-apple/selftest  # firmware and GFX domain off
 *   cat /sys/kernel/debug/pvr-apple/selftest                # the last result
 *
 * GPU memory is an arena of ours, mapped in context 0 by page tables built
 * here (the Rogue MMU format, 4 KiB pages): GPU VA 0x200000 (GV), 0x300000
 * (PMB: region headers, TPC), 0x400000 (G3: the code heap = PDS / USC base,
 * the render target, the PM's buffers and pages, the store's buffers), and a
 * page at VA 0. The jobs are G5's and G6's (tools/gpu/gfx-init.py `--store`,
 * `--pm --own --tri --clear`); the USC code is ours, from the offline
 * compiler (apple/g9p-selftest.bin, tools/gpu/g9p-selftest.py).
 */

#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-fence.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/unaligned.h>

#include "pvr_apple.h"
#include "pvr_device.h"
#include "pvr_gem.h"
#include "pvr_vm.h"

/*
 * test_pvr_vm=1 (default): the test's GPU memory is GEM objects mapped into
 * the kernel's VM context by upstream's pvr_vm / pvr_mmu (16 KiB GPU
 * pages); 0: one DMA arena with page tables built here (4 KiB pages), as
 * the first self-tests had it.
 */
static bool test_pvr_vm = true;
module_param(test_pvr_vm, bool, 0644);
static bool r3_slots;
module_param(r3_slots, bool, 0644);
/* r3_probe=1: every zero 64-bit Start3D slot gets GV + 0x1000 + 0x80 i; the render reports which were written */
static bool r3_probe;
module_param(r3_probe, bool, 0644);

#define GVA	0x200000ULL	/* GPU VA of region GV */
#define G3VA	0x400000ULL	/* GPU VA of region G3 */
#define RGN_VA	0x300000ULL	/* region headers (PMB) */
#define TPC_VA	0x340000ULL	/* tail pointer cache (PMB) */
#define ARENA	0x200000u

/*
 * The powervr UAPI's heaps (pvr_rogue_heap_config.h): the PDS and USC
 * execution bases are global, so the firmware gets those of the UAPI
 * (P+0x10 / P+0x18), and the self-tests' code region (G3) is mapped there
 * too; user VMs take VAs only inside the heaps (data: the general heap).
 */
#define GEN_HEAP	0x8000000000ULL
#define PDS_HEAP	PVR_APPLE_PDS_BASE
#define USC_HEAP	PVR_APPLE_USC_BASE

/* data masters */
#define DM_TA	0
#define DM_3D	1
#define DM_CP	2

struct pvr_apple_test {
	void *cpu;
	dma_addr_t dma;
	/*
	 * One firmware queue per data master: a second one -- in group 1
	 * (channel 5) or a second queue object on channel 2 -- was never
	 * served, and its stuck command held up compute for every queue.
	 */
	struct pvr_apple_queue *cp, *ta, *r3;
	bool vm;				/* GEM objects in the kernel VM context */
	bool code_heaps;			/* G3 also mapped at PDS_HEAP / USC_HEAP */
	u32 boot;				/* the firmware boot the objects belong to */
	struct pvr_gem_object *bo[9];		/* per arena region, in vm mode */
	void *map[9];
	struct pvr_apple_hwrt *rt;
	struct pvr_apple_ta_args ta_args;
	struct pvr_apple_3d_args r3_args;
};

/* the arena's regions, and the register blocks the builders write */
enum { TR_Z, TR_PDR, TR_PTR, TR_PTR2, TR_PTR0, TR_VA0, TR_GV, TR_PMB, TR_G3,
       TR_ARENA_END, TR_TAA = TR_ARENA_END, TR_TAB, TR_S3A, TR_S3B };
static const u32 arena_off[TR_ARENA_END] = {
	[TR_Z] = 0x0000,	/* context 0's page catalog */
	[TR_PDR] = 0x1000,	/* its page directory */
	[TR_PTR] = 0x2000,	/* page table: GPU VA 0x200000.. */
	[TR_PTR2] = 0x3000,	/* GPU VA 0x400000.. */
	[TR_PTR0] = 0x4000,	/* GPU VA 0.. */
	[TR_VA0] = 0x5000,	/* the page at GPU VA 0 */
	[TR_GV] = 0x8000,	/* 0x4000: GPU VA 0x200000.. */
	[TR_PMB] = 0x20000,	/* 0xe0000: GPU VA 0x300000.. */
	[TR_G3] = 0x100000,	/* 0x100000: GPU VA 0x400000.. */
};

/* GPU VAs and sizes of the regions the GEM objects stand for (vm mode) */
static const struct { u64 va; u32 size; } vm_region[TR_ARENA_END] = {
	/* (VA 0: the driver's page in every VM now) */
	[TR_GV] = { 0x200000, 0x4000 },
	[TR_PMB] = { 0x300000, 0xe0000 },
	[TR_G3] = { 0x400000, 0x100000 },
};

#define R(n) TR_##n
#define PA(n) (a->test->dma + arena_off[TR_##n])

/* the bus address behind region r + off (arena or GEM object) */
static u64 region_dma(struct pvr_apple *a, u32 r, u32 off)
{
	struct pvr_apple_test *t = a->test;
	dma_addr_t d = 0;

	if (!t->vm)
		return t->dma + arena_off[r] + off;
	WARN_ON(pvr_gem_get_dma_addr(t->bo[r], off, &d));
	return d;
}

static void *rptr(struct pvr_apple *a, u32 r, u32 off)
{
	struct pvr_apple_test *t = a->test;

	switch (r) {
	case TR_TAA: return (void *)t->ta_args.a + off;
	case TR_TAB: return (void *)t->ta_args.b + off;
	case TR_S3A: return (void *)t->r3_args.a + off;
	case TR_S3B: return (void *)t->r3_args.b + off;
	default: return t->vm ? t->map[r] + off : t->cpu + arena_off[r] + off;
	}
}

static void w32(struct pvr_apple *a, u32 r, u32 off, u32 val)
{
	put_unaligned_le32(val, rptr(a, r, off));
}

static void w64(struct pvr_apple *a, u32 r, u32 off, u64 val)
{
	put_unaligned_le64(val, rptr(a, r, off));
}

static u32 r32(struct pvr_apple *a, u32 r, u32 off)
{
	return get_unaligned_le32(rptr(a, r, off));
}

static void wbytes(struct pvr_apple *a, u32 r, u32 off, const void *src, size_t n)
{
	memcpy(rptr(a, r, off), src, n);
}

/* IEEE-754 single-precision bit patterns (no floating point in the kernel) */
#define F_M1	0xbf800000	/* -1.0 */
#define F_0	0x00000000
#define F_1	0x3f800000
#define F_025	0x3e800000
#define F_05	0x3f000000
#define F_075	0x3f400000
#define F_32	0x42000000
#define F_M32	0xc2000000

/* -- the self-test's USC code: apple/g9p-selftest.bin ------------------ */

struct g9p_blob_entry {
	char name[24];
	__le32 off;
	__le32 size;
};

static int shader(struct pvr_apple *a, const char *name, const u8 **code, u32 *size)
{
	const struct firmware *fw;
	const struct g9p_blob_entry *e;
	u32 n, i;
	int err;

	if (!a->shaders) {
		err = request_firmware(&a->shaders, "apple/g9p-selftest.bin", a->dev);
		if (err) {
			a->shaders = NULL;
			return err;
		}
	}
	fw = a->shaders;
	if (fw->size < 8 || memcmp(fw->data, "G9PT", 4))
		return -EINVAL;
	n = get_unaligned_le32(fw->data + 4);
	if (8 + n * sizeof(*e) > fw->size)
		return -EINVAL;
	e = (const void *)(fw->data + 8);
	for (i = 0; i < n; i++, e++) {
		u32 off = le32_to_cpu(e->off), sz = le32_to_cpu(e->size);

		if (strncmp(e->name, name, sizeof(e->name)))
			continue;
		if (off > fw->size || sz > fw->size - off)
			return -EINVAL;
		*code = fw->data + off;
		*size = sz;
		return 0;
	}
	dev_err(a->dev, "g9p-selftest.bin has no %s\n", name);
	return -ENOENT;
}

/* write a USC program at region+off, zero-padded to 4 bytes */
static int put_shader(struct pvr_apple *a, const char *name, u32 region, u32 off)
{
	static const u8 zero[4];
	const u8 *code;
	u32 size;
	int err = shader(a, name, &code, &size);

	if (!err) {
		wbytes(a, region, off, code, size);
		wbytes(a, region, off + size, zero, -size & 3);
	}
	return err;
}

/* the 64x64 target and its tiling, as upstream pvr_hwrt.c sizes it (32x32 tiles) */
#define RT_W	64
#define RT_H	64
#define TX	2
#define TY	2
#define MX	4
#define MY	4

/* region G3 (GPU VA 0x400000..): programs, target, PM buffers */
#define G3_CODE		0x00000		/* PDS / USC heap = the code base */
#define G3_RT		0x50000
#define G3_MTA		0x56000
#define G3_MLIST	0x57000
#define G3_VHEAP	0x59000
#define G3_FL		0x5b000
#define G3_CTX		0x5c000
#define G3_PPP		0x64000
#define G3_T		0x6c000		/* constants, colour, image state, PPP blocks, stream */
#define G3_PAGES	0x80000
#define PM_PAGES	128

/* USC code offsets in the heap; PDS programs below */
#define USC_TRI_VS	0x400
#define USC_TRI_FS	0x600
#define USC_BG_FS	0x700
#define USC_EOT		0x900
#define USC_END470	0xb00
#define PDS_470		0x1000
#define PDS_VTX		0x1100
#define PDS_VTX_DATA	0x1180
#define PDS_SHARED	0x1200
#define PDS_SHARED_DATA	0x1280
#define PDS_FRAG	0x1300
#define PDS_BG		0x1400
#define PDS_BG_TEXU	0x1480
#define PDS_BG_UNIF	0x14c0
#define PDS_EOT		0x1500
#define PDS_EOT_DATA	0x1540

#define BG_COLOUR	0xff4080bf	/* (0.25, 0.5, 0.75, 1) as BGRA8 */
#define TRI_COLOUR	0xffff0000

static void map_g3(struct pvr_apple *a)
{
	int i;

	w64(a, R(PDR), 8 * 2, PA(PTR2) | 1);		/* VA 0x400000..0x5fffff */
	w64(a, R(PDR), 8 * 0, PA(PTR0) | 1);		/* VA 0..0x1fffff */
	for (i = 0; i < 0x100; i++)
		w64(a, R(PTR2), 8 * i, (PA(G3) + 0x1000 * i) | 1);
	/* a page at GPU VA 0: the end of tile writes there (G6 3D run 33) */
	w64(a, R(PTR0), 0, PA(VA0) | 1);
}

/* the PM, the programs and the VDM stream: gfx-init.py pm_setup() + own_setup() */
static int build_scene(struct pvr_apple *a)
{
	static const u16 eot[] = { 0x0198, 0x0100, 0x0101, 0x0120, 0x0154, 0x0038,	/* o0 = 0 */
				   0x011c, 0x4100, 0x0800,				/* PBE emit */
				   0x8000 };						/* end */
	static const u8 end470[] = { 0x5e, 0x01, 0x00, 0xa0 };
	const u64 tva = G3VA + G3_T;
	u32 pv, i;
	int err;

	if (!a->test->vm)
		map_g3(a);

	/* free list: 128 physical pages (the render target's descriptor points here) */
	for (i = 0; i < PM_PAGES; i++)
		w32(a, R(G3), G3_FL + 4 * i, region_dma(a, R(G3), G3_PAGES + 0x1000 * i) >> 12);
	/* StartTA's 32-bit block: 0xc78, PPP_CTRL, PPP_SCREEN, TE_SCREEN, TE_MTILE1 / 2, 0xc20, TE_PSG */
	w32(a, R(TAA), 0x00, 0x88);
	w32(a, R(TAA), 0x04, 0x2);
	w32(a, R(TAA), 0x08, ((RT_H - 1) << 16) | (RT_W - 1));
	w32(a, R(TAA), 0x0c, ((TY - 1) << 12) | (TX - 1));
	w32(a, R(TAA), 0x10, (MX << 18) | (2 * MX << 9) | 3 * MX);
	w32(a, R(TAA), 0x14, (MY << 18) | (2 * MY << 9) | 3 * MY);
	w32(a, R(TAA), 0x18, MX * MY);
	w32(a, R(TAA), 0x1c, 0x20000);
	/* region headers and the tail pointer cache: PMB at GPU VA 0x300000 */
	for (i = 0; i < 0xe0; i++)
		if (!a->test->vm)
			w64(a, R(PTR), 8 * (((RGN_VA >> 12) & 0x1ff) + i), (PA(PMB) + 0x1000 * i) | 1);
	/* StartTA's 64-bit block */
	w64(a, R(TAB), 0x00, 0x1e3ce508);		/* 0xc90 PDS_CTRL */
	w64(a, R(TAB), 0x08, RGN_VA);			/* 0xc38 region headers */
	w64(a, R(TAB), 0x10, TPC_VA);			/* 0xc40 TPC */
	w64(a, R(TAB), 0x18, 0);			/* 0xc00 TE_AA */
	w64(a, R(TAB), 0x20, 0x88);			/* 0xc80 PPP_MULTISAMPLECTL */
	w64(a, R(TAB), 0x28, G3VA + G3_CTX + 0x100);	/* 0x428 */
	w64(a, R(TAB), 0x30, G3VA + G3_CTX + 0x200);	/* 0xcc0 */
	w64(a, R(TAB), 0x68, PDS_470);			/* 0x470: a PDS program (heap offset) */
	w64(a, R(TAB), 0x70, 0);			/* 0x418 VDM call stack */

	/* the code heap: PDS HALT throughout, then our programs */
	for (i = 0; i < 0x10000; i += 4)
		w32(a, R(G3), G3_CODE + i, 0xd3000000);
	err = put_shader(a, "tri_vs", R(G3), G3_CODE + USC_TRI_VS) ?:
	      put_shader(a, "tri_fs", R(G3), G3_CODE + USC_TRI_FS) ?:
	      put_shader(a, "bg_fs", R(G3), G3_CODE + USC_BG_FS);
	if (err)
		return err;
	wbytes(a, R(G3), G3_CODE + USC_EOT, eot, sizeof(eot));
	wbytes(a, R(G3), G3_CODE + USC_END470, end470, sizeof(end470));

	/* 0x470: data {DOUTU -> end470} + code at +0x20 (DOUTU END, HALT) */
	w32(a, R(G3), PDS_470 + 0x0, USC_END470);
	w32(a, R(G3), PDS_470 + 0x4, 0x10);
	for (i = 0x8; i < 0x20; i += 4)
		w32(a, R(G3), PDS_470 + i, 0);
	w32(a, R(G3), PDS_470 + 0x20, 0xf4000002);
	w32(a, R(G3), PDS_470 + 0x24, 0xd3000000);
	/* vertex: DOUTW, DOUTU, HALT; data {DOUTU -> tri_vs, 0x80000000} */
	w32(a, R(G3), PDS_VTX + 0x0, 0xf0000a01);
	w32(a, R(G3), PDS_VTX + 0x4, 0xfc000002);
	w32(a, R(G3), PDS_VTX + 0x8, 0xd3000000);
	w32(a, R(G3), PDS_VTX_DATA + 0x0, USC_TRI_VS);
	w32(a, R(G3), PDS_VTX_DATA + 0x4, 0x10);
	w32(a, R(G3), PDS_VTX_DATA + 0x8, 0x80000000);
	w32(a, R(G3), PDS_VTX_DATA + 0xc, 0);
	/* shared data: DOUTD END of the corners into the common store */
	w32(a, R(G3), PDS_SHARED + 0x0, 0xf4000800);
	w32(a, R(G3), PDS_SHARED + 0x4, 0xd3000000);
	w64(a, R(G3), PDS_SHARED_DATA + 0x0, tva + 0x800);
	w64(a, R(G3), PDS_SHARED_DATA + 0x8, 0x90000006);
	/* the corners (-1,-1) (0,-1) (-1,1): the bottom-left quarter */
	w32(a, R(G3), G3_T + 0x800, F_M1);
	w32(a, R(G3), G3_T + 0x804, F_M1);
	w32(a, R(G3), G3_T + 0x808, F_0);
	w32(a, R(G3), G3_T + 0x80c, F_M1);
	w32(a, R(G3), G3_T + 0x810, F_M1);
	w32(a, R(G3), G3_T + 0x814, F_1);
	/* fragment: data {DOUTU -> tri_fs} + code at +0x20 (DOUTU END) */
	w32(a, R(G3), PDS_FRAG + 0x0, USC_TRI_FS);
	w32(a, R(G3), PDS_FRAG + 0x4, 0x10);
	for (i = 0x8; i < 0x20; i += 4)
		w32(a, R(G3), PDS_FRAG + i, 0);
	w32(a, R(G3), PDS_FRAG + 0x20, 0xf4000002);
	/* background: {DOUTU -> bg_fs} + DOUTU END; texunicode DOUTD END of the colour */
	w32(a, R(G3), PDS_BG + 0x0, USC_BG_FS);
	w32(a, R(G3), PDS_BG + 0x4, 0x10);
	for (i = 0x8; i < 0x20; i += 4)
		w32(a, R(G3), PDS_BG + i, 0);
	w32(a, R(G3), PDS_BG + 0x20, 0xf4000002);
	w32(a, R(G3), PDS_BG_TEXU + 0x0, 0xf4000800);
	w32(a, R(G3), PDS_BG_TEXU + 0x4, 0xd3000000);
	w32(a, R(G3), G3_T + 0x900, F_025);		/* (0.25, 0.5, 0.75, 1) */
	w32(a, R(G3), G3_T + 0x904, F_05);
	w32(a, R(G3), G3_T + 0x908, F_075);
	w32(a, R(G3), G3_T + 0x90c, F_1);
	w64(a, R(G3), PDS_BG_UNIF + 0x0, tva + 0x900);
	w64(a, R(G3), PDS_BG_UNIF + 0x8, 0x90004004);
	/*
	 * image state: BGRA8 linear 64x64; width - 1 in word 0 bits 27..31 +
	 * word 1 bits 0..8, height - 1 in word 1 bits 9..22, word 2 = address
	 * >> 1, word 3 bits 15.. = row pitch / 64 - 1
	 */
	w32(a, R(G3), G3_T + 0x920, 0x03050502 | (((RT_W - 1) & 0x1f) << 27));
	w32(a, R(G3), G3_T + 0x924, (((RT_W - 1) >> 5) & 0x1ff) | ((RT_H - 1) << 9));
	w32(a, R(G3), G3_T + 0x928, (G3VA + G3_RT) >> 1);
	w32(a, R(G3), G3_T + 0x92c, 0x80000000 | ((4 * RT_W / 64 - 1) << 15));
	w32(a, R(G3), G3_T + 0x930, 0);
	w32(a, R(G3), G3_T + 0x934, 0xc0000000);
	w32(a, R(G3), G3_T + 0x938, 0);
	/* end of tile: DOUTD of the image state, DOUTU END, HALT; data {state, ctl, DOUTU} */
	w32(a, R(G3), PDS_EOT + 0x0, 0xf0000800);
	w32(a, R(G3), PDS_EOT + 0x4, 0xf4000012);
	w32(a, R(G3), PDS_EOT + 0x8, 0xd3000000);
	w64(a, R(G3), PDS_EOT_DATA + 0x0, tva + 0x920);
	w64(a, R(G3), PDS_EOT_DATA + 0x8, 0x90000007);
	w32(a, R(G3), PDS_EOT_DATA + 0x10, USC_EOT);
	w32(a, R(G3), PDS_EOT_DATA + 0x14, 0x10);
	/* the target: a marker, so written pixels show */
	for (i = 0; i < 4 * RT_W * RT_H; i += 4)
		w32(a, R(G3), G3_RT + i, 0x5a5a5a5a);

	/* PPP terminate (for the empty-stream fallback at GV+0x3c00) */
	w32(a, R(G3), G3_PPP + 0x0, 0x02000000);
	w32(a, R(G3), G3_PPP + 0x4, ((TX - 1) << 18) | (TY - 1));
	w32(a, R(G3), G3_PPP + 0x8, 0);
	pv = G3VA + G3_PPP;
	w32(a, R(GV), 0x3c00, 3 << 8);
	w32(a, R(GV), 0x3c04, pv & 0xfffffffc);
	w32(a, R(GV), 0x3c08, 0xc0000000);

	/* PPP state blocks (Mesa csbgen ppp.xml), each starting with STATE_HEADER */
	{
		static const u32 var[] = { 0x001c0000, 0, 0, 0 };
		const u32 frag[] = { 0x000000c0, PDS_FRAG, 0xc0, 0x00800000, 0, 0xc0 };
		static const u32 os[] = { 0x00020000, 0x04010000 };
		static const u32 isp[] = { 0x0000001f, 0x03c40000, 0x003e0f00,
					   0x0e000000, 0x00100f00, 0x00000000 };
		const u32 vp[] = { 0x00000c00, 0x80000000 | (TX - 1), TY - 1,
				   F_32, F_32, F_32, F_M32,	/* viewport: w / 2, h / 2 = 32 */
				   0x00000000, 0x3f800000 };
		static const u32 ctrl[] = { 0x00210000, 0x1e3ce508, 0x000004a0 };
		static const u32 dbsc[] = { 0x00000020, 0x00000000 };
		const u32 term[] = { 0x02000000, ((TX - 1) << 18) | (TY - 1), 0 };
		const struct { const u32 *w; u32 n; } blk[] = {
			{ var, ARRAY_SIZE(var) }, { frag, ARRAY_SIZE(frag) },
			{ os, ARRAY_SIZE(os) }, { isp, ARRAY_SIZE(isp) },
			{ vp, ARRAY_SIZE(vp) }, { ctrl, ARRAY_SIZE(ctrl) },
			{ dbsc, ARRAY_SIZE(dbsc) }, { term, ARRAY_SIZE(term) },
		};
		u32 s[40], n = 0, b;

#define PPP(b) do { u64 at_ = tva + 0x100 + 0x40 * (b);				\
		    s[n++] = (blk[b].n << 8) | (u32)(at_ >> 32);			\
		    s[n++] = (u32)at_ & 0xfffffffc; } while (0)

		for (b = 0; b < ARRAY_SIZE(blk); b++)
			wbytes(a, R(G3), G3_T + 0x100 + 0x40 * b, blk[b].w, 4 * blk[b].n);
		/* VDM_STATE (vertex PDS code, sizes), PPP {varyings} */
		s[n++] = 0x44002f80; s[n++] = PDS_VTX; s[n++] = 0x00000004; s[n++] = 0x00001081;
		PPP(0);
		/* PDS_STATE (shared data DMA), VDM_STATE (vertex PDS data) */
		s[n++] = 0x20020001; s[n++] = PDS_SHARED_DATA; s[n++] = PDS_SHARED;
		s[n++] = 0x48002f80; s[n++] = PDS_VTX_DATA;
		for (b = 1; b <= 6; b++)	/* frag, os, isp, vp, ctrl, dbsc */
			PPP(b);
		/* INDEX_LIST: 3 auto indices, TRI_LIST */
		s[n++] = 0x6e440600; s[n++] = 3; s[n++] = 0; s[n++] = 0; s[n++] = 0xaa;
		PPP(7);
		s[n++] = 0xc0000000;		/* STREAM_TERMINATE */
#undef PPP
		wbytes(a, R(G3), G3_T, s, 4 * n);
	}
	w64(a, R(TAB), 0x78, tva);			/* 0x408 VDM stream */
	if (r3_probe)		/* the zero StartTA slots too: GV + 0x3000 + 0x80 i */
		for (i = 0; i < 28; i++)
			if (!a->test->ta_args.b[i])
				a->test->ta_args.b[i] = GVA + 0x3000 + 0x80 * i;
	return 0;
}

/*
 * G5's store_const, laid out as iPadOS dispatches it (docs "G5 ground
 * truth"), with the code base 0x400000 the render needs too (one firmware
 * boot): every PDS / USC address an offset from it.
 *   code +0x2000 HALT (terminate / context-store PDS) and its data
 *   code +0x2100 the DMA task's PDS program, +0x3000 + 0x40 i job i's data
 *   code +0x2200 the kernel's PDS program, +0x2280 its data; +0x2400 store_const
 *   G3 +0x20000 + 0x20 i job i's argument entry, +0x30000 + 0x40 i its output,
 *   G3 +0x40000 + 0x100 i its CDM stream
 */
#define ST_HALT		0x2000
#define ST_DMA_PDS	0x2100
#define ST_KER_PDS	0x2200
#define ST_KER_DATA	0x2280
#define ST_USC		0x2400
#define ST_DMA_DATA	0x3000
#define ST_ARGS		0x20000
#define ST_OUT		0x30000
#define ST_STREAM	0x40000
#define ST_SLOTS	64

static int store_static(struct pvr_apple *a)
{
	static const u32 dma_pds[] = { 0xf0001000, 0xf4001808, 0xd3000000 };
	static const u32 ker_pds[] = { 0xc2c00003, 0xf0000a01, 0xd3000000,
				       0xf0000e01, 0xfc000002, 0xd3000000 };
	int i, err;

	err = put_shader(a, "store_const", R(G3), G3_CODE + ST_USC);
	if (err)
		return err;
	w32(a, R(G3), ST_HALT, 0xd3000000);
	for (i = 4; i < 0x20; i += 4)
		w32(a, R(G3), ST_HALT + i, 0);
	wbytes(a, R(G3), ST_DMA_PDS, dma_pds, sizeof(dma_pds));
	wbytes(a, R(G3), ST_KER_PDS, ker_pds, sizeof(ker_pds));
	w32(a, R(G3), ST_KER_DATA + 0x0, ST_USC);	/* DOUTU: EXE_OFF, TEMPS 2 */
	w32(a, R(G3), ST_KER_DATA + 0x4, 0x10);
	w32(a, R(G3), ST_KER_DATA + 0x8, 0x90000000);
	w32(a, R(G3), ST_KER_DATA + 0xc, 0x80000000);
	return 0;
}

/* job slot i: sentinel output, argument entry, DMA data, stream; its CDM arguments */
static void store_job_args_out(struct pvr_apple *a, u32 i, u64 out_va, u64 gva, u64 g3va,
			       struct pvr_apple_cp_args *args);

static void store_job_args(struct pvr_apple *a, u32 i, struct pvr_apple_cp_args *args)
{
	store_job_args_out(a, i, G3VA + ST_OUT + 0x40 * i, GVA, G3VA, args);
}

static void ca64(struct pvr_apple_cp_args *args, u32 i, u64 v)
{
	args->ca[i] = lower_32_bits(v);
	args->ca[i + 1] = upper_32_bits(v);
}

/* the same, writing to out_va, with GV / G3 mapped at gva / g3va */
static void store_job_args_out(struct pvr_apple *a, u32 i, u64 out_va, u64 gva, u64 g3va,
			       struct pvr_apple_cp_args *args)
{
	const u32 rec[] = {
		0x4, (ST_DMA_DATA + 0x40 * i) | 0x2, ST_DMA_PDS, 0, 0, 0, 0x40000001, 0,	/* DMA */
		0x2103, ST_KER_DATA, ST_KER_PDS, 0, 0, 0, 0x40000001, 0,			/* kernel: 1 thread */
		0x80000000,
	};
	u64 out = out_va, arg = g3va + ST_ARGS + 0x20 * i;
	u32 k;

	for (k = 0; k < 0x40; k += 4)
		w32(a, R(G3), ST_OUT + 0x40 * i + k, 0xabadbeef);
	w64(a, R(G3), ST_ARGS + 0x20 * i + 0x0, out);	/* {buffer VA, 0, 0x00010001} */
	w64(a, R(G3), ST_ARGS + 0x20 * i + 0x8, 0);
	w32(a, R(G3), ST_ARGS + 0x20 * i + 0x10, 0x00010001);
	w64(a, R(G3), ST_DMA_DATA + 0x40 * i + 0x00, arg);
	w64(a, R(G3), ST_DMA_DATA + 0x40 * i + 0x08, arg + 0x10);
	w64(a, R(G3), ST_DMA_DATA + 0x40 * i + 0x10, 0x10000002);
	w64(a, R(G3), ST_DMA_DATA + 0x40 * i + 0x18, 0x90004003);
	for (k = 0; k < ARRAY_SIZE(rec); k++)
		w32(a, R(G3), ST_STREAM + 0x100 * i + 4 * k, rec[k]);

	memset(args, 0, sizeof(*args));
	ca64(args, 0, gva + 0x2000);			/* RGX+0x498 */
	args->ca[2] = ST_HALT;				/* RGX+0x4b8: terminate PDS code / data */
	args->ca[3] = ST_HALT + 0x10;
	ca64(args, 4, g3va + ST_STREAM + 0x100 * i);	/* RGX+0x480: the stream */
	for (k = 0; k < 6; k++)
		ca64(args, 6 + 2 * k, gva + 0x3000 + 0x100 * k);
	args->ctx[0xe] = ST_HALT;			/* RGX+0x4a8 / +0x4ac */
	args->ctx[0xf] = ST_HALT + 0x10;
	args->fast = false;	/* the full FinalizeCompute: its flush writes the USC's stores out */
}

static bool store_ok(struct pvr_apple *a, u32 i, u32 *out0)
{
	u32 k;

	*out0 = r32(a, R(G3), ST_OUT + 0x40 * i);
	if (*out0 != 0xcafe0000)
		return false;
	for (k = 4; k < 0x40; k += 4)
		if (r32(a, R(G3), ST_OUT + 0x40 * i + k) != 0xabadbeef)
			return false;
	return true;
}

/* the 3D job's register blocks (gfx-init.py build_3d()) */
static void r3_args(struct pvr_apple *a)
{
	struct pvr_apple_3d_args *r = &a->test->r3_args;
	int i;

	memset(r, 0, sizeof(*r));
	r->a[0] = (MX << 16) | MY;			/* 0xf18 ISP_MTILE_SIZE */
	r->a[1] = 0x3f800000;				/* 0xf80 */
	r->a[2] = 0x300;				/* 0xf88 */
	r->a[3] = 0xf;
	r->a[4] = 0x1f;					/* 0x6d0 */
	r->a[5] = PDS_EOT;				/* 0x618 end-of-tile PDS code */
	r->a[6] = PDS_EOT_DATA;				/* 0x620 its data */
	r->a[7] = 0x402;				/* 0x628 info */
	r->a[8] = 0x0023000f;				/* 0xf38 ISP_CTL */
	r->a[9] = 0x408;				/* 0x3e00 */
	r->a[10] = 4;					/* 0xfd8 */
	r->a[11] = 0xf9;				/* 0x4070 */
	r->b[0] = 0x88;					/* 0xfa8 */
	r->b[10] = MX * MY * 5 / 4;			/* 0xf28 ISP_RGN */
	r->b[11] = ((u64)PDS_BG_TEXU << 32) | PDS_BG;	/* 0x6a0 PDS_BGRND0 */
	r->b[13] = PDS_BG_UNIF;				/* 0x6b0 PDS_BGRND2 */
	r->b[14] = 0x0080000000800000ULL;		/* 0x6b8 PDS_BGRND3: sizes */
	for (i = 22; i < 28; i++)
		r->b[i] = 0x1d3;			/* 0x11080 x 6 */
	/*
	 * r3_slots=1: the six 64-bit slots through RGX+0x11020 (b[16..21]) on
	 * the kernel's scratch instead of 0 -- what the end of tile wrote to
	 * GPU VA 0 for (to check); va0=0 drops the page at VA 0.
	 */
	if (r3_slots)
		for (i = 16; i < 22; i++)
			r->b[i] = PVR_APPLE_CDM_SLOTS_VA + 0x800 + 0x100 * (i - 16);
	if (r3_probe)
		for (i = 0; i < 29; i++)
			if (!r->b[i])
				r->b[i] = GVA + 0x1000 + 0x80 * i;
	if (r3_probe)		/* A[12..17] -> RGX 0x11d0..0x11f8, A[3] */
		for (i = 12; i < 18; i++)
			r->a[i] = GVA + 0x2000 + 0x80 * (i - 12);
}

/* vm mode: one GEM object per region, CPU-mapped (write-combined) and mapped at its GPU VA */
static void vm_regions_destroy(struct pvr_apple *a)
{
	struct pvr_apple_test *t = a->test;
	struct pvr_vm_context *vm = a->pvr_dev->kernel_vm_ctx;
	int r;

	if (t->code_heaps) {
		pvr_vm_unmap(vm, PDS_HEAP, vm_region[TR_G3].size);
		pvr_vm_unmap(vm, USC_HEAP, vm_region[TR_G3].size);
		t->code_heaps = false;
	}
	for (r = 0; r < TR_ARENA_END; r++) {
		if (!t->bo[r])
			continue;
		if (t->map[r])
			pvr_gem_object_vunmap(t->bo[r]);
		pvr_vm_unmap_obj(vm, t->bo[r], vm_region[r].va, vm_region[r].size);
		pvr_gem_object_put(t->bo[r]);
		t->bo[r] = NULL;
		t->map[r] = NULL;
	}
}

static int vm_regions_create(struct pvr_apple *a)
{
	struct pvr_apple_test *t = a->test;
	struct pvr_vm_context *vm = a->pvr_dev->kernel_vm_ctx;
	int r, err;

	for (r = 0; r < TR_ARENA_END; r++) {
		struct pvr_gem_object *bo;
		void *p;

		if (!vm_region[r].size)
			continue;
		bo = pvr_gem_object_create(a->pvr_dev, vm_region[r].size, 0);
		if (IS_ERR(bo))
			return PTR_ERR(bo);
		err = pvr_vm_map(vm, bo, 0, vm_region[r].va, vm_region[r].size);
		if (err) {
			pvr_gem_object_put(bo);
			return err;
		}
		t->bo[r] = bo;
		p = pvr_gem_object_vmap(bo);
		if (IS_ERR(p))
			return PTR_ERR(p);
		t->map[r] = p;
	}
	return 0;
}

static void test_free(struct pvr_apple *a)
{
	struct pvr_apple_test *t = a->test;

	if (!t)
		return;
	pvr_apple_hwrt_destroy(t->rt);		/* (a no-op after a firmware restart) */
	if (t->cpu)
		dma_free_coherent(a->dev, ARENA, t->cpu, t->dma);
	vm_regions_destroy(a);
	kfree(t);
	a->test = NULL;
}

void pvr_apple_test_fini(struct pvr_apple *a)
{
	test_free(a);
}

/* the arena, its page tables and programs; the firmware; the queues and the render target */
static int test_start(struct pvr_apple *a)
{
	struct pvr_vm_context *kvm = a->pvr_dev->kernel_vm_ctx;
	struct pvr_apple_test *t;
	int err;

	/* the driver's firmware (restarted after a crash: then our objects go too) */
	if (a->test && (a->crashed || !a->running || a->test->boot != a->boot))
		test_free(a);
	err = pvr_apple_start_locked(a);
	if (err)
		return err;
	if (a->test)
		return 0;
	if (!test_pvr_vm)
		return -EOPNOTSUPP;	/* the arena mode needs a firmware of its own: gone */
	t = kzalloc(sizeof(*t), GFP_KERNEL);
	if (!t)
		return -ENOMEM;
	a->test = t;
	t->vm = true;
	t->boot = a->boot;
	t->cp = a->q[DM_CP];
	t->ta = a->q[DM_TA];
	t->r3 = a->q[DM_3D];
	err = vm_regions_create(a);
	if (err)
		goto err;
	/* the code region at the UAPI's PDS / USC heap bases too */
	err = pvr_vm_map(kvm, t->bo[TR_G3], 0, PDS_HEAP, vm_region[TR_G3].size) ?:
	      pvr_vm_map(kvm, t->bo[TR_G3], 0, USC_HEAP, vm_region[TR_G3].size);
	if (err)
		goto err;
	t->code_heaps = true;
	err = build_scene(a) ?: store_static(a);
	if (err)
		goto err;
	r3_args(a);
	t->rt = pvr_apple_hwrt_create(a, G3VA + G3_FL, PM_PAGES, G3VA + G3_MLIST,
				      G3VA + G3_MTA, G3VA + G3_VHEAP);
	if (IS_ERR(t->rt)) {
		err = PTR_ERR(t->rt);
		t->rt = NULL;
		goto err;
	}
	return 0;

err:
	test_free(a);
	return err;
}

/* wait for a fence; returns 0, its error, or -ETIMEDOUT */
static int wait(struct dma_fence *f, unsigned int ms)
{
	long t;
	int err;

	if (IS_ERR(f))
		return PTR_ERR(f);
	t = dma_fence_wait_timeout(f, false, msecs_to_jiffies(ms));
	err = t == 0 ? -ETIMEDOUT : t < 0 ? (int)t : f->error;
	dma_fence_put(f);
	return err;
}

static struct dma_fence *submit_store(struct pvr_apple *a, u32 slot, struct pvr_apple_queue *q)
{
	struct pvr_apple_cp_args args;
	struct pvr_apple_job *job;

	store_job_args(a, slot, &args);
	job = pvr_apple_job_cp(q, &args);
	if (IS_ERR(job))
		return ERR_CAST(job);
	return pvr_apple_submit(job);
}

/* N store jobs one after the other on one firmware boot */
static int test_store(struct pvr_apple *a, u32 n, char *res, size_t len)
{
	u32 i, ok = 0, out0 = 0, first_bad = ~0u, k;
	ktime_t t0 = ktime_get();
	int err = 0, e;

	for (i = 0; i < n; i++) {
		e = wait(submit_store(a, i % ST_SLOTS, a->test->cp), 2000);
		if (!e && store_ok(a, i % ST_SLOTS, &out0))
			ok++;
		else if (first_bad == ~0u) {
			first_bad = i;
			err = e ?: -EIO;
			if (e)
				break;
		}
	}
	k = scnprintf(res, len, "store x%u: %s -- %u of %u wrote cafe0000; first failure %d (job %d, out %08x); %lld us per job%s; ",
		  n, ok == n ? "PASS" : "FAIL", ok, n, err, (int)first_bad, out0,
		  n ? ktime_us_delta(ktime_get(), t0) / n : 0, a->crashed ? "; firmware crashed" : "");
	k += pvr_apple_queue_debug(a->test->cp, res + k, len - k);
	scnprintf(res + k, len - k, "\n");
	return ok == n ? 0 : (err ?: -EIO);
}

/* N store jobs submitted at once, then wait for all */
static int test_storeq(struct pvr_apple *a, u32 n, char *res, size_t len)
{
	struct dma_fence *f[ST_SLOTS];
	u32 i, ok = 0, out0;
	ktime_t t0 = ktime_get();
	int err = 0, e;

	n = min_t(u32, n, ST_SLOTS);
	for (i = 0; i < n; i++)
		f[i] = submit_store(a, i, a->test->cp);
	for (i = 0; i < n; i++) {
		e = wait(f[i], 3000);
		if (!e && store_ok(a, i, &out0))
			ok++;
		else if (!err)
			err = e ?: -EIO;
	}
	scnprintf(res, len, "storeq x%u: %s -- %u of %u wrote cafe0000 (%d); %lld us for all%s\n",
		  n, ok == n ? "PASS" : "FAIL", ok, n, err, ktime_us_delta(ktime_get(), t0),
		  a->crashed ? "; firmware crashed" : "");
	return ok == n ? 0 : (err ?: -EIO);
}

/*
 * MMU invalidation: one GPU VA, mapped in turn onto two objects; a store
 * job each time must land in the object mapped now. A stale translation
 * would write into the other one.
 */
#define REMAP_VA	0x800000ULL

static int test_remap(struct pvr_apple *a, u32 n, char *res, size_t len)
{
	struct pvr_vm_context *vm = a->pvr_dev->kernel_vm_ctx;
	struct pvr_gem_object *bo[2] = { NULL, NULL };
	u32 *map[2] = { NULL, NULL }, k, ok = 0, got = 0, other = 0;
	int err = 0, e, i;

	if (!a->test->vm) {
		scnprintf(res, len, "remap: needs test_pvr_vm=1\n");
		return -EINVAL;
	}
	for (i = 0; i < 2; i++) {
		bo[i] = pvr_gem_object_create(a->pvr_dev, 0x4000, 0);
		if (IS_ERR(bo[i])) {
			err = PTR_ERR(bo[i]);
			bo[i] = NULL;
			goto out;
		}
		map[i] = pvr_gem_object_vmap(bo[i]);
		if (IS_ERR(map[i])) {
			err = PTR_ERR(map[i]);
			map[i] = NULL;
			goto out;
		}
	}
	for (k = 0; k < n; k++) {
		struct pvr_apple_cp_args args;
		struct pvr_apple_job *job;
		u32 cur = k & 1;

		map[0][0] = map[1][0] = 0xabadbeef;
		err = pvr_vm_map(vm, bo[cur], 0, REMAP_VA, 0x4000);
		if (err)
			break;
		store_job_args_out(a, 0, REMAP_VA, GVA, G3VA, &args);
		job = pvr_apple_job_cp(a->test->cp, &args);
		e = IS_ERR(job) ? PTR_ERR(job) : wait(pvr_apple_submit(job), 2000);
		got = READ_ONCE(map[cur][0]);
		other = READ_ONCE(map[!cur][0]);
		pvr_vm_unmap(vm, REMAP_VA, 0x4000);
		if (!e && got == 0xcafe0000 && other == 0xabadbeef) {
			ok++;
		} else {
			err = e ?: -EIO;
			break;
		}
	}
out:
	scnprintf(res, len, "remap x%u: %s -- %u of %u stores went to the object mapped at the time (%d); last: mapped %08x, other %08x%s\n",
		  n, ok == n ? "PASS" : "FAIL", ok, n, err, got, other, a->crashed ? "; firmware crashed" : "");
	for (i = 0; i < 2; i++) {
		if (map[i])
			pvr_gem_object_vunmap(bo[i]);
		if (bo[i])
			pvr_gem_object_put(bo[i]);
	}
	return err;
}

/*
 * A second VM on its own MMU context: the same objects at the same VAs,
 * but a different object at REMAP_VA than the kernel's VM. Store jobs on
 * context 0 and on the second context, in turn, must each write into their
 * own VM's object there.
 */
static int test_vm2(struct pvr_apple *a, u32 n, char *res, size_t len)
{
	struct pvr_vm_context *kvm = a->pvr_dev->kernel_vm_ctx, *vm2;
	struct pvr_apple_test *t = a->test;
	struct pvr_gem_object *bo[2] = { NULL, NULL };
	u32 *map[2] = { NULL, NULL }, k, ok = 0, got[2] = { 0, 0 };
	int err = 0, e, i;
	bool mapped[2] = { false, false };

	if (!t->vm) {
		scnprintf(res, len, "vm2: needs test_pvr_vm=1\n");
		return -EINVAL;
	}
	vm2 = pvr_vm_create_context(a->pvr_dev, true);
	if (IS_ERR(vm2)) {
		scnprintf(res, len, "vm2: no VM context (%ld)\n", PTR_ERR(vm2));
		return PTR_ERR(vm2);
	}
	/* in the second VM: G3 in the general heap and at the code bases, GV in the general heap */
	err = pvr_vm_map(vm2, t->bo[TR_G3], 0, GEN_HEAP + G3VA, vm_region[TR_G3].size) ?:
	      pvr_vm_map(vm2, t->bo[TR_G3], 0, PDS_HEAP, vm_region[TR_G3].size) ?:
	      pvr_vm_map(vm2, t->bo[TR_G3], 0, USC_HEAP, vm_region[TR_G3].size) ?:
	      pvr_vm_map(vm2, t->bo[TR_GV], 0, GEN_HEAP + GVA, vm_region[TR_GV].size);
	for (i = 0; i < 2 && !err; i++) {
		bo[i] = pvr_gem_object_create(a->pvr_dev, 0x4000, 0);
		if (IS_ERR(bo[i])) {
			err = PTR_ERR(bo[i]);
			bo[i] = NULL;
			break;
		}
		map[i] = pvr_gem_object_vmap(bo[i]);
		if (IS_ERR(map[i])) {
			err = PTR_ERR(map[i]);
			map[i] = NULL;
			break;
		}
		err = pvr_vm_map(i ? vm2 : kvm, bo[i], 0, i ? GEN_HEAP + REMAP_VA : REMAP_VA, 0x4000);
		mapped[i] = !err;
	}
	for (k = 0; k < n && !err; k++) {
		struct pvr_apple_cp_args args;
		struct pvr_apple_job *job;
		u32 which = k & 1;

		map[0][0] = map[1][0] = 0xabadbeef;
		if (which)
			store_job_args_out(a, 1, GEN_HEAP + REMAP_VA, GEN_HEAP + GVA, GEN_HEAP + G3VA, &args);
		else
			store_job_args_out(a, 0, REMAP_VA, GVA, G3VA, &args);
		job = pvr_apple_job_cp(t->cp, &args);
		if (!IS_ERR(job) && which)
			job->vm = vm2;
		e = IS_ERR(job) ? PTR_ERR(job) : wait(pvr_apple_submit(job), 2000);
		got[0] = READ_ONCE(map[0][0]);
		got[1] = READ_ONCE(map[1][0]);
		if (!e && got[which] == 0xcafe0000 && got[!which] == 0xabadbeef)
			ok++;
		else
			err = e ?: -EIO;
	}
	scnprintf(res, len, "vm2 x%u: %s -- %u of %u stores landed in their own VM (jobs of the two VMs in turn) (%d); last: kernel VM %08x, second VM %08x%s\n",
		  n, ok == n ? "PASS" : "FAIL", ok, n, err, got[0], got[1], a->crashed ? "; firmware crashed" : "");
	pvr_apple_vm_release(a, vm2);
	for (i = 0; i < 2; i++) {
		if (mapped[i])
			pvr_vm_unmap(i ? vm2 : kvm, i ? GEN_HEAP + REMAP_VA : REMAP_VA, 0x4000);
		if (map[i])
			pvr_gem_object_vunmap(bo[i]);
		if (bo[i])
			pvr_gem_object_put(bo[i]);
	}
	pvr_vm_unmap_all(vm2);
	pvr_vm_context_put(vm2);
	return err;
}

/* N renders: TA, then 3D, each time into a freshly marked target */
static int test_render(struct pvr_apple *a, u32 n, char *res, size_t len)
{
	struct pvr_apple_test *t = a->test;
	u32 bg = 0, tri = 0, mark = 0, other = 0, first_other = 0, i, k, ok = 0;
	ktime_t t0 = ktime_get();
	int err = 0, e;

	for (k = 0; k < n; k++) {
		struct pvr_apple_job *job;

		for (i = 0; i < 4 * RT_W * RT_H; i += 4)
			w32(a, R(G3), G3_RT + i, 0x5a5a5a5a);
		if (r3_probe && t->bo[TR_VA0])
			for (i = 0; i < 0x4000; i += 4)
				w32(a, R(VA0), i, 0xa5a5a5a5);
		job = pvr_apple_job_ta(t->ta, &t->ta_args, t->rt);
		e = IS_ERR(job) ? PTR_ERR(job) : wait(pvr_apple_submit(job), 2000);
		if (r3_probe) {
			u32 fs = 0, fa = 0;

			pvr_apple_rgx_read(a, 0xe150, &fs);
			pvr_apple_rgx_read(a, 0xe154, &fa);
			dev_info(a->dev, "r3_probe: after the TA: fault status %#x %#x\n", fs, fa);
		}
		if (!e) {
			job = pvr_apple_job_3d(t->r3, &t->r3_args, t->rt);
			e = IS_ERR(job) ? PTR_ERR(job) : wait(pvr_apple_submit(job), 3000);
		}
		bg = tri = mark = other = 0;
		for (i = 0; i < 4 * RT_W * RT_H; i += 4) {
			u32 px = r32(a, R(G3), G3_RT + i);

			if (px == BG_COLOUR)
				bg++;
			else if (px == TRI_COLOUR)
				tri++;
			else if (px == 0x5a5a5a5a)
				mark++;
			else if (!other++)
				first_other = px;
		}
		if (r3_probe) {
			u32 q, w, any = 0;

			for (q = 0; q < 29; q++)
				for (w = 0; w < 0x80; w += 4)
					if (r32(a, R(GV), 0x1000 + 0x80 * q + w)) {
						dev_info(a->dev, "r3_probe: Start3D b[%u] +%#x = %#x\n", q, w,
							 r32(a, R(GV), 0x1000 + 0x80 * q + w));
						any++;
					}
			for (q = 0; q < 6; q++)
				for (w = 0; w < 0x80; w += 4)
					if (r32(a, R(GV), 0x2000 + 0x80 * q + w)) {
						dev_info(a->dev, "r3_probe: Start3D a[%u] +%#x = %#x\n", 12 + q, w,
							 r32(a, R(GV), 0x2000 + 0x80 * q + w));
						any++;
					}
			for (q = 0; q < 28; q++)
				for (w = 0; w < 0x80; w += 4)
					if (r32(a, R(GV), 0x3000 + 0x80 * q + w)) {
						dev_info(a->dev, "r3_probe: StartTA b[%u] +%#x = %#x\n", q, w,
							 r32(a, R(GV), 0x3000 + 0x80 * q + w));
						any++;
					}
			if (t->bo[TR_VA0])
				for (w = 0; w < 0x4000; w += 4)
					if (r32(a, R(VA0), w) != 0xa5a5a5a5 && any++ < 64)
						dev_info(a->dev, "r3_probe: VA0 +%#x = %#x\n", w, r32(a, R(VA0), w));
			dev_info(a->dev, "r3_probe: %u words written\n", any);
		}
		if (!e && bg == 3072 && tri == 1024)
			ok++;
		else {
			err = e ?: -EIO;
			break;
		}
	}
	scnprintf(res, len,
		  "render x%u: %s -- %u of %u right (%d); last: %u background %08x, %u triangle %08x, %u untouched, %u other (first %08x); %lld us per render%s\n",
		  n, ok == n ? "PASS" : "FAIL", ok, n, err, bg, BG_COLOUR, tri, TRI_COLOUR, mark, other,
		  first_other, n ? ktime_us_delta(ktime_get(), t0) / n : 0,
		  a->crashed ? "; firmware crashed" : "");
	return err;
}

/* a few RGX words for the log, when the power state allows it */
static void rgx_dump(struct pvr_apple *a, char *res, size_t len)
{
	static const u32 regs[] = { 0xe150, 0xe154, 0xe158, 0x4a0, 0x480, 0x610,
				    0x4008, 0x12e0, 0x12e8, 0x8328 };
	size_t n = strlen(res);
	u32 v;
	int i;

	if (n && res[n - 1] == '\n')
		n--;
	if (a->powered)
		n += scnprintf(res + n, len - n, "; Fender +0x10=%#x +0x14=%#x +0x1000=%#x +0x2044=%#x +0x2058=%#x",
			       readl(a->fender + 0x10), readl(a->fender + 0x14),
			       readl(a->fender + 0x1000), readl(a->fender + 0x2044),
			       readl(a->fender + 0x2058));
	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		if (pvr_apple_rgx_read(a, regs[i], &v)) {
			n += scnprintf(res + n, len - n, "; RGX not readable");
			break;
		}
		n += scnprintf(res + n, len - n, "%s%#x=%#x", i ? " " : "; RGX ", regs[i], v);
	}
	scnprintf(res + n, len - n, "\n");
}

/* -- debugfs --------------------------------------------------------------- */

static ssize_t selftest_write(struct file *f, const char __user *ubuf, size_t len, loff_t *pos)
{
	struct pvr_apple *a = file_inode(f)->i_private;
	char cmd[32] = { 0 }, res[sizeof(a->result)] = { 0 }, *arg;
	u32 n = 1;
	int err;

	if (copy_from_user(cmd, ubuf, min(len, sizeof(cmd) - 1)))
		return -EFAULT;
	strim(cmd);
	arg = strchr(cmd, ' ');
	if (arg) {
		*arg++ = 0;
		if (kstrtou32(strim(arg), 0, &n) || !n || n > 100000)
			return -EINVAL;
	}

	mutex_lock(&a->lock);
	if (!strcmp(cmd, "stop")) {
		test_free(a);
		pvr_apple_stop_locked(a);
		scnprintf(a->result, sizeof(a->result), "stopped\n");
		mutex_unlock(&a->lock);
		return len;
	}
	if (!strcmp(cmd, "status")) {
		size_t k = 0;
		int q;

		for (q = 0; q < 3; q++)
			if (a->q[q])
				k += pvr_apple_queue_debug(a->q[q], res + k, sizeof(res) - k),
				k += scnprintf(res + k, sizeof(res) - k, "\n");
		rgx_dump(a, res, sizeof(res));
		scnprintf(a->result, sizeof(a->result), "%s%s", res, a->crashed ? "firmware crashed\n" : "");
		mutex_unlock(&a->lock);
		return len;
	}
	if (strcmp(cmd, "store") && strcmp(cmd, "storeq") && strcmp(cmd, "render") && strcmp(cmd, "remap") && strcmp(cmd, "vm2") && strcmp(cmd, "start")) {
		mutex_unlock(&a->lock);
		return -EINVAL;
	}
	err = test_start(a);
	if (err)
		scnprintf(res, sizeof(res), "start: error %d%s\n", err, a->crashed ? " (firmware crashed)" : "");
	else if (!strcmp(cmd, "store"))
		err = test_store(a, n, res, sizeof(res));
	else if (!strcmp(cmd, "storeq"))
		err = test_storeq(a, n, res, sizeof(res));
	else if (!strcmp(cmd, "render"))
		err = test_render(a, n, res, sizeof(res));
	else if (!strcmp(cmd, "remap"))
		err = test_remap(a, n, res, sizeof(res));
	else if (!strcmp(cmd, "vm2"))
		err = test_vm2(a, n, res, sizeof(res));

	else
		scnprintf(res, sizeof(res), "started\n");
	pvr_apple_drain_rings(a);
	rgx_dump(a, res, sizeof(res));
	scnprintf(a->result, sizeof(a->result),
		  "%sfirmware ring messages %u; job events: 0x1 x%u, 0x4 (fault) x%u\n",
		  res, a->ring_msgs, a->ev_count[0][1], a->ev_count[0][4]);
	dev_info(a->dev, "selftest %s", a->result);
	mutex_unlock(&a->lock);
	return len;
}

static ssize_t selftest_read(struct file *f, char __user *ubuf, size_t len, loff_t *pos)
{
	struct pvr_apple *a = file_inode(f)->i_private;

	return simple_read_from_buffer(ubuf, len, pos, a->result, strlen(a->result));
}

static const struct file_operations selftest_fops = {
	.owner = THIS_MODULE,
	.read = selftest_read,
	.write = selftest_write,
	.llseek = default_llseek,
};

/* fender: the nonzero words of Fender 0..0x4000 (only with the GFX domain on) */
static int fender_show(struct seq_file *m, void *unused)
{
	struct pvr_apple *a = m->private;
	u32 off, v;

	mutex_lock(&a->lock);
	if (!a->powered) {
		seq_puts(m, "GFX domain off\n");
	} else {
		for (off = 0; off < 0x4000; off += 4) {
			v = readl(a->fender + off);
			if (v)
				seq_printf(m, "%#06x %#010x\n", off, v);
		}
	}
	mutex_unlock(&a->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(fender);

/* rgx-standin: what the firmware wrote to the RAM it took for RGX */
static int rgx_standin_show(struct seq_file *m, void *unused)
{
	struct pvr_apple *a = m->private;
	u32 off, v;

	mutex_lock(&a->lock);
	if (a->standin)
		for (off = 0; off < SZ_128K; off += 4) {
			v = READ_ONCE(*(u32 *)(a->standin + off));
			if (v)
				seq_printf(m, "%#07x %#010x\n", off, v);
		}
	mutex_unlock(&a->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(rgx_standin);

void pvr_apple_debugfs_init(struct pvr_apple *a)
{
	a->debugfs = debugfs_create_dir("pvr-apple", NULL);
	debugfs_create_file("selftest", 0600, a->debugfs, a, &selftest_fops);
	debugfs_create_file("fender", 0400, a->debugfs, a, &fender_fops);
	debugfs_create_file("rgx-standin", 0400, a->debugfs, a, &rgx_standin_fops);
	scnprintf(a->result, sizeof(a->result), "no self-test run yet: echo store|storeq|render [N] > selftest\n");
}
