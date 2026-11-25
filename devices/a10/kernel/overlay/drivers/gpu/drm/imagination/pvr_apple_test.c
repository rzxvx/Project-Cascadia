// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple A10 GPU: the first jobs from the kernel, without the Python harness
 * (Cascadia G7, first milestone). Each self-test boots the firmware cold,
 * hands it the init data and runs one job, then checks the result in memory:
 *
 *   echo store  > /sys/kernel/debug/pvr-apple/selftest   # G5: a compute store
 *   echo render > /sys/kernel/debug/pvr-apple/selftest   # G6: clear + triangle, 64x64
 *   echo stop   > /sys/kernel/debug/pvr-apple/selftest   # firmware off, GFX domain off
 *   cat /sys/kernel/debug/pvr-apple/selftest             # the last result
 *
 * The jobs are the harness's (tools/gpu/gfx-init.py `--job --store` and
 * `--job --kick-ta --pm --own --tri --clear --kick-3d --va0`), word for
 * word: the same heap layout, page tables, queues, commands and
 * microsequences (devices/a10/docs/gpu.md, G4-G6). The USC code is ours,
 * built by the offline compiler on the Mac into a firmware file
 * (apple/g9p-selftest.bin, tools/gpu/g9p-selftest.py); the PDS programs,
 * the end-of-tile code and every register value are written here.
 */

#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/seq_file.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/unaligned.h>

#include <linux/io.h>

#include "pvr_apple.h"

#define R(n) PVR_APPLE_R_##n
#define VA(n) pvr_apple_va(R(n))
#define PA(n) pvr_apple_pa(a, R(n))

#define GVA	0x200000ULL	/* GPU VA of region GV */
#define G3VA	0x400000ULL	/* GPU VA of region G3 */
#define RGN_VA	0x300000ULL	/* region headers (PMB) */
#define TPC_VA	0x340000ULL	/* tail pointer cache (PMB) */

/* data masters: the kick's low bits and the work-ring entry's +0xc */
#define DM_TA	0
#define DM_3D	1
#define DM_CP	2

static void w32(struct pvr_apple *a, u32 region, u32 off, u32 val)
{
	pvr_apple_w32(a, region, off, val);
}

static void w64(struct pvr_apple *a, u32 region, u32 off, u64 val)
{
	pvr_apple_w64(a, region, off, val);
}

static u32 r32(struct pvr_apple *a, u32 region, u32 off)
{
	return pvr_apple_r32(a, region, off);
}

static void wbytes(struct pvr_apple *a, u32 region, u32 off, const void *src, size_t n)
{
	memcpy(pvr_apple_ptr(a, region, off), src, n);
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

/* -- queues -------------------------------------------------------------- */

/*
 * One queue per data master: a work-ring entry on channel ch {+8 queue,
 * +0xc data master, +0x10 u16 the queue's command write index}, the queue
 * object {+0 state, +4 command ring, +0xc scheduling entries, +0x44 unit
 * mask, +0x88 status} and one command (docs "G4 progress").
 */
static void queue(struct pvr_apple *a, u32 ch, u32 dm, u32 q, u32 qs, u32 cr,
		  u32 se, u32 st, u32 cmd)
{
	u32 ring = 0x1400 * ch;

	w32(a, R(W), ring + 0x0, 0);
	w32(a, R(W), ring + 0x4, 0);
	w32(a, R(W), ring + 0x8, pvr_apple_va(q));
	w32(a, R(W), ring + 0xc, dm);
	w32(a, R(W), ring + 0x10, 1);			/* u16 index 1, priority 0, flags 0 */
	w32(a, q, 0x00, pvr_apple_va(qs));
	w32(a, q, 0x04, pvr_apple_va(cr));
	w32(a, q, 0x0c, pvr_apple_va(se));
	w32(a, q, 0x44, 1);
	w32(a, q, 0x88, pvr_apple_va(st));
	w32(a, qs, 0x50, 256);
	w32(a, cr, 0x00, pvr_apple_va(cmd));
}

/*
 * The host write index of channel ch, set last, right before the doorbell:
 * the firmware scans every channel whenever it wakes and would read a
 * half-written command (G6 3D run 2).
 */
static int kick(struct pvr_apple *a, u32 ch, u32 dm)
{
	w32(a, R(G), 0x10 * ch + 4, 1);
	dma_wmb();
	return pvr_apple_kick(a, dm);
}

static int wait_word(struct pvr_apple *a, u32 region, u32 off, u32 want, unsigned int ms)
{
	u32 v;

	return read_poll_timeout(r32, v, v == want || a->crashed, 1000, ms * 1000, false,
				 a, region, off) ?: (a->crashed ? -EIO : 0);
}

/*
 * FinalizeCompute's path. Its full path stops the CDM and flushes the GPU's
 * caches (RGX 0x11f00, 0x688, 0x1720, 0xe230); the "already finished" fast
 * path (cp_fast=1, what the harness used) skips that. USC stores sit in a
 * cache and reach the MMU only when it is flushed: with the firmware
 * owning power, its flush before each idle power-off wrote them out; with
 * the host holding the power state nothing does, the output stays
 * untouched and even an unmapped buffer raises no fault. So: the full path.
 */
static bool cp_fast;
module_param(cp_fast, bool, 0644);

/* -- the compute job (G4 / G5) ------------------------------------------- */

/*
 * Context 0's page tables (Rogue MMU, 4 KiB pages: PC u32 (PD >> 12) << 4
 * | 1 at VA[39:30], PD u64 PT | 1 at VA[29:21], PT u64 page | 1 at
 * VA[20:12]): GPU VA 0x200000.. onto GV; the CDM arguments, context state
 * and the microsequence StartCompute -> WaitIRQ(COMPUTE_FINISHED) ->
 * FinalizeCompute -> End; the command's completion record. gfx-init.py
 * build_job().
 */
static void build_job(struct pvr_apple *a)
{
	int i;

	w32(a, R(Z), 4 * (GVA >> 30), ((PA(PDR) >> 12) << 4) | 1);
	w64(a, R(PDR), 8 * ((GVA >> 21) & 0x1ff), PA(PTR) | 1);
	for (i = 0; i < 4; i++)
		w64(a, R(PTR), 8 * (((GVA >> 12) & 0x1ff) + i), (PA(GV) + 0x1000 * i) | 1);

	w32(a, R(GV), 0x1000, 0xd3000000);		/* PDS HALT */
	w32(a, R(GV), 0x0, 0x80000000);			/* CDM stream: terminate */
	/* CDM arguments: [0..1] RGX+0x498, [2..3] +0x4b8, [4..5] +0x480 (stream) */
	w32(a, R(CA), 0x00, GVA + 0x2000);
	w32(a, R(CA), 0x08, GVA + 0x1000);
	w32(a, R(CA), 0x0c, GVA + 0x1010);
	w32(a, R(CA), 0x10, GVA);
	for (i = 0; i < 6; i++)				/* six slots via RGX+0x11040 */
		w32(a, R(CA), 0x18 + 8 * i, GVA + 0x3000 + 0x100 * i);
	w32(a, R(CTX), 0x38, GVA + 0x1000);
	w32(a, R(CTX), 0x3c, GVA + 0x1010);

	/* StartCompute (0x4c bytes) */
	w32(a, R(US), 0x00, 0xb);
	w32(a, R(US), 0x04, VA(CDR));
	w32(a, R(US), 0x08, VA(CA));
	w32(a, R(US), 0x0c, VA(UT));
	w32(a, R(US), 0x2c, VA(CTX));
	w32(a, R(US), 0x3c, VA(FL));			/* cancelled flag */
	w32(a, R(US), 0x48, VA(FL) + 0x10);		/* threshold: never cancel */
	w32(a, R(FL), 0x10, 0xffffffff);
	w32(a, R(FL), 0x14, 0xffffffff);
	w32(a, R(US), 0x4c, 4 << 6);			/* WaitIRQ: event bit 2 */
	/* FinalizeCompute (0x34 bytes on completion) */
	w32(a, R(US), 0x50, 0xc);
	w32(a, R(US), 0x54, VA(UT));
	w32(a, R(US), 0x70, VA(STAMP));			/* stamp: STAMP+0 = 1 */
	w32(a, R(US), 0x74, 1);
	w32(a, R(US), 0x78, 0x34);
	w32(a, R(US), 0x7c, VA(FL) + 0x20);		/* "already finished" flag */
	w32(a, R(FL), 0x20, cp_fast);
	w32(a, R(US), 0x80, VA(FL) + 0x30);
	w32(a, R(US), 0x84, 2);				/* End */

	w32(a, R(CMD), 0x000, 3);
	w32(a, R(CMD), 0x014, VA(CTX));
	w32(a, R(CMD), 0x1e8, VA(US));
	w32(a, R(CMD), 0x1ec, 0x88);
	w32(a, R(CMD), 0x298, VA(STAMP) + 0x100);
	w32(a, R(CMD), 0x29c, VA(STAMP) + 0x200);
	w32(a, R(CMD), 0x26c, VA(STAMP) + 0x10);	/* completion record */
	w32(a, R(CMD), 0x274, 0xc0de);
}

/*
 * G5: store_const writes the buffer, laid out as iPadOS dispatches it
 * (docs "G5 ground truth"); PDS base = USC base = GVA, every PDS / USC
 * address an offset from it. gfx-init.py build_store().
 *   +0x0000 CDM stream: DMA record, kernel record (8 words each), terminate
 *   +0x1000 HALT (terminate / context-store PDS)
 *   +0x1100 DMA PDS program, +0x1180 its data; +0x1200 kernel PDS, +0x1280 data
 *   +0x1400 store_const; +0x1600 the argument entry; +0x2800 the output
 */
/*
 * store_probe: point one piece of the store job at an unmapped offset (PDS
 * base + 0x100000) to see whether the GPU fetches it (an MMU fault in RGX
 * 0xe150): 1 the kernel's PDS code, 2 the kernel's USC code (DOUTU), 3 the
 * DMA task's PDS code, 4 the buffer (the store's write).
 */
static int store_probe;
module_param(store_probe, int, 0644);


static int build_store(struct pvr_apple *a)
{
	static const u32 dma_pds[] = { 0xf0001000, 0xf4001808, 0xd3000000 };
	static const u32 ker_pds[] = { 0xc2c00003, 0xf0000a01, 0xd3000000,
				       0xf0000e01, 0xfc000002, 0xd3000000 };
	const u32 rec[] = {
		0x4, 0x1180 | 0x2, 0x1100, 0, 0, 0, 0x40000001, 0,	/* DMA, usc_common_shared */
		0x2103, 0x1280, 0x1200, 0, 0, 0, 0x40000001, 0,		/* kernel: 1 thread */
		0x80000000,
	};
	int i, err;

	err = put_shader(a, "store_const", R(GV), 0x1400);
	if (err)
		return err;
	for (i = 0; i < 0x40; i += 4)
		w32(a, R(GV), 0x2800 + i, 0xabadbeef);
	for (i = 0; i < ARRAY_SIZE(rec); i++)
		w32(a, R(GV), 4 * i, rec[i]);
	if (store_probe == 1)
		w32(a, R(GV), 4 * 10, 0x100000);
	if (store_probe == 3)
		w32(a, R(GV), 4 * 2, 0x100000);
	wbytes(a, R(GV), 0x1100, dma_pds, sizeof(dma_pds));
	w64(a, R(GV), 0x1180, GVA + 0x1600);
	w64(a, R(GV), 0x1188, GVA + 0x1610);
	w64(a, R(GV), 0x1190, 0x10000002);
	w64(a, R(GV), 0x1198, 0x90004003);
	wbytes(a, R(GV), 0x1200, ker_pds, sizeof(ker_pds));
	w32(a, R(GV), 0x1280, store_probe == 2 ? 0x100000 : 0x1400);	/* DOUTU: EXE_OFF, TEMPS 2 */
	w32(a, R(GV), 0x1284, 0x10);
	w32(a, R(GV), 0x1288, 0x90000000);
	w32(a, R(GV), 0x128c, 0x80000000);
	w64(a, R(GV), 0x1600, store_probe == 4 ? 0x5000000 : GVA + 0x2800);	/* {buffer VA, 0, 0x00010001} */
	w32(a, R(GV), 0x1610, 0x00010001);
	w32(a, R(GV), 0x1000, 0xd3000000);
	w32(a, R(CA), 0x08, 0x1000);			/* now offsets from the PDS base */
	w32(a, R(CA), 0x0c, 0x1010);
	w32(a, R(CTX), 0x38, 0x1000);
	w32(a, R(CTX), 0x3c, 0x1010);
	return 0;
}

static int test_store(struct pvr_apple *a, char *res, size_t len, bool fw_power)
{
	const struct pvr_apple_boot_cfg cfg = { .code_base = GVA, .perf_state = 6,
						.fw_power = fw_power };
	u32 out[4];
	int err, i;

	err = pvr_apple_fw_boot(a, &cfg);
	if (err)
		return err;
	/* the job and its page tables before the init data (the GPU may come up with it) */
	queue(a, 2, DM_CP, R(K), R(QS), R(CR), R(SE), R(ST), R(CMD));
	build_job(a);
	err = build_store(a) ?: pvr_apple_fw_init(a, &cfg);
	if (err)
		return err;
	err = kick(a, 2, DM_CP);
	if (err)
		return err;
	err = wait_word(a, R(STAMP), 0x10, 0xc0de, 2000);
	for (i = 0; i < 4; i++)
		out[i] = r32(a, R(GV), 0x2800 + 4 * i);
	pvr_apple_drain_rings(a);
	scnprintf(res, len,
		  "store%s: %s -- STAMP+0 %#x (1), +0x10 %#x (0xc0de); output %08x %08x %08x %08x (cafe0000 abadbeef ...)%s\n",
		  fw_power ? " (firmware-owned power, as in G5)" : "",
		  !err && out[0] == 0xcafe0000 && out[1] == 0xabadbeef ? "PASS" : "FAIL",
		  r32(a, R(STAMP), 0), r32(a, R(STAMP), 0x10), out[0], out[1], out[2], out[3],
		  a->crashed ? "; firmware crashed" : "");
	return err;
}

/* -- the render job (G6) -------------------------------------------------- */

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

static u32 unit_table(u32 dm)
{
	/* the firmware's own per-data-master blocks: P2 + 4 (TA), P3 + 8 (3D) */
	return dm == DM_TA ? VA(P2) + 4 : VA(P3) + 8;
}

/* TA: StartTA -> WaitIRQ(TA_FINISHED) -> FinalizeTA -> End; gfx-init.py build_ta() */
static void build_ta(struct pvr_apple *a)
{
	queue(a, 0, DM_TA, R(KT), R(QST), R(CRT), R(SET), R(STT), R(CMDT));
	w32(a, R(CMDT), 0x000, 0);
	w32(a, R(CMDT), 0x014, VA(CTXT));
	w32(a, R(CMDT), 0x020, VA(T74));
	w32(a, R(CMDT), 0x024, VA(TBIG));
	w32(a, R(GV), 0x3c00, 0xc0000000);
	w64(a, R(TAB), 0x78, GVA + 0x3c00);

	/* StartTA (0x6c bytes) */
	w32(a, R(UST), 0x00, 5);
	w32(a, R(UST), 0x04, VA(TAA));
	w32(a, R(UST), 0x08, VA(TAB));
	w32(a, R(UST), 0x0c, VA(FL2) + 0x30);
	w32(a, R(UST), 0x10, VA(TAO));
	w32(a, R(UST), 0x14, unit_table(DM_TA));
	w32(a, R(UST), 0x18, 1);
	w32(a, R(UST), 0x1c, VA(FL2));			/* cancelled flag */
	w32(a, R(UST), 0x40, VA(CTX2));			/* context state 0: fresh */
	w32(a, R(UST), 0x64, VA(FL2) + 0x10);		/* threshold */
	w32(a, R(FL2), 0x10, 0xffffffff);
	w32(a, R(FL2), 0x14, 0xffffffff);
	w32(a, R(UST), 0x68, 0x20 << 6);		/* (inside StartTA: unused) */
	w32(a, R(UST), 0x6c, 0x20 << 6);		/* WaitIRQ: TA_FINISHED */
	/* FinalizeTA */
	w32(a, R(UST), 0x70, 6);
	w32(a, R(UST), 0x74, VA(TAO));
	w32(a, R(UST), 0x78, VA(TAC));
	w32(a, R(UST), 0x7c, unit_table(DM_TA));
	w32(a, R(UST), 0x84, VA(FL2) + 0x20);		/* "already finished" */
	w32(a, R(FL2), 0x20, 1);
	w32(a, R(UST), 0xa0, VA(STAMP) + 0x20);		/* stamp: STAMP+0x20 = 1 */
	w32(a, R(UST), 0xa4, 1);
	w32(a, R(UST), 0xb0, 2);			/* End */
	w32(a, R(CMDT), 0x464, VA(UST));
	w32(a, R(CMDT), 0x468, 0xb4);
	w32(a, R(CMDT), 0x554, VA(STAMP) + 0x30);	/* completion record */
	w32(a, R(CMDT), 0x55c, 0x7a);
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

	/* free list: 128 physical pages, top = n - 1 (descriptor T74 -> RGX 0x210..) */
	for (i = 0; i < PM_PAGES; i++)
		w32(a, R(G3), G3_FL + 4 * i, (PA(G3) + G3_PAGES + 0x1000 * i) >> 12);
	w64(a, R(T74), 0x1c, G3VA + G3_FL);
	w32(a, R(T74), 0x28, 0);
	w32(a, R(T74), 0x40, PM_PAGES - 1);
	/* the PM slot: MLIST, macrotile array, VHEAP table */
	w64(a, R(TAO), 0xa0, G3VA + G3_MLIST);
	w64(a, R(TAO), 0xa8, G3VA + G3_MTA);
	w64(a, R(TAO), 0xb0, G3VA + G3_VHEAP);
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

/* 3D: Start3D -> WaitIRQ(PIXELBE_END_RENDER) -> Finalize3D -> End; gfx-init.py build_3d() */
static void build_3d(struct pvr_apple *a)
{
	u32 s3a[18] = { 0 };
	u64 s3b[29] = { 0 };
	int i;

	queue(a, 1, DM_3D, R(K3), R(QS3), R(CR3), R(SE3), R(ST3), R(CMD3));
	w32(a, R(TAO), 0xc0, VA(PMH));			/* the PM slot's header */
	w32(a, R(T74), 0x50, VA(T74) + 0x80);
	w32(a, R(CMD3), 0x000, 1);
	w32(a, R(CMD3), 0x01c, VA(CTX3));
	w32(a, R(CMD3), 0x020, VA(T74));		/* the TA's free list */
	w32(a, R(CMD3), 0x024, VA(TAO));		/* the TA's PM slot */
	w32(a, R(CMD3), 0x89c, VA(STAMP) + 0x50);	/* completion record */
	w32(a, R(CMD3), 0x8a4, 0x3d);

	s3a[0] = (MX << 16) | MY;			/* 0xf18 ISP_MTILE_SIZE */
	s3a[1] = 0x3f800000;				/* 0xf80 */
	s3a[2] = 0x300;					/* 0xf88 */
	s3a[3] = 0xf;
	s3a[4] = 0x1f;					/* 0x6d0 */
	s3a[5] = PDS_EOT;				/* 0x618 end-of-tile PDS code */
	s3a[6] = PDS_EOT_DATA;				/* 0x620 its data */
	s3a[7] = 0x402;					/* 0x628 info */
	s3a[8] = 0x0023000f;				/* 0xf38 ISP_CTL */
	s3a[9] = 0x408;					/* 0x3e00 */
	s3a[10] = 4;					/* 0xfd8 */
	s3a[11] = 0xf9;					/* 0x4070 */
	s3b[0] = 0x88;					/* 0xfa8 */
	s3b[10] = MX * MY * 5 / 4;			/* 0xf28 ISP_RGN */
	s3b[11] = ((u64)PDS_BG_TEXU << 32) | PDS_BG;	/* 0x6a0 PDS_BGRND0 */
	s3b[13] = PDS_BG_UNIF;				/* 0x6b0 PDS_BGRND2 */
	s3b[14] = 0x0080000000800000ULL;		/* 0x6b8 PDS_BGRND3: sizes */
	for (i = 22; i < 28; i++)
		s3b[i] = 0x1d3;				/* 0x11080 x 6 */
	for (i = 0; i < ARRAY_SIZE(s3a); i++)
		w32(a, R(S3A), 4 * i, s3a[i]);
	for (i = 0; i < ARRAY_SIZE(s3b); i++)
		w64(a, R(S3B), 8 * i, s3b[i]);

	/* Start3D (0x78 bytes) */
	w32(a, R(US3), 0x00, 7);
	w32(a, R(US3), 0x04, VA(S3A));
	w32(a, R(US3), 0x08, VA(S3B));
	w32(a, R(US3), 0x0c, VA(TAO));			/* PM slot */
	w32(a, R(US3), 0x10, unit_table(DM_3D));
	w32(a, R(US3), 0x14, VA(FL3) + 0x40);		/* -> 0: no partial render */
	w32(a, R(US3), 0x18, VA(FL3) + 0x44);
	w32(a, R(US3), 0x1c, VA(FL3));			/* cancelled flag */
	w32(a, R(US3), 0x20, 1);
	w32(a, R(US3), 0x24, VA(CMD3));
	w32(a, R(US3), 0x28, 0);
	w32(a, R(US3), 0x34, 0);			/* free-list id */
	w32(a, R(US3), 0x48, VA(FL3) + 0x48);		/* -> 0: no resume */
	w32(a, R(US3), 0x74, VA(FL3) + 0x10);		/* threshold */
	w32(a, R(FL3), 0x10, 0xffffffff);
	w32(a, R(FL3), 0x14, 0xffffffff);
	w32(a, R(US3), 0x78, 8 << 6);			/* WaitIRQ: PIXELBE_END_RENDER */
	/* Finalize3D (0x50 bytes) */
	w32(a, R(US3), 0x7c, 8);
	w32(a, R(US3), 0x88, VA(STAMP) + 0x40);		/* stamp: STAMP+0x40 = 1 */
	w32(a, R(US3), 0x8c, 1);
	w32(a, R(US3), 0x90, VA(TAO));
	w32(a, R(US3), 0x94, VA(F3C));
	w32(a, R(US3), 0x98, 1);
	w32(a, R(US3), 0x9c, unit_table(DM_3D));
	w32(a, R(US3), 0xa0, VA(FL3) + 0x20);		/* "already finished" */
	w32(a, R(FL3), 0x20, 1);
	w32(a, R(US3), 0xac, VA(CMD3));
	w32(a, R(US3), 0xb4, VA(FL3) + 0x50);
	w32(a, R(US3), 0xc4, 0x50);
	w32(a, R(US3), 0xc8, VA(FL3) + 0x60);
	w32(a, R(US3), 0xcc, 2);			/* End */
	w32(a, R(CMD3), 0x014, VA(US3));
	w32(a, R(CMD3), 0x018, 0xd0);
}

static int test_render(struct pvr_apple *a, char *res, size_t len)
{
	const struct pvr_apple_boot_cfg cfg = { .code_base = G3VA, .perf_state = 6 };
	u32 bg = 0, tri = 0, mark = 0, other = 0, first_other = 0, i;
	int err, err_ta, err_3d;

	err = pvr_apple_fw_boot(a, &cfg) ?: pvr_apple_fw_init(a, &cfg);
	if (err)
		return err;
	build_job(a);				/* page tables, the flush fence's page */
	build_ta(a);
	err = build_scene(a);
	if (err)
		return err;
	err = kick(a, 0, DM_TA);
	if (err)
		return err;
	err_ta = wait_word(a, R(STAMP), 0x30, 0x7a, 2000);

	build_3d(a);
	pvr_apple_drain_rings(a);		/* room for the 3D's events */
	err = kick(a, 1, DM_3D);
	if (err)
		return err;
	err_3d = wait_word(a, R(STAMP), 0x50, 0x3d, 3000);

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
	pvr_apple_drain_rings(a);
	scnprintf(res, len,
		  "render: %s -- TA STAMP+0x20 %#x +0x30 %#x (1 0x7a), 3D +0x40 %#x +0x50 %#x (1 0x3d); "
		  "pixels: %u background %08x (3072), %u triangle %08x (1024), %u untouched, %u other (first %08x); "
		  "px(0,0) %08x px(0,63) %08x%s\n",
		  !err_ta && !err_3d && bg == 3072 && tri == 1024 ? "PASS" : "FAIL",
		  r32(a, R(STAMP), 0x20), r32(a, R(STAMP), 0x30),
		  r32(a, R(STAMP), 0x40), r32(a, R(STAMP), 0x50),
		  bg, BG_COLOUR, tri, TRI_COLOUR, mark, other, first_other,
		  r32(a, R(G3), G3_RT), r32(a, R(G3), G3_RT + 4 * RT_W * (RT_H - 1)),
		  a->crashed ? "; firmware crashed" : "");
	return err_ta ?: err_3d;
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
	char cmd[16] = { 0 }, res[sizeof(a->result)] = { 0 };
	int err;

	if (copy_from_user(cmd, ubuf, min(len, sizeof(cmd) - 1)))
		return -EFAULT;
	strim(cmd);

	mutex_lock(&a->lock);
	if (!strcmp(cmd, "store") || !strcmp(cmd, "store-fw")) {
		/* store: the host holds the power state (perf 6); store-fw: G5's way */
		err = test_store(a, res, sizeof(res), !strcmp(cmd, "store-fw"));
	} else if (!strcmp(cmd, "render")) {
		err = test_render(a, res, sizeof(res));
	} else if (!strcmp(cmd, "boot")) {
		const struct pvr_apple_boot_cfg cfg = { .code_base = G3VA, .perf_state = 6 };

		err = pvr_apple_fw_boot(a, &cfg) ?: pvr_apple_fw_init(a, &cfg);
		scnprintf(res, sizeof(res), "boot: %s (%d)\n", err ? "FAIL" : "ok", err);
	} else if (!strcmp(cmd, "stop")) {
		pvr_apple_fw_stop(a);
		err = 0;
		scnprintf(res, sizeof(res), "stopped\n");
	} else {
		mutex_unlock(&a->lock);
		return -EINVAL;
	}
	if (!res[0])
		scnprintf(res, sizeof(res), "%s: error %d%s\n", cmd, err,
			  a->crashed ? " (firmware crashed)" : "");
	if (strcmp(cmd, "stop"))
		rgx_dump(a, res, sizeof(res));
	scnprintf(a->result, sizeof(a->result),
		  "%sfirmware ring messages %u; job events: 0x1 x%u, 0x4 (fault) x%u; power / trace ring entries %u / %u\n",
		  res, a->ring_msgs, a->ev_count[0][1], a->ev_count[0][4],
		  a->ev_other[1] + a->ev_count[1][0] + a->ev_count[1][1],
		  a->ev_other[2] + a->ev_count[2][0] + a->ev_count[2][1]);
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
	scnprintf(a->result, sizeof(a->result), "no self-test run yet: echo store|render > selftest\n");
}
