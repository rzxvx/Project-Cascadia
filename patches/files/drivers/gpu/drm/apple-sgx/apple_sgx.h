/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Apple S5L8940X (A5) PowerVR SGX543MP2 -- what the hardware side
 * (apple_sgx_hw.c: power, MMU, microkernel, queues, debugfs) and the render
 * node (apple_sgx_drm.c) share.
 */
#ifndef __APPLE_SGX_H__
#define __APPLE_SGX_H__

#include <linux/atomic.h>
#include <linux/mutex.h>
#include <linux/sizes.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/workqueue.h>

struct apple_sgx_drm;
struct clk_bulk_data;
struct dentry;
struct device;
struct drm_device;
struct drm_gem_object;
struct page;
struct resource;

/* The register window: six 16 KiB banks -- bank 0 broadcast (write only:
 * a read hangs the bus), bank 1 the master, bank 2 + n core n. */
#define SGX_BANK_SIZE			0x4000
#define SGX_BCAST			0x0000
#define SGX_MASTER			(1 * SGX_BANK_SIZE)
#define SGX_CORE(n)			((2 + (n)) * SGX_BANK_SIZE)
#define SGX_MAX_CORES			4

/* ---- the GPU's MMU (sgxmmu.h; iOS's PTE writer is 0x80bf4dc4) ---------- */

#define SGX_PAGE_SIZE			0x1000
#define SGX_PD_ENTRIES			1024
#define SGX_PDE_VALID			BIT(0)
#define SGX_PTE_VALID			BIT(0)
#define SGX_PTE_READONLY		BIT(2)
#define SGX_PTE_CACHECONSISTENT		BIT(3)
#define SGX_PTE_EDMPROTECT		BIT(4)

/*
 * The GPU's address space, one for everything:
 *
 *   0x00001000            the 2D engine's code page (USE code base 10 = 0)
 *   0x80000000-0x807fffff the microkernel's buffers and the kernel's queues.
 *                         The microkernel carries no address of its own
 *                         (every one is patched in or handed over in a
 *                         register), so the layout is ours; 0x80000000 is
 *                         where iOS's "GART" starts.  Everything stays within
 *                         8 MiB of the code, because some addresses are
 *                         given as offsets from it in 20-bit fields.
 *   0x80800000-0xefffffff render node buffers (apple_sgx_drm.h), and what
 *                         debugfs "map" puts there
 *     0x90000000-+16 MiB  the framebuffer, for the 2D engine and renders
 *     0x9a000000-+8 MiB   the USSE code zone; USE_CODE_BASE_3 and _5 point
 *                         at its start.  8 MiB is all a DOUTU or a PHAS can
 *                         reach: they name a program by its index from the
 *                         base, 20 bits of 8-byte instructions.  (It was 16
 *                         MiB, handed out top down, and Mesa's first program
 *                         landed out of reach: every render hung.)
 *     0x89000000-+8.3 MiB the kernel's parameter buffer, shared by all
 *     0x8a000000-0x977fffff  the TA's heap (APPLE_SGX_BO_TA_HEAP): render
 *                         target data, within 256 MiB of the TA's base
 *     0xa8000000-         where the kernel picks addresses, top down,
 *                         up to the tiled window
 *     0xe0000000-+256 MiB the BIF's tiled window 1 (stride 4096): only at
 *                         an address asked for
 *   0xf0000000-           the BIF's tiled window 2 (stride 8192), unused
 *
 * The tiled windows are iOS's (BIF_TILE1/2, apple_sgx_hw.c): what the GPU
 * writes through them lands in 256-byte x 16-line tiles, which the CPU,
 * reading the buffer as linear, sees scrambled.  (Until 2026-10-04 the
 * kernel picked addresses from 0xf0000000 down: Mesa's first render
 * targets landed in window 1 and every non-uniform draw came back
 * scrambled; uniform clears hid it.)
 *
 * PDS data pointers carry bit 31 implied, so everything a PDS program reads
 * has to be at 0x80000000 or above.
 */
#define SGX_VA_BASE			0x80000000u
#define SGX_FB_VA			0x90000000u
#define SGX_FB_WINDOW			SZ_16M
#define SGX_USER_VA_START		0x80800000u
#define SGX_USER_VA_END			0xf0000000u
#define SGX_CODE_BASE			0x9a000000u
#define SGX_CODE_VA_END			(SGX_CODE_BASE + SZ_8M)
#define SGX_AUTO_VA_START		0xa8000000u
/* The TA's base (its requests' page numbers count from here), the kernel's
 * parameter buffer, and where TA-heap buffers go: within 256 MiB of the
 * base, clear of the windows of the template frame's pack (0x87b00000 to
 * 0x88851000, sgx2d's).  A parameter buffer at 0x8cc00000-0x8ef00000 hangs
 * renders or makes them come out wrong, every time or now and then, and
 * nowhere else in the heap -- not understood (docs/research/p105-mesa.md,
 * M17); this one is where it works, near iOS's (0x88000000). */
#define SGX_TA_BASE			0x87800000u
#define SGX_PB_VA			0x89000000u
#define SGX_TA_HEAP_START		0x8a000000u
#define SGX_TA_HEAP_END			0x97800000u
#define SGX_TILED_VA_START		0xe0000000u

/* The GPU clock in kHz: GFX-CLK, PLL@0x18 (513 MHz) / 5 in perf state 2. */
#define SGX_CLOCK_KHZ			102600

/* The DDK's SGXMKIF_CC_* (sgx_mkif_km.h): what a kernel CCB command asks
 * the microkernel to invalidate before it runs. */
#define SGX_CC_INVAL_BIF_PT		BIT(0)
#define SGX_CC_INVAL_BIF_PD		BIT(1)
#define SGX_CC_INVAL_BIF_SL		BIT(2)
#define SGX_CC_INVAL_DATA		BIT(3)

/* Where in the scratch buffer the render node's completions land (words
 * 0 and 1 are debugfs's and the 2D engine's). */
#define SGX_SCRATCH_RENDER		0x40

enum sgx_buf_id {
	B_CODE,		/* 0x6d4 */
	B_PDS_6D8, B_PDS_6E0, B_PDS_6E8, B_PDS_6F0, B_PDS_6F8,
	B_CODE2,	/* 0x700 */
	B_PDS_704,
	B_TAB_784, B_TAB_78C,
	B_740, B_HOST, B_750, B_758, B_KICKER, B_768,
	B_TAB_774, B_IDX_77C,
	B_794, B_798, B_79C, B_7A0,
	B_TQ_CTX, B_TQ_CCB, B_TQ_CTL,	/* one transfer queue (IMGSGXTQChannel) */
	B_R_CTX, B_R_CCB, B_R_CTL,	/* one render queue (TA + 3D) */
	B_SCRATCH,			/* where test commands write */
	B_BLT_BLOCK, B_BLT_PARAM,	/* the 2D engine's PDS block and parameter page */
	B_NUM
};

struct sgx_buf {
	u32 *cpu;
	dma_addr_t dma;
	u32 va;
	size_t size;
	struct page **pages;	/* set: allocated page by page (map_extra) */
	dma_addr_t *page_dma;
	struct device *dev;
};

#define SGX_MAX_EXTRA			32

struct apple_sgx {
	struct device *dev;
	void __iomem *regs;
	resource_size_t size;
	struct clk_bulk_data *clks;
	int num_clks;
	unsigned int ncores;
	u32 core_id, core_rev;
	bool brn_31195;
	struct dentry *debugfs;
	struct mutex lock;

	/* the MMU */
	struct mutex mmu_lock;		/* the page tables */
	u32 *pd;
	dma_addr_t pd_dma;
	u32 *pt[SGX_PD_ENTRIES];
	dma_addr_t pt_dma[SGX_PD_ENTRIES];
	u32 va_next;
	atomic_t mmu_dirty;		/* PTEs changed since the last kick */

	struct sgx_buf buf[B_NUM];
	bool bufs_ready;

	/* buffers mapped at a chosen GPU address (replaying iOS's layout) */
	struct sgx_buf extra[SGX_MAX_EXTRA];
	int nextra;
	int boot_result;	/* 0 never tried, 1 acknowledged, -errno */
	unsigned int boots;

	/* the interrupt */
	int irq;
	bool irq_on;
	unsigned int irq_count, irq_unhandled, irq_run;
	u32 irq_events[SGX_MAX_CORES];	/* every event bit seen, per core */
	u32 irq_host_flags;		/* host control's interrupt flags, last */

	/* the 2D engine (blits and fills through the transfer queue) */
	bool quiet;			/* no log line per microkernel command */
	spinlock_t ccb_lock;		/* the kernel CCB: fbcon may draw from atomic context */
	spinlock_t blt_lock;		/* one 2D job at a time; also guards boot_result */
	bool blt_ready, blt_broken, fb_hooked;
	struct work_struct boot_work;
	phys_addr_t fb_pa;
	u64 *blt_code;			/* CPU side of the code page at GPU 0x1000 */
	u32 blt_seq, r_seq;
	u32 fb_w, fb_h, fb_stride;	/* the framebuffer: pixels, pixels, pixels */

	/* the render node */
	struct apple_sgx_drm *drm;
};

/* apple_sgx_hw.c */
int apple_sgx_mmu_map(struct apple_sgx *sgx, u32 va, dma_addr_t pa, size_t size,
		      u32 flags);
void apple_sgx_mmu_unmap(struct apple_sgx *sgx, u32 va, size_t size);
bool apple_sgx_up(struct apple_sgx *sgx);
int apple_sgx_restart(struct apple_sgx *sgx);
int apple_sgx_render_queue(struct apple_sgx *sgx, const u32 *cmd, u32 len, u32 pb,
			   u32 *details, u32 done_va, u32 seq, u32 cache_control, u32 *at);
bool apple_sgx_render_waiting(struct apple_sgx *sgx, u32 at, const u32 *details);
int apple_sgx_render_rekick(struct apple_sgx *sgx);
void apple_sgx_report(struct apple_sgx *sgx, const char *why);
int apple_sgx_fb_find(struct apple_sgx *sgx, struct resource *res);
int apple_sgx_fb_show(struct apple_sgx *sgx, u32 va, u32 stride, u32 x, u32 y, u32 w, u32 h);

/* apple_sgx_drm.c */
int apple_sgx_drm_init(struct apple_sgx *sgx);
void apple_sgx_drm_fini(struct apple_sgx *sgx);
bool apple_sgx_drm_busy(struct apple_sgx *sgx);
int apple_sgx_drm_reserve(struct apple_sgx *sgx, int slot, u32 va, u32 size);
void apple_sgx_drm_unreserve(struct apple_sgx *sgx, int slot);
u32 apple_sgx_bo_va(struct drm_gem_object *obj);

/* apple_sgx_kms.c */
int apple_sgx_kms_init(struct drm_device *drm, struct apple_sgx *sgx);

#endif
