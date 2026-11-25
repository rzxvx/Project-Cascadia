/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * Apple A10 (T8010, "G9P") GPU: a PowerVR Rogue core driven by Apple's GFX
 * firmware (RTKit on the gfx-kf coprocessor) instead of Imagination's.
 *
 * This is the firmware layer of the powervr driver's Apple backend: power,
 * the firmware image, RTKit over the A7-style IOP mailbox, the init data the
 * firmware takes on endpoint 0x20, its channels and rings, and kicks on
 * endpoint 0x21. Every structure offset here was found in the 22H355
 * firmware and checked on an iPad 7 (Cascadia, devices/a10/docs/gpu.md;
 * the Python harness tools/gpu/gfx-init.py is the executable reference).
 */
#ifndef PVR_APPLE_H
#define PVR_APPLE_H

#include <linux/errno.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/workqueue.h>

struct device;
struct dentry;
struct firmware;
struct apple_rtkit;
struct pvr_device;

#define PVR_APPLE_FW_SIZE	0xa4000		/* the 22H355 image, load base 0 */
#define PVR_APPLE_RUNTIME	0xd8000		/* its RTSZ: image + zeroed BSS / heap */

/* Endpoints and messages (bits 55:0 of a mailbox word; the endpoint is apart) */
#define PVR_APPLE_EP_INIT	0x20
#define PVR_APPLE_EP_DOORBELL	0x21
#define PVR_APPLE_MSG_INIT	(0x81ULL << 48)		/* | PA of the init page */
#define PVR_APPLE_MSG_KICK	(3ULL << 48)		/* | data master (0 TA, 1 3D, 2 CP) */
#define PVR_APPLE_MSG_RINGS	0x42			/* payload >> 48 on EP 0x20: drain the rings */

/* Fender (sgx reg 1) */
#define FENDER_PERF_STATE	0x1000		/* 0x10 | state to request; reads back the state */
#define FENDER_WIN_PA_LO	0x200c
#define FENDER_WIN_PA_HI	0x2010
#define FENDER_WIN_IOP_LO	0x2014
#define FENDER_WIN_IOP_HI	0x2018
#define FENDER_WIN_SIZE		0x201c
#define FENDER_WIN_SIZE_HI	0x2020
#define FENDER_RUN		0x2048		/* bit 0: the coprocessor runs */

/* gfx-kf, the A7-style IOP (v2) */
#define IOP_WIN_PA_LO		0x08
#define IOP_WIN_PA_HI		0x10
#define IOP_WIN_IOP_LO		0x18
#define IOP_WIN_IOP_HI		0x20
#define IOP_WIN_SIZE		0x28
#define IOP_WIN_SIZE_HI		0x30
#define IOP_WIN_ENABLE		0x38
#define IOP_TIME_CONTROL	0x80c		/* bit 1: timebase running */
#define IOP_TIME_LO		0xc030
#define IOP_TIME_HI		0xc038

/*
 * One heap of firmware structures and GPU memory, mapped by memory
 * descriptor 0 at firmware VA 0x80400000 (window 2: a host mapping below
 * 32 MiB hangs the firmware on its first access). The regions and their
 * offsets are the harness's (gfx-init.py REGIONS), kept as they are so the
 * two can be compared word for word.
 */
#define PVR_APPLE_HEAP_VA	0x80400000u
#define PVR_APPLE_HEAP_SIZE	0x400000u

#define PVR_APPLE_REGIONS(R)					\
	R(X, 0x00000, 0x1000)	/* the root: X+0 = P */		\
	R(P, 0x10000, 0x4000)					\
	R(T, 0x20000, 0x1000)					\
	R(B, 0x30000, 0x8000)					\
	R(A, 0x40000, 0x1000)					\
	R(C, 0x41000, 0x1000)					\
	R(D, 0x42000, 0x1000)					\
	R(S, 0x43000, 0x1000)	/* FenderScratchRam stand-in */	\
	R(E, 0x44000, 0x1000)	/* X+0x4: the flush fence VA */	\
	R(F, 0x45000, 0x1000)					\
	R(G, 0x46000, 0x1000)	/* channel ring indices */	\
	R(H, 0x48000, 0x8000)	/* X+0x311: power struct */	\
	R(W, 0xa0000, 0x12000)	/* 13 work rings */		\
	R(K, 0xb2000, 0x1000)	/* compute queue object */	\
	R(L, 0xb3000, 0x1000)					\
	R(M, 0xb4000, 0x1000)					\
	R(N, 0xb5000, 0x4000)					\
	R(Y, 0xb9000, 0x1000)	/* MMU roots */			\
	R(GEN, 0xba000, 0x1000)					\
	R(XG, 0xbb000, 0x1000)					\
	R(Z, 0xbc000, 0x4000)	/* context 0's page catalog */	\
	R(QS, 0xc0000, 0x1000)					\
	R(CR, 0xc1000, 0x1000)					\
	R(SE, 0xc2000, 0x4000)					\
	R(ST, 0xc6000, 0x1000)					\
	R(CMD, 0xc7000, 0x1000)					\
	R(US, 0xc8000, 0x1000)					\
	R(CTX, 0xc9000, 0x1000)					\
	R(CA, 0xca000, 0x1000)					\
	R(STAMP, 0xcb000, 0x1000)				\
	R(P2, 0xcc000, 0x1000)					\
	R(P3, 0xcd000, 0x1000)					\
	R(P4, 0xce000, 0x1000)					\
	R(P5, 0xcf000, 0x1000)					\
	R(PDR, 0xd0000, 0x1000)					\
	R(PTR, 0xd1000, 0x1000)					\
	R(GV, 0xd4000, 0x4000)	/* GPU VA 0x200000.. */		\
	R(CDR, 0xd8000, 0x1000)					\
	R(UT, 0xd9000, 0x1000)					\
	R(FL, 0xda000, 0x1000)					\
	R(EC0, 0xdb000, 0x100)	/* fw -> host ring controls */	\
	R(EC1, 0xdb100, 0x100)					\
	R(EC2, 0xdb200, 0x100)					\
	R(EV0, 0xdc000, 0x4000)	/* job events */		\
	R(EV1, 0xe0000, 0x4000)	/* power events */		\
	R(EV2, 0xe4000, 0x8000)	/* trace */			\
	R(TS, 0xec000, 0x4000)	/* TempSensorRegister stand-in */ \
	R(KT, 0x100000, 0x1000)	/* TA queue */			\
	R(QST, 0x101000, 0x1000)				\
	R(CRT, 0x102000, 0x1000)				\
	R(SET, 0x103000, 0x4000)				\
	R(STT, 0x107000, 0x1000)				\
	R(CMDT, 0x108000, 0x1000)				\
	R(UST, 0x109000, 0x1000)				\
	R(CTXT, 0x10a000, 0x1000)				\
	R(T74, 0x10b000, 0x1000)	/* free-list descriptor */	\
	R(TBIG, 0x10c000, 0x4000)				\
	R(TAA, 0x110000, 0x1000)				\
	R(TAB, 0x111000, 0x1000)				\
	R(TAO, 0x112000, 0x1000)	/* the PM slot */	\
	R(FL2, 0x114000, 0x1000)				\
	R(TAC, 0x115000, 0x1000)				\
	R(CTX2, 0x116000, 0x1000)				\
	R(PMB, 0x120000, 0xe0000)	/* GPU VA 0x300000.. */	\
	R(K3, 0x200000, 0x1000)	/* 3D queue */			\
	R(QS3, 0x201000, 0x1000)				\
	R(CR3, 0x202000, 0x1000)				\
	R(SE3, 0x203000, 0x4000)				\
	R(ST3, 0x207000, 0x1000)				\
	R(CMD3, 0x208000, 0x1000)				\
	R(US3, 0x209000, 0x1000)				\
	R(CTX3, 0x20a000, 0x1000)				\
	R(S3A, 0x20b000, 0x1000)				\
	R(S3B, 0x20c000, 0x1000)				\
	R(FL3, 0x20e000, 0x1000)				\
	R(F3C, 0x20f000, 0x1000)				\
	R(PMH, 0x211000, 0x1000)				\
	R(PTR2, 0x212000, 0x1000)	/* GPU VA 0x400000.. */	\
	R(PTR0, 0x213000, 0x1000)	/* GPU VA 0.. */	\
	R(VA0, 0x214000, 0x1000)				\
	R(G3, 0x240000, 0x100000)	/* GPU VA 0x400000.. */

#define PVR_APPLE_REGION_ENUM(n, o, s) PVR_APPLE_R_##n,
enum pvr_apple_region { PVR_APPLE_REGIONS(PVR_APPLE_REGION_ENUM) PVR_APPLE_R_COUNT };
#undef PVR_APPLE_REGION_ENUM

extern const u32 pvr_apple_region_off[PVR_APPLE_R_COUNT];

/* firmware -> host rings: {control region, entries region, entry count} */
#define PVR_APPLE_RING_ENTRY	0x3c
#define PVR_APPLE_NR_RINGS	3

struct pvr_apple {
	struct pvr_device *pvr_dev;
	struct device *dev;

	void __iomem *fender;
	void __iomem *iop;
	void __iomem *rgx;			/* read only through pvr_apple_rgx_read() */
	phys_addr_t rgx_pa, fender_pa;

	const struct firmware *fw;
	const struct firmware *shaders;		/* the self-test's USC code */

	/* one boot of the firmware */
	struct mutex lock;
	bool powered;
	struct apple_rtkit *rtk;
	void *rt;
	dma_addr_t rt_dma;
	void *heap;
	dma_addr_t heap_dma;
	void *page;				/* init page; +0x1000 the init sequence */
	dma_addr_t page_dma;
	void *desc;				/* memory descriptors */
	dma_addr_t desc_dma;
	bool crashed;
	u32 held_state;				/* the Fender perf state the host holds */
	void *standin;				/* rgx_standin=1: RAM in place of RGX */
	dma_addr_t standin_dma;
	struct delayed_work poll_work;		/* mbox_poll=1: no receive interrupt */

	/* ring events, counted per (ring, type) for the logs */
	spinlock_t ring_lock;
	u32 ring_msgs;
	u32 ev_count[PVR_APPLE_NR_RINGS][8];
	u32 ev_other[PVR_APPLE_NR_RINGS];

	struct dentry *debugfs;
	char result[1024];
};

/* boot parameters that differ between jobs (one firmware boot each) */
struct pvr_apple_boot_cfg {
	u64 code_base;		/* P+0x10 / P+0x18: PDS and USC execution base */
	u32 perf_state;		/* Fender perf state the host holds (1..6) */
	bool fw_power;		/* the firmware powers the GPU (P+0xd34 = 0xd3c = 1), as in G4 / G5 */
};

static inline u32 pvr_apple_va(u32 region)
{
	return PVR_APPLE_HEAP_VA + pvr_apple_region_off[region];
}

static inline u64 pvr_apple_pa(struct pvr_apple *a, u32 region)
{
	return a->heap_dma + pvr_apple_region_off[region];
}

static inline void *pvr_apple_ptr(struct pvr_apple *a, u32 region, u32 off)
{
	return a->heap + pvr_apple_region_off[region] + off;
}

void pvr_apple_w32(struct pvr_apple *a, u32 region, u32 off, u32 val);
void pvr_apple_w64(struct pvr_apple *a, u32 region, u32 off, u64 val);
u32 pvr_apple_r32(struct pvr_apple *a, u32 region, u32 off);

int pvr_apple_fw_boot(struct pvr_apple *a, const struct pvr_apple_boot_cfg *cfg);
int pvr_apple_fw_init(struct pvr_apple *a, const struct pvr_apple_boot_cfg *cfg);
void pvr_apple_fw_stop(struct pvr_apple *a);
int pvr_apple_kick(struct pvr_apple *a, u32 dm);
void pvr_apple_drain_rings(struct pvr_apple *a);
int pvr_apple_rgx_read(struct pvr_apple *a, u32 off, u32 *val);

void pvr_apple_debugfs_init(struct pvr_apple *a);

#if IS_ENABLED(CONFIG_DRM_POWERVR_APPLE)
bool pvr_apple_match(struct device *dev);
int pvr_apple_probe(struct pvr_device *pvr_dev);
void pvr_apple_remove(struct pvr_device *pvr_dev);
#else
static inline bool pvr_apple_match(struct device *dev) { return false; }
static inline int pvr_apple_probe(struct pvr_device *pvr_dev) { return -ENODEV; }
static inline void pvr_apple_remove(struct pvr_device *pvr_dev) { }
#endif

#endif /* PVR_APPLE_H */
