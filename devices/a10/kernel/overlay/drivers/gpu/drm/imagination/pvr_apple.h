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

#include <linux/bitmap.h>
#include <linux/errno.h>
#include <linux/list.h>
#include <linux/sizes.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/workqueue.h>

struct device;
struct dentry;
struct firmware;
struct apple_rtkit;
struct pvr_device;
struct pvr_apple_test;
struct dma_fence;
struct pvr_vm_context;
struct pvr_gem_object;

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
 * One heap of firmware structures, mapped by memory descriptor 0 at
 * firmware VA 0x80400000 (window 2, 0x80000000 + 8 MiB; a host mapping
 * below 32 MiB hangs the firmware on its first access). The first MiB holds
 * the static structures at the offsets the harness used (gfx-init.py
 * REGIONS); the rest is a pool for queues, commands and render objects.
 * GPU memory lives elsewhere: the firmware never maps it.
 */
#define PVR_APPLE_HEAP_VA	0x80400000u
#define PVR_APPLE_HEAP_SIZE	0x400000u
#define PVR_APPLE_POOL_OFF	0x100000u
#define PVR_APPLE_POOL_UNIT	0x1000u
/*
 * The fence page every VM maps at the same VA: the flush in FinalizeCompute's
 * full path and before a power-off writes to the GPU VA in X+0x4 (RGX+0xe240)
 * in the context running then. The last 16 KiB of the UAPI's general heap
 * (not reported to userspace: step 3).
 */
#define PVR_APPLE_FENCE_VA	(0x8000000000ULL + SZ_128G - SZ_16K)
#define PVR_APPLE_RESERVED_SIZE	SZ_16K

/* the UAPI's execution bases (pvr_rogue_heap_config.h): PDS / USC code heaps */
#define PVR_APPLE_PDS_BASE	0xDA00000000ULL
#define PVR_APPLE_USC_BASE	0xE000000000ULL

/* a range userspace may not map (the fence page) */
static inline bool pvr_apple_va_reserved(u64 addr, u64 size)
{
	return addr < PVR_APPLE_FENCE_VA + PVR_APPLE_RESERVED_SIZE &&
	       addr + size > PVR_APPLE_FENCE_VA;
}

#define PVR_APPLE_POOL_UNITS	((PVR_APPLE_HEAP_SIZE - PVR_APPLE_POOL_OFF) / PVR_APPLE_POOL_UNIT)

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
	R(L, 0xb3000, 0x1000)	/* the mapping ring */		\
	R(M, 0xb4000, 0x1000)					\
	R(N, 0xb5000, 0x4000)					\
	R(Y, 0xb9000, 0x1000)	/* MMU roots: PC PA per context */ \
	R(GEN, 0xba000, 0x1000)					\
	R(XG, 0xbb000, 0x1000)					\
	R(P2, 0xcc000, 0x1000)	/* per-data-master blocks */	\
	R(P3, 0xcd000, 0x1000)					\
	R(P4, 0xce000, 0x1000)					\
	R(P5, 0xcf000, 0x1000)					\
	R(UT, 0xd9000, 0x1000)	/* compute unit table */	\
	R(EC0, 0xdb000, 0x100)	/* fw -> host ring controls */	\
	R(EC1, 0xdb100, 0x100)					\
	R(EC2, 0xdb200, 0x100)					\
	R(EV0, 0xdc000, 0x4000)	/* job events */		\
	R(EV1, 0xe0000, 0x4000)	/* power events */		\
	R(EV2, 0xe4000, 0x8000)	/* trace */			\
	R(TS, 0xec000, 0x4000)	/* TempSensorRegister stand-in */

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

	/* the pool (heap + PVR_APPLE_POOL_OFF), in 4 KiB units */
	spinlock_t pool_lock;
	DECLARE_BITMAP(pool_map, PVR_APPLE_POOL_UNITS);

	/* queues and jobs: job_lock covers the work rings, the queues' lists */
	spinlock_t job_lock;
	u32 ring_wr[13];			/* host write index per channel */
	struct list_head queues;
	struct delayed_work done_work;		/* completions, as a fallback to EP 0x20 */
	spinlock_t fence_lock;			/* every job fence's lock (outlives the queues) */

	/* MMU context 0's catalog page: the active VM's root table (pvr_apple.c) */
	void *active_pc;
	dma_addr_t active_pc_dma;
	struct pvr_vm_context *active_vm;	/* ring_lock; changed only with no job in the firmware */
	struct pvr_vm_context *default_vm;	/* jobs' VM unless the caller sets one */
	struct pvr_gem_object *fence_bo;	/* mapped at PVR_APPLE_FENCE_VA in every VM */

	/* the running firmware (pvr_apple_start): its boot number, the queues */
	bool running;
	u32 boot;
	struct pvr_apple_queue *q[3];		/* TA, 3D, CP: one per data master (G7 step 1) */

	struct dentry *debugfs;
	struct pvr_apple_test *test;
	char result[1024];
};

/* a piece of the pool: CPU pointer and firmware VA */
struct pvr_apple_obj {
	void *cpu;
	u32 va;
	u32 size;
	u32 boot;		/* the firmware boot it belongs to: freed with a restart */
};

/*
 * A firmware queue on one channel (ch = 3 * group + data master): the queue
 * object, its state, command ring, scheduling entries, status, and the word
 * the commands' completion records write their sequence number to.
 */
struct pvr_apple_queue {
	struct pvr_apple *a;
	struct list_head node;
	u32 ch, dm;
	struct pvr_apple_obj obj;
	u32 cmd_idx;				/* commands submitted (the ring's write index) */
	u32 seqno;
	struct list_head pending;		/* submitted, in order; the first may be in flight */
	struct pvr_apple_job *inflight;		/* the one command the firmware holds */
	u64 fence_ctx;				/* one fence timeline per queue */
};

struct pvr_apple_job {
	struct pvr_apple_queue *q;
	struct pvr_apple_obj blk;		/* command, microsequence, their objects */
	u32 rec_addr_off, rec_val_off;		/* the command's completion record */
	u32 seqno;
	struct pvr_vm_context *vm;		/* translates the job's addresses (MMU context 0) */
	struct dma_fence *fence;
	struct list_head node;
};

/* raw register blocks of the Start ops (docs/gpu.md G4 / G6), filled by the caller */
struct pvr_apple_cp_args {
	u32 cdr[2];		/* StartCompute +4: RGX+0x4c0 / +0x4c8 */
	u32 ca[18];		/* StartCompute +8: 0x498, 0x4b8, 0x480 (stream), six 0x11040 slots */
	u32 ctx[36];		/* +0x2c context state: [0xe..0x10] -> 0x4a8 / 0x4ac / 0x4b0 */
	bool fast;		/* FinalizeCompute's "already finished" path (no CDM stop / flush) */
	u8 mmu_ctx;		/* MMU context (StartCompute +0x14 -> RGX+0xf07a) */
};

struct pvr_apple_ta_args {
	u32 a[8];		/* StartTA +4: 0xc78 0xc88 0xc98 0xc18 0xc08 0xc10 0xc20 0xc28 */
	u64 b[28];		/* StartTA +8: 0xc90 0xc38 0xc40 0xc00 0xc80 0x428 0xcc0 ... 0x408 ... */
	u8 mmu_ctx;		/* MMU context (StartTA +0x20 -> RGX+0xf078) */
};

struct pvr_apple_3d_args {
	u32 a[18];		/* Start3D +4: 0xf18 0xf80 0xf88 . 0x6d0 0x618 0x620 0x628 0xf38 ... */
	u64 b[29];		/* Start3D +8: 0xfa8 ... 0xf28 0x6a0 . 0x6b0 0x6b8 ... 0x11080 x 6 */
	u8 mmu_ctx;		/* MMU context (Start3D +0x28 -> RGX+0xf079) */
};

/* a render target's firmware objects: free-list descriptor, PM slot, the TA's large object */
struct pvr_apple_hwrt {
	struct pvr_apple *a;
	struct pvr_apple_obj obj;
	u32 fl_va, slot_va, tbig_va, pmh_va;
};

/* boot parameters */
struct pvr_apple_boot_cfg {
	u64 pds_base;		/* P+0x10 -> RGX+0x610: PDS execution base (2 MiB aligned) */
	u64 usc_base;		/* P+0x18 -> RGX+0x4008..: USC code base */
	struct pvr_vm_context *ctx0_vm;	/* the kernel's VM, mirrored into context 0's catalog page */
	u64 ctx0_pc;		/* else: context 0's page catalog (PA) */
	u64 fence_va;		/* GPU VA (context 0) the flush before a power-off writes to */
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

int pvr_apple_obj_alloc(struct pvr_apple *a, u32 size, struct pvr_apple_obj *o);
void pvr_apple_obj_free(struct pvr_apple *a, struct pvr_apple_obj *o);

struct pvr_apple_queue *pvr_apple_queue_create(struct pvr_apple *a, u32 group, u32 dm);
void pvr_apple_queue_destroy(struct pvr_apple_queue *q);
struct pvr_apple_job *pvr_apple_job_cp(struct pvr_apple_queue *q, const struct pvr_apple_cp_args *args);
struct pvr_apple_job *pvr_apple_job_ta(struct pvr_apple_queue *q, const struct pvr_apple_ta_args *args,
				       struct pvr_apple_hwrt *rt);
struct pvr_apple_job *pvr_apple_job_3d(struct pvr_apple_queue *q, const struct pvr_apple_3d_args *args,
				       struct pvr_apple_hwrt *rt);
void pvr_apple_job_free(struct pvr_apple_job *job);
struct dma_fence *pvr_apple_submit(struct pvr_apple_job *job);
void pvr_apple_process_completions(struct pvr_apple *a);
u32 pvr_apple_queue_word(struct pvr_apple_queue *q, u32 off);
int pvr_apple_queue_debug(struct pvr_apple_queue *q, char *buf, size_t len);
void pvr_apple_fail_all(struct pvr_apple *a, int err);
struct pvr_apple_hwrt *pvr_apple_hwrt_create(struct pvr_apple *a, u64 fl_base, u32 fl_pages,
					     u64 mlist, u64 mta, u64 vheap);
void pvr_apple_hwrt_destroy(struct pvr_apple_hwrt *rt);

void pvr_apple_vm_activate(struct pvr_apple *a, struct pvr_vm_context *vm);

int pvr_apple_start_locked(struct pvr_apple *a);
void pvr_apple_stop_locked(struct pvr_apple *a);

void pvr_apple_debugfs_init(struct pvr_apple *a);
void pvr_apple_test_fini(struct pvr_apple *a);

#if IS_ENABLED(CONFIG_DRM_POWERVR_APPLE)
int pvr_apple_mmu_flush(struct pvr_apple *a);
void pvr_apple_vm_release(struct pvr_apple *a, struct pvr_vm_context *vm);
int pvr_apple_vm_map_fence(struct pvr_apple *a, struct pvr_vm_context *vm);
int pvr_apple_start(struct pvr_apple *a);
bool pvr_apple_match(struct device *dev);
int pvr_apple_probe(struct pvr_device *pvr_dev);
void pvr_apple_remove(struct pvr_device *pvr_dev);
#else
static inline bool pvr_apple_match(struct device *dev) { return false; }
static inline int pvr_apple_probe(struct pvr_device *pvr_dev) { return -ENODEV; }
static inline void pvr_apple_remove(struct pvr_device *pvr_dev) { }
static inline int pvr_apple_start(struct pvr_apple *a) { return -ENODEV; }
static inline int pvr_apple_mmu_flush(struct pvr_apple *a) { return 0; }
static inline int pvr_apple_vm_map_fence(struct pvr_apple *a, struct pvr_vm_context *vm) { return 0; }
static inline void pvr_apple_vm_release(struct pvr_apple *a, struct pvr_vm_context *vm) { }
#endif

#endif /* PVR_APPLE_H */
