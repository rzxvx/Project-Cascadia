// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple A10 (T8010, "G9P") GPU: the firmware layer of the powervr driver's
 * Apple backend. See pvr_apple.h; the values and their sources are in
 * Cascadia's devices/a10/docs/gpu.md (G2 / G3 / G4 / "the host owns the
 * power state").
 *
 * The AP never reads or writes the Rogue registers ("rgx") itself: while
 * the firmware manages the core, an RGX access that races a power-down is
 * an SError on the AP. The host keeps the power state (Fender) and leaves
 * the firmware's own power / DVFS path off (P+0xd34 = P+0xd3c = 0).
 */

#include <linux/debugfs.h>
#include <linux/dma-fence.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/soc/apple/rtkit.h>
#include <linux/unaligned.h>
#include <clocksource/arm_arch_timer.h>

#include "pvr_apple.h"
#include "pvr_device.h"
#include "pvr_vm.h"

#define PVR_APPLE_REGION_OFF(n, o, s) [PVR_APPLE_R_##n] = (o),
const u32 pvr_apple_region_off[PVR_APPLE_R_COUNT] = {
	PVR_APPLE_REGIONS(PVR_APPLE_REGION_OFF)
};
#undef PVR_APPLE_REGION_OFF

/*
 * mbox_poll=1: read the mailbox every millisecond instead of waiting for its
 * receive interrupt (in case the interrupt routing is wrong).
 */
static bool mbox_poll;
module_param(mbox_poll, bool, 0644);
MODULE_PARM_DESC(mbox_poll, "Apple GPU: poll the coprocessor mailbox (no receive IRQ)");

/*
 * rgx_standin=1: hand the firmware 128 KiB of RAM instead of RGX (P+0x8c0),
 * so every RGX register it writes can be read back (debugfs rgx-standin);
 * no job completes that way. As in the harness's `P+0x8c0=%R`.
 */
static bool rgx_standin;
module_param(rgx_standin, bool, 0644);

static int pvr_apple_rgx_gate(struct pvr_apple *a);

#define R(n) PVR_APPLE_R_##n
#define VA(n) pvr_apple_va(R(n))
#define PA(n) pvr_apple_pa(a, R(n))

void pvr_apple_w32(struct pvr_apple *a, u32 region, u32 off, u32 val)
{
	/* some fields are unaligned (X+0x311); the heap is Normal memory */
	put_unaligned_le32(val, pvr_apple_ptr(a, region, off));
}

void pvr_apple_w64(struct pvr_apple *a, u32 region, u32 off, u64 val)
{
	put_unaligned_le64(val, pvr_apple_ptr(a, region, off));
}

u32 pvr_apple_r32(struct pvr_apple *a, u32 region, u32 off)
{
	return get_unaligned_le32(pvr_apple_ptr(a, region, off));
}

static void w32(struct pvr_apple *a, u32 region, u32 off, u32 val)
{
	pvr_apple_w32(a, region, off, val);
}

/* -- firmware -> host rings ------------------------------------------- */

static const struct {
	u32 ctl, ents, n;
} pvr_apple_rings[PVR_APPLE_NR_RINGS] = {
	{ R(EC0), R(EV0), 256 },	/* X+0xdc / +0xe0: job events */
	{ R(EC1), R(EV1), 256 },	/* X+0xf4 / +0xf8: power events */
	{ R(EC2), R(EV2), 512 },	/* X+0xec / +0xf0: trace */
};

/*
 * The host read index (+0) catches up with the firmware's write index
 * (+0x20); a full ring makes the firmware wait ("Waiting for space").
 */
void pvr_apple_drain_rings(struct pvr_apple *a)
{
	unsigned long flags;
	int i;

	spin_lock_irqsave(&a->ring_lock, flags);
	if (!a->heap)
		goto out;
	for (i = 0; i < PVR_APPLE_NR_RINGS; i++) {
		u32 n = pvr_apple_rings[i].n;
		u32 rd = pvr_apple_r32(a, pvr_apple_rings[i].ctl, 0);
		u32 wr = pvr_apple_r32(a, pvr_apple_rings[i].ctl, 0x20);

		if (rd >= n || wr >= n)
			continue;
		while (rd != wr) {
			u32 t = pvr_apple_r32(a, pvr_apple_rings[i].ents,
					      PVR_APPLE_RING_ENTRY * rd);

			if (t < ARRAY_SIZE(a->ev_count[i]))
				a->ev_count[i][t]++;
			else
				a->ev_other[i]++;
			rd = (rd + 1) % n;
		}
		w32(a, pvr_apple_rings[i].ctl, 0, wr);
	}
out:
	spin_unlock_irqrestore(&a->ring_lock, flags);
}

/* -- RTKit ------------------------------------------------------------ */

static void pvr_apple_rtkit_crashed(void *cookie, const void *crashlog,
				    size_t size)
{
	struct pvr_apple *a = cookie;

	a->crashed = true;
	dev_err(a->dev, "GFX firmware crashed (crash log: %zu bytes)\n", size);
}

static void pvr_apple_rtkit_recv(void *cookie, u8 ep, u64 msg)
{
	struct pvr_apple *a = cookie;

	if (ep == PVR_APPLE_EP_INIT && ((msg >> 48) & 0xff) == PVR_APPLE_MSG_RINGS) {
		a->ring_msgs++;
		pvr_apple_drain_rings(a);
		pvr_apple_process_completions(a);
		return;
	}
	dev_info_ratelimited(a->dev, "ep %#x: message %#llx\n", ep, msg);
}

static void pvr_apple_poll_work(struct work_struct *work)
{
	struct pvr_apple *a = container_of(work, struct pvr_apple, poll_work.work);

	apple_rtkit_poll(a->rtk);
	schedule_delayed_work(&a->poll_work, 1);
}

static const struct apple_rtkit_ops pvr_apple_rtkit_ops = {
	.crashed = pvr_apple_rtkit_crashed,
	.recv_message = pvr_apple_rtkit_recv,
};

/* -- the pool ----------------------------------------------------------- */

int pvr_apple_obj_alloc(struct pvr_apple *a, u32 size, struct pvr_apple_obj *o)
{
	u32 n = DIV_ROUND_UP(size, PVR_APPLE_POOL_UNIT);
	unsigned long flags, i;

	spin_lock_irqsave(&a->pool_lock, flags);
	i = a->heap ? bitmap_find_next_zero_area(a->pool_map, PVR_APPLE_POOL_UNITS, 0, n, 0)
		    : PVR_APPLE_POOL_UNITS;
	if (i + n <= PVR_APPLE_POOL_UNITS)
		bitmap_set(a->pool_map, i, n);
	spin_unlock_irqrestore(&a->pool_lock, flags);
	if (i + n > PVR_APPLE_POOL_UNITS)
		return -ENOMEM;
	o->size = n * PVR_APPLE_POOL_UNIT;
	o->cpu = a->heap + PVR_APPLE_POOL_OFF + i * PVR_APPLE_POOL_UNIT;
	o->va = PVR_APPLE_HEAP_VA + PVR_APPLE_POOL_OFF + i * PVR_APPLE_POOL_UNIT;
	memset(o->cpu, 0, o->size);
	return 0;
}

void pvr_apple_obj_free(struct pvr_apple *a, struct pvr_apple_obj *o)
{
	unsigned long flags;

	if (!o->size)
		return;
	spin_lock_irqsave(&a->pool_lock, flags);
	bitmap_clear(a->pool_map, (o->va - PVR_APPLE_HEAP_VA - PVR_APPLE_POOL_OFF) / PVR_APPLE_POOL_UNIT,
		     o->size / PVR_APPLE_POOL_UNIT);
	spin_unlock_irqrestore(&a->pool_lock, flags);
	o->size = 0;
}

/* -- boot ------------------------------------------------------------- */

static void pvr_apple_free_mem(struct pvr_apple *a)
{
	unsigned long flags;
	void *heap;

	/* the rings' drain takes the heap under this lock; free it outside */
	spin_lock_irqsave(&a->ring_lock, flags);
	heap = a->heap;
	a->heap = NULL;
	spin_unlock_irqrestore(&a->ring_lock, flags);
	if (heap)
		dma_free_coherent(a->dev, PVR_APPLE_HEAP_SIZE, heap, a->heap_dma);
	if (a->page)
		dma_free_coherent(a->dev, SZ_16K, a->page, a->page_dma);
	if (a->desc)
		dma_free_coherent(a->dev, SZ_16K, a->desc, a->desc_dma);
	if (a->rt)
		dma_free_coherent(a->dev, PVR_APPLE_RUNTIME, a->rt, a->rt_dma);
	if (a->standin)
		dma_free_coherent(a->dev, SZ_128K, a->standin, a->standin_dma);
	a->standin = NULL;
	a->page = a->desc = a->rt = NULL;
}

/*
 * Stop the coprocessor, drop RTKit and the GFX power domain. With the
 * domain off the Rogue core loses its state, so the next boot is cold.
 */
void pvr_apple_fw_stop(struct pvr_apple *a)
{
	struct pvr_apple_queue *q, *tmp;

	cancel_delayed_work_sync(&a->done_work);
	pvr_apple_fail_all(a, -ENODEV);
	list_for_each_entry_safe(q, tmp, &a->queues, node)
		pvr_apple_queue_destroy(q);
	if (a->rtk) {
		cancel_delayed_work_sync(&a->poll_work);
		writel(readl(a->fender + FENDER_RUN) & ~1, a->fender + FENDER_RUN);
		usleep_range(10000, 11000);
		apple_rtkit_free(a->rtk);
		a->rtk = NULL;
	}
	if (a->powered) {
		pm_runtime_put_sync(a->dev);
		a->powered = false;
		/* the GPU may still have been writing: let the domain go off first */
		msleep(50);
	}
	pvr_apple_free_mem(a);
	a->crashed = false;
	a->held_state = 0;
	a->ring_msgs = 0;
	memset(a->ev_count, 0, sizeof(a->ev_count));
	memset(a->ev_other, 0, sizeof(a->ev_other));
}

/*
 * Power the GFX domain, load the image into a fresh runtime region and
 * boot RTKit (AppleA7IOP-AKFWrap-v2's recipe, docs "G2: the recipe"):
 * the mailbox (set up by the mailbox driver when RTKit starts it), the
 * timebase, the remap window in both copies, the run bit in Fender.
 */
int pvr_apple_fw_boot(struct pvr_apple *a, const struct pvr_apple_boot_cfg *cfg)
{
	struct device *dev = a->dev;
	u64 t;
	u32 v;
	int err;

	pvr_apple_fw_stop(a);

	err = pm_runtime_resume_and_get(dev);
	if (err)
		return err;
	a->powered = true;

	a->rt = dma_alloc_coherent(dev, PVR_APPLE_RUNTIME, &a->rt_dma, GFP_KERNEL);
	a->heap = dma_alloc_coherent(dev, PVR_APPLE_HEAP_SIZE, &a->heap_dma, GFP_KERNEL);
	a->page = dma_alloc_coherent(dev, SZ_16K, &a->page_dma, GFP_KERNEL);
	a->desc = dma_alloc_coherent(dev, SZ_16K, &a->desc_dma, GFP_KERNEL);
	if (!a->rt || !a->heap || !a->page || !a->desc) {
		err = -ENOMEM;
		goto err_stop;
	}
	memcpy(a->rt, a->fw->data, PVR_APPLE_FW_SIZE);
	dma_wmb();
	bitmap_zero(a->pool_map, PVR_APPLE_POOL_UNITS);
	memset(a->ring_wr, 0, sizeof(a->ring_wr));

	if (readl(a->fender + FENDER_RUN) & 1) {
		dev_warn(dev, "firmware was running: clearing the run bit\n");
		writel(readl(a->fender + FENDER_RUN) & ~1, a->fender + FENDER_RUN);
		usleep_range(10000, 11000);
	}

	a->rtk = apple_rtkit_init(dev, a, NULL, 0, &pvr_apple_rtkit_ops);
	if (IS_ERR(a->rtk)) {
		err = PTR_ERR(a->rtk);
		a->rtk = NULL;
		goto err_stop;
	}
	if (mbox_poll)
		schedule_delayed_work(&a->poll_work, 1);

	/* timebase (gfx-kf has force-timebase-sync) */
	v = readl(a->iop + IOP_TIME_CONTROL);
	writel(v & ~BIT(1), a->iop + IOP_TIME_CONTROL);
	t = arch_timer_read_counter();
	writel(lower_32_bits(t), a->iop + IOP_TIME_LO);
	writel(upper_32_bits(t), a->iop + IOP_TIME_HI);
	writel(v | BIT(1), a->iop + IOP_TIME_CONTROL);

	/* the remap window: the GFX copy in Fender, then the IOP's own */
	writel(0, a->fender + FENDER_WIN_IOP_LO);
	writel(0, a->fender + FENDER_WIN_IOP_HI);
	writel(PVR_APPLE_RUNTIME & ~0xfff, a->fender + FENDER_WIN_SIZE);
	writel(0, a->fender + FENDER_WIN_SIZE_HI);
	writel(lower_32_bits(a->rt_dma), a->fender + FENDER_WIN_PA_LO);
	writel(upper_32_bits(a->rt_dma), a->fender + FENDER_WIN_PA_HI);
	writel(0, a->iop + IOP_WIN_IOP_LO);
	writel(0, a->iop + IOP_WIN_IOP_HI);
	writel(PVR_APPLE_RUNTIME & ~0xfff, a->iop + IOP_WIN_SIZE);
	writel(0, a->iop + IOP_WIN_SIZE_HI);
	writel(lower_32_bits(a->rt_dma), a->iop + IOP_WIN_PA_LO);
	writel(upper_32_bits(a->rt_dma), a->iop + IOP_WIN_PA_HI);
	writel(1, a->iop + IOP_WIN_ENABLE);

	/* run: Fender +0x2048 bit 0 (V2GFX overrides the V2 run bit) */
	writel(readl(a->fender + FENDER_RUN) | 1, a->fender + FENDER_RUN);

	err = apple_rtkit_wake(a->rtk);
	if (err) {
		dev_err(dev, "RTKit did not come up: %d (Fender run %#x; IOP +0x48 %#x, +0x80c %#x, inbox %#x, outbox %#x)\n",
			err, readl(a->fender + FENDER_RUN), readl(a->iop + 0x48),
			readl(a->iop + IOP_TIME_CONTROL), readl(a->iop + 0x4008),
			readl(a->iop + 0x4020));
		goto err_stop;
	}
	err = apple_rtkit_start_ep(a->rtk, PVR_APPLE_EP_INIT);
	if (!err)
		err = apple_rtkit_start_ep(a->rtk, PVR_APPLE_EP_DOORBELL);
	if (err) {
		dev_err(dev, "endpoints 0x20 / 0x21 did not start: %d\n", err);
		goto err_stop;
	}
	dev_info(dev, "GFX firmware up: runtime region %pad, heap %pad\n",
		 &a->rt_dma, &a->heap_dma);
	return 0;

err_stop:
	pvr_apple_fw_stop(a);
	return err;
}

/*
 * The init data (fw 0x19370 and the main task fw 0x1cbd0) and the
 * host-owned power state; then the init message on endpoint 0x20. The
 * values are those of the harness's render runs: its defaults, the "base"
 * arguments of G4 runs 43-45 and `--power POWER_STATE=6 P+0xd34=0
 * P+0xd3c=0`.
 */
int pvr_apple_fw_init(struct pvr_apple *a, const struct pvr_apple_boot_cfg *cfg)
{
	u64 seq = a->page_dma + 0x1000;
	u32 st = cfg->perf_state;
	int ch, err;
	u32 v;

	/* memory descriptor 0: the heap at firmware VA 0x80400000 */
	put_unaligned_le32(PVR_APPLE_HEAP_VA, a->desc + 0x0);
	put_unaligned_le32(a->heap_dma >> 12, a->desc + 0x4);
	put_unaligned_le32(0, a->desc + 0x8);
	put_unaligned_le32((PVR_APPLE_HEAP_SIZE >> 12) & 0xffff, a->desc + 0xc);

	/* the init page: an empty init sequence, X / T, the descriptors, A-D */
	put_unaligned_le64(seq, a->page + 0x08);
	put_unaligned_le32(VA(X), a->page + 0x18);
	put_unaligned_le32(VA(T), a->page + 0x1c);
	put_unaligned_le64(a->desc_dma, a->page + 0x8c);
	put_unaligned_le32(1, a->page + 0x94);
	put_unaligned_le32(VA(A), a->page + 0x98);
	put_unaligned_le32(VA(B), a->page + 0x9c);
	put_unaligned_le32(VA(C), a->page + 0xa0);
	put_unaligned_le32(VA(D), a->page + 0xa4);

	w32(a, R(X), 0x0, VA(P));
	/*
	 * P+0x0..0xc: {u64 base, u64 size} -> the four BIF tiling ranges
	 * (RGX+0x12e0.., fw 0x5660). Left 0 they tile the whole GPU VA space
	 * (G5 "DONE"): keep them far above our VAs.
	 */
	pvr_apple_w64(a, R(P), 0x0, 0x600000000ULL);
	pvr_apple_w64(a, R(P), 0x8, 0xffc00000ULL);
	/* P+0x10 / +0x18: the PDS / USC execution base (RGX+0x610, +0x4008..) */
	pvr_apple_w64(a, R(P), 0x10, cfg->pds_base);
	pvr_apple_w64(a, R(P), 0x18, cfg->usc_base);

	/* firmware -> host rings; without X+0xf4 a cold power-up faults */
	w32(a, R(X), 0xdc, VA(EC0));
	w32(a, R(X), 0xe0, VA(EV0));
	w32(a, R(X), 0xf4, VA(EC1));
	w32(a, R(X), 0xf8, VA(EV1));
	w32(a, R(X), 0xec, VA(EC2));
	w32(a, R(X), 0xf0, VA(EV2));

	/* I/O type 0x14: RGX, mapped by the firmware */
	pvr_apple_w64(a, R(P), 0x8c0, a->rgx_pa);
	if (rgx_standin) {
		a->standin = dma_alloc_coherent(a->dev, SZ_128K, &a->standin_dma, GFP_KERNEL);
		if (!a->standin)
			return -ENOMEM;
		pvr_apple_w64(a, R(P), 0x8c0, a->standin_dma);
	}
	w32(a, R(P), 0x8cc, 0x20000);

	/*
	 * 13 channels at X+0xc + 0x10 * ch: {&firmware read index, &its copy,
	 * &host write index, ring base}; ch = 3 * group + data master, ch 12
	 * = device control. One index word each on page G.
	 */
	for (ch = 0; ch < 13; ch++) {
		w32(a, R(X), 0xc + 0x10 * ch, VA(G) + 0x10 * ch);
		w32(a, R(X), 0x14 + 0x10 * ch, VA(G) + 0x10 * ch + 4);
		w32(a, R(X), 0x10 + 0x10 * ch, VA(G) + 0x100 + 4 * ch);
		w32(a, R(X), 0x18 + 0x10 * ch, VA(W) + 0x1400 * ch);
	}

	/*
	 * X+0x4 -> E: the GPU VA the cache flush before a power-off writes its
	 * fence to (RGX+0xe240, fw 0x5660); mapped by the job's page tables.
	 */
	pvr_apple_w64(a, R(E), 0x0, cfg->fence_va);

	/* the "base" arguments (docs: G4 runs 43-45) */
	pvr_apple_w64(a, R(P), 0xac0, PA(S));		/* FenderScratchRam: a RAM page */
	w32(a, R(X), 0x4, VA(E));
	w32(a, R(X), 0x8, VA(F));
	w32(a, R(X), 0x311, VA(H));			/* the power struct, unaligned */
	w32(a, R(B), 0x49e8, VA(L));			/* the mapping ring */
	w32(a, R(B), 0x49ec, VA(M));
	pvr_apple_w64(a, R(P), 0x9c0, a->fender_pa);	/* I/O type 0x1c: Fender */
	w32(a, R(P), 0x9cc, 0x4000);
	w32(a, R(P), 0x9dc, 1);
	w32(a, R(X), 0x150, VA(N));
	w32(a, R(P), 0xd34, cfg->fw_power);		/* 0: the host owns the power state */
	w32(a, R(P), 0xd3c, cfg->fw_power);
	w32(a, R(T), 0x7ec, VA(Y));			/* MMU roots: context 0 = the kernel's VM */
	pvr_apple_w64(a, R(Y), 0x0, cfg->ctx0_pc);
	for (ch = 1; ch < PVR_APPLE_MMU_CTXS; ch++)	/* 1..31: the slots' catalog pages */
		pvr_apple_w64(a, R(Y), 8 * ch, a->slots_dma + SZ_4K * ch);
	w32(a, R(T), 0x7f0, GENMASK(PVR_APPLE_MMU_CTXS - 1, 1));
	w32(a, R(T), 0x7e8, VA(GEN));
	w32(a, R(X), 0x198, VA(XG));
	w32(a, R(B), 0x4a24, 1);			/* scheduler init */
	w32(a, R(X), 0x144, VA(P2));			/* per-data-master blocks */
	w32(a, R(X), 0x148, VA(P3));
	w32(a, R(X), 0x14c, VA(P4));
	w32(a, R(P), 0x1abc, 1);			/* unit masks: TA / 3D / CP */
	w32(a, R(P), 0x1ac0, 1);
	w32(a, R(P), 0x1ac4, 1);
	pvr_apple_w64(a, R(P), 0x6e0, PA(TS));		/* I/O type 5, TempSensorRegister */
	w32(a, R(P), 0x6ec, 0x4000);
	w32(a, R(H), 0x4, 240000);			/* DVFS sample period: 10 ms */
	dma_wmb();

	/* the host's power state, before the init message (as the kext) */
	if (cfg->fw_power)
		goto send;
	writel(0x10 | st, a->fender + FENDER_PERF_STATE);
	err = readl_poll_timeout(a->fender + FENDER_PERF_STATE, v,
				 (v & 0x1f) == st, 100, 200000);
	a->held_state = err ? 0 : st;
	usleep_range(10000, 11000);
	dev_info(a->dev, "Fender perf state %u (%#x)%s\n", st,
		 readl(a->fender + FENDER_PERF_STATE), err ? ": not reached" : "");
	if (err)
		return err;

send:
	err = apple_rtkit_send_message(a->rtk, PVR_APPLE_EP_INIT,
				       PVR_APPLE_MSG_INIT | a->page_dma, NULL, false);
	if (err)
		return err;

	/* the main task marks A+0x4 / +0x10 / +0x14 when it is done */
	err = read_poll_timeout(pvr_apple_r32, v, v == 1 || a->crashed, 1000, 2000000,
				false, a, R(A), 0x14);
	if (a->crashed) {
		dev_err(a->dev, "the firmware crashed on its init data\n");
		return -EIO;
	}
	/* the harness gave it 2 s; the flags may also sit in its D-cache */
	msleep(err ? 0 : 200);
	dev_info(a->dev, "init data %s (A+0x4 %#x, +0x10 %#x, +0x14 %#x)\n",
		 err ? "sent, main task flags not seen after 2 s" : "taken",
		 pvr_apple_r32(a, R(A), 0x4), pvr_apple_r32(a, R(A), 0x10),
		 pvr_apple_r32(a, R(A), 0x14));
	return 0;
}

/*
 * An RGX read from the AP. Only while the host holds the GPU's power state
 * (P+0xd34 = 0: the firmware never powers the core down by itself) and
 * Fender still reports that state; otherwise a read can race a power-down,
 * which is an SError on the AP (docs: G4 runs 43-44).
 */
int pvr_apple_rgx_read(struct pvr_apple *a, u32 off, u32 *val)
{
	if (off >= SZ_1M || pvr_apple_rgx_gate(a))
		return -EBUSY;
	*val = readl(a->rgx + off);
	return 0;
}

/*
 * Page tables changed (pvr_mmu_flush_exec): every context's MMU generation
 * moves (T+0x7e8 -> u32 per context). When the scheduler dispatches a job
 * of a context (fw 0x2167c / 0x1071a -> fw 0x19f0e) it compares that word
 * with its own copy (X+0x198), and the bits that differ pick the depth:
 * old ^ new < 0x1000 -> RGX+0xe138 = 0x1 (PT), < 0x400000 -> 0x2 (PD),
 * else 0x4 (PC) -- the table at fw 0x2fee0 -- | context << 3, polled
 * until clear. Upstream's MMU code frees and reallocates page tables, so a
 * PT-level invalidation (a generation + 1) left a stale directory entry
 * (an MMU fault after unmap + map at the same VA): move bit 22, the whole
 * context.
 */
#define PVR_APPLE_MMU_GEN_PC	0x400000u
/*
 * mmu_inval_ctx=1: after mirroring a slot's root table the host also
 * invalidates that context's MMU caches itself (RGX+0xe138 = PC level |
 * context << 3, as fw 0x19f0e does), under the RGX access gate.
 */
static bool mmu_inval_ctx = true;
module_param(mmu_inval_ctx, bool, 0644);
static uint mmu_inval_bits = 0x7;	/* 0x4 PC, 0x2 PD, 0x1 PT */
module_param(mmu_inval_bits, uint, 0644);

static int pvr_apple_rgx_gate(struct pvr_apple *a)
{
	if (!a->powered || !a->held_state || !a->heap || a->standin ||
	    pvr_apple_r32(a, PVR_APPLE_R_P, 0xd34) || pvr_apple_r32(a, PVR_APPLE_R_P, 0xd3c) ||
	    (readl(a->fender + FENDER_PERF_STATE) & 0x1f) != a->held_state)
		return -EBUSY;
	return 0;
}

int pvr_apple_mmu_flush(struct pvr_apple *a)
{
	unsigned long flags;
	u32 slots = 0, v;
	int ctx;

	spin_lock_irqsave(&a->ring_lock, flags);
	for (ctx = 1; ctx < PVR_APPLE_MMU_CTXS; ctx++)
		if (a->slot_vm[ctx]) {
			memcpy(a->slots + SZ_4K * ctx,
			       pvr_vm_get_page_table_root_ptr(a->slot_vm[ctx]), SZ_4K);
			slots |= BIT(ctx);
		}
	if (a->heap) {
		for (ctx = 0; ctx < PVR_APPLE_MMU_CTXS; ctx++)
			w32(a, R(GEN), 4 * ctx, pvr_apple_r32(a, R(GEN), 4 * ctx) + PVR_APPLE_MMU_GEN_PC);
		dma_wmb();
	}
	spin_unlock_irqrestore(&a->ring_lock, flags);

	for (ctx = 1; mmu_inval_ctx && ctx < PVR_APPLE_MMU_CTXS; ctx++) {
		if (!(slots & BIT(ctx)) || pvr_apple_rgx_gate(a))
			continue;
		writel(mmu_inval_bits | ctx << 3, a->rgx + 0xe138);
		if (readl_poll_timeout_atomic(a->rgx + 0xe138, v, !(v & 0x7), 1, 1000))
			dev_warn(a->dev, "MMU invalidation of context %d did not finish\n", ctx);
	}
	return 0;
}

/* give a VM a firmware MMU context; returns its number (1..31) */
int pvr_apple_vm_attach(struct pvr_apple *a, struct pvr_vm_context *vm)
{
	unsigned long flags;
	int ctx, ret = -ENOSPC;

	spin_lock_irqsave(&a->ring_lock, flags);
	for (ctx = 1; ctx < PVR_APPLE_MMU_CTXS; ctx++) {
		if (!a->slot_vm[ctx]) {
			a->slot_vm[ctx] = vm;
			ret = ctx;
			break;
		}
	}
	spin_unlock_irqrestore(&a->ring_lock, flags);
	if (ret > 0)
		pvr_apple_mmu_flush(a);		/* mirror its root table */
	return ret;
}

void pvr_apple_vm_detach(struct pvr_apple *a, int ctx)
{
	unsigned long flags;

	if (ctx <= 0 || ctx >= PVR_APPLE_MMU_CTXS)
		return;
	spin_lock_irqsave(&a->ring_lock, flags);
	a->slot_vm[ctx] = NULL;
	memset(a->slots + SZ_4K * ctx, 0, SZ_4K);	/* nothing mapped */
	spin_unlock_irqrestore(&a->ring_lock, flags);
	pvr_apple_mmu_flush(a);
}

/* debug: the page-catalog base RGX holds for an MMU context (RGX+0xe140 select, +0xe148 read) */
int pvr_apple_mmu_cbase(struct pvr_apple *a, int ctx, u64 *pc)
{
	u32 sel, v;

	if (pvr_apple_rgx_gate(a))
		return -EBUSY;
	sel = readl(a->rgx + 0xe140);
	writel(ctx, a->rgx + 0xe140);
	v = readl(a->rgx + 0xe148);
	writel(sel, a->rgx + 0xe140);
	*pc = (u64)(v & 0x0fffffff) << 12;
	return 0;
}

int pvr_apple_kick(struct pvr_apple *a, u32 dm)
{
	return apple_rtkit_send_message(a->rtk, PVR_APPLE_EP_DOORBELL,
					PVR_APPLE_MSG_KICK | dm, NULL, false);
}

/* -- probe ------------------------------------------------------------ */

static void pvr_apple_done_work(struct work_struct *work)
{
	pvr_apple_process_completions(container_of(work, struct pvr_apple, done_work.work));
}

static void pvr_apple_iounmap(void *p)
{
	iounmap((void __iomem __force *)p);
}

bool pvr_apple_match(struct device *dev)
{
	return of_device_is_compatible(dev->of_node, "apple,t8010-gpu");
}

int pvr_apple_probe(struct pvr_device *pvr_dev)
{
	struct device *dev = from_pvr_device(pvr_dev)->dev;
	struct platform_device *pdev = to_platform_device(dev);
	const char *fw_name = "apple/gfx-t8010-22H355.bin";
	struct resource *res;
	struct pvr_apple *a;
	int err;

	a = devm_kzalloc(dev, sizeof(*a), GFP_KERNEL);
	if (!a)
		return -ENOMEM;
	a->pvr_dev = pvr_dev;
	a->dev = dev;
	mutex_init(&a->lock);
	spin_lock_init(&a->ring_lock);
	spin_lock_init(&a->pool_lock);
	spin_lock_init(&a->job_lock);
	INIT_LIST_HEAD(&a->queues);
	INIT_DELAYED_WORK(&a->done_work, pvr_apple_done_work);
	spin_lock_init(&a->fence_lock);
	INIT_DELAYED_WORK(&a->poll_work, pvr_apple_poll_work);

	/* RGX: handed to the firmware (P+0x8c0); the AP reads it only through pvr_apple_rgx_read() */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "rgx");
	if (!res)
		return -EINVAL;
	a->rgx_pa = res->start;
	a->rgx = ioremap_np(res->start, resource_size(res));
	if (!a->rgx)
		return -ENOMEM;
	err = devm_add_action_or_reset(dev, pvr_apple_iounmap, (void __force *)a->rgx);
	if (err)
		return err;

	a->fender = devm_platform_get_and_ioremap_resource(pdev, 1, &res);
	if (IS_ERR(a->fender))
		return PTR_ERR(a->fender);
	a->fender_pa = res->start;

	/* shared with the mailbox driver, which owns the region */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "iop");
	if (!res)
		return -EINVAL;
	a->iop = ioremap_np(res->start, resource_size(res));
	if (!a->iop)
		return -ENOMEM;
	err = devm_add_action_or_reset(dev, pvr_apple_iounmap, (void __force *)a->iop);
	if (err)
		return err;

	err = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(40));
	if (err)
		return err;

	of_property_read_string(dev->of_node, "firmware-name", &fw_name);
	err = request_firmware(&a->fw, fw_name, dev);
	if (err)
		return dev_err_probe(dev, err, "no GFX firmware %s\n", fw_name);
	if (a->fw->size != PVR_APPLE_FW_SIZE) {
		dev_err(dev, "%s: %zu bytes, not %#x\n", fw_name, a->fw->size,
			PVR_APPLE_FW_SIZE);
		release_firmware(a->fw);
		return -EINVAL;
	}

	err = devm_pm_runtime_enable(dev);
	if (err) {
		release_firmware(a->fw);
		return err;
	}

	/*
	 * What upstream's VM / MMU code needs of the device info so far: a
	 * 40-bit GPU VA space (Rogue's; our tiling range sits at 0x6_0000_0000).
	 * The kernel's own VM context maps what the firmware and the
	 * self-tests need; with 16 KiB CPU pages it uses 16 KiB GPU pages.
	 */
	a->slots = dmam_alloc_coherent(dev, SZ_4K * PVR_APPLE_MMU_CTXS, &a->slots_dma, GFP_KERNEL);
	if (!a->slots) {
		release_firmware(a->fw);
		return -ENOMEM;
	}
	pvr_dev->features.has_virtual_address_space_bits = true;
	pvr_dev->features.virtual_address_space_bits = 40;
	pvr_dev->apple = a;
	pvr_dev->kernel_vm_ctx = pvr_vm_create_context(pvr_dev, false);
	if (IS_ERR(pvr_dev->kernel_vm_ctx)) {
		err = PTR_ERR(pvr_dev->kernel_vm_ctx);
		pvr_dev->kernel_vm_ctx = NULL;
		pvr_dev->apple = NULL;
		release_firmware(a->fw);
		return err;
	}
	pvr_apple_debugfs_init(a);
	dev_info(dev, "Apple G9P GPU, firmware %s; self-tests in debugfs pvr-apple/selftest\n",
		 fw_name);
	return 0;
}

void pvr_apple_remove(struct pvr_device *pvr_dev)
{
	struct pvr_apple *a = pvr_dev->apple;

	if (!a)
		return;
	debugfs_remove_recursive(a->debugfs);
	mutex_lock(&a->lock);
	pvr_apple_test_fini(a);
	pvr_apple_fw_stop(a);
	mutex_unlock(&a->lock);
	if (pvr_dev->kernel_vm_ctx)
		pvr_vm_context_put(pvr_dev->kernel_vm_ctx);
	pvr_dev->kernel_vm_ctx = NULL;
	release_firmware(a->shaders);
	release_firmware(a->fw);
	pvr_dev->apple = NULL;
}
