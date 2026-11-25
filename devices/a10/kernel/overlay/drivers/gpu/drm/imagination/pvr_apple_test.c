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

#define GVA	0x200000ULL	/* GPU VA of region GV */
#define G3VA	0x400000ULL	/* GPU VA of region G3 */
#define RGN_VA	0x300000ULL	/* region headers (PMB) */
#define TPC_VA	0x340000ULL	/* tail pointer cache (PMB) */
#define ARENA	0x200000u

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

#define R(n) TR_##n
#define PA(n) (a->test->dma + arena_off[TR_##n])

static void *rptr(struct pvr_apple *a, u32 r, u32 off)
{
	struct pvr_apple_test *t = a->test;

	switch (r) {
	case TR_TAA: return (void *)t->ta_args.a + off;
	case TR_TAB: return (void *)t->ta_args.b + off;
	case TR_S3A: return (void *)t->r3_args.a + off;
	case TR_S3B: return (void *)t->r3_args.b + off;
	default: return t->cpu + arena_off[r] + off;
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

	map_g3(a);

	/* free list: 128 physical pages (the render target's descriptor points here) */
	for (i = 0; i < PM_PAGES; i++)
		w32(a, R(G3), G3_FL + 4 * i, (PA(G3) + G3_PAGES + 0x1000 * i) >> 12);
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
static void store_job_args(struct pvr_apple *a, u32 i, struct pvr_apple_cp_args *args)
{
	const u32 rec[] = {
		0x4, (ST_DMA_DATA + 0x40 * i) | 0x2, ST_DMA_PDS, 0, 0, 0, 0x40000001, 0,	/* DMA */
		0x2103, ST_KER_DATA, ST_KER_PDS, 0, 0, 0, 0x40000001, 0,			/* kernel: 1 thread */
		0x80000000,
	};
	u64 out = G3VA + ST_OUT + 0x40 * i, arg = G3VA + ST_ARGS + 0x20 * i;
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
	args->ca[0] = GVA + 0x2000;			/* RGX+0x498 */
	args->ca[2] = ST_HALT;				/* RGX+0x4b8: terminate PDS code / data */
	args->ca[3] = ST_HALT + 0x10;
	args->ca[4] = G3VA + ST_STREAM + 0x100 * i;	/* RGX+0x480: the stream */
	for (k = 0; k < 6; k++)
		args->ca[6 + 2 * k] = GVA + 0x3000 + 0x100 * k;
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
}

static void test_free(struct pvr_apple *a)
{
	struct pvr_apple_test *t = a->test;

	if (!t)
		return;
	pvr_apple_fw_stop(a);			/* also destroys the queues */
	pvr_apple_hwrt_destroy(t->rt);
	if (t->cpu)
		dma_free_coherent(a->dev, ARENA, t->cpu, t->dma);
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
	struct pvr_apple_boot_cfg cfg = { .code_base = G3VA, .perf_state = 6 };
	struct pvr_apple_test *t;
	int i, err;

	if (a->test && a->crashed)
		test_free(a);		/* a crashed firmware: start again */
	if (a->test)
		return 0;
	t = kzalloc(sizeof(*t), GFP_KERNEL);
	if (!t)
		return -ENOMEM;
	a->test = t;
	t->cpu = dma_alloc_coherent(a->dev, ARENA, &t->dma, GFP_KERNEL);
	if (!t->cpu) {
		err = -ENOMEM;
		goto err;
	}

	/* context 0: GPU VA 0x200000.. onto GV, 0x300000.. onto PMB (in PTR), 0x400000.. G3, 0 the VA0 page */
	w32(a, R(Z), 0, ((PA(PDR) >> 12) << 4) | 1);
	w64(a, R(PDR), 8 * 1, PA(PTR) | 1);
	for (i = 0; i < 4; i++)
		w64(a, R(PTR), 8 * i, (PA(GV) + 0x1000 * i) | 1);
	for (i = 0; i < 0xe0; i++)
		w64(a, R(PTR), 8 * (((RGN_VA >> 12) & 0x1ff) + i), (PA(PMB) + 0x1000 * i) | 1);
	err = build_scene(a) ?: store_static(a);
	if (err)
		goto err;
	r3_args(a);

	cfg.ctx0_pc = PA(Z);
	cfg.fence_va = GVA + 0x3800;	/* the flush before a power-off writes there */
	err = pvr_apple_fw_boot(a, &cfg) ?: pvr_apple_fw_init(a, &cfg);
	if (err)
		goto err;
	t->cp = pvr_apple_queue_create(a, 0, DM_CP);
	t->ta = pvr_apple_queue_create(a, 0, DM_TA);
	t->r3 = pvr_apple_queue_create(a, 0, DM_3D);
	t->rt = pvr_apple_hwrt_create(a, G3VA + G3_FL, PM_PAGES, G3VA + G3_MLIST,
				      G3VA + G3_MTA, G3VA + G3_VHEAP);
	if (IS_ERR(t->cp) || IS_ERR(t->ta) || IS_ERR(t->r3) || IS_ERR(t->rt)) {
		if (IS_ERR(t->rt))
			t->rt = NULL;
		err = -ENOMEM;
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
		job = pvr_apple_job_ta(t->ta, &t->ta_args, t->rt);
		e = IS_ERR(job) ? PTR_ERR(job) : wait(pvr_apple_submit(job), 2000);
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
		pvr_apple_fw_stop(a);
		scnprintf(a->result, sizeof(a->result), "stopped\n");
		mutex_unlock(&a->lock);
		return len;
	}
	if (strcmp(cmd, "store") && strcmp(cmd, "storeq") && strcmp(cmd, "render") && strcmp(cmd, "start")) {
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
