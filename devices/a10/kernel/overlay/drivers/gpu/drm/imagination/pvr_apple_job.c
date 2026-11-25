// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple A10 GPU: queues, jobs and their fences on a running GFX firmware.
 *
 * Submission (docs/gpu.md "G4 progress"): a command's firmware VA goes into
 * its queue's command ring; a work-ring entry on the queue's channel names
 * the queue, the data master and the queue's new command write index; the
 * channel's host write index moves last; a kick on endpoint 0x21 wakes the
 * firmware. Completion: every command carries a completion record (address,
 * value) the firmware's post-processing writes when the command is done --
 * here the queue's done word and the job's sequence number -- and the
 * firmware then signals its rings with 0x42 on endpoint 0x20.
 *
 * The Start / Finalize microsequences are those the harness and the first
 * self-tests ran (G4-G6), built from register blocks the caller fills.
 */

#include <linux/device.h>
#include <linux/dma-fence.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>

#include "pvr_apple.h"
#include "pvr_device.h"
#include "pvr_vm.h"

/* queue block (pool): object, state, command ring, status + done word, scheduling entries */
#define QB_OBJ		0x0000
#define QB_STATE	0x1000
#define QB_CR		0x2000
#define QB_STATUS	0x3000

#define QB_SE		0x4000
#define QB_SIZE		0x8000
#define QB_RING		256	/* command ring entries (state +0x50) */
#define WORK_RING	256	/* work-ring entries per channel (0x14 bytes each) */
#define JOB_DONE	0x1ff0	/* the job's own completion word, at the end of its block */

static void put32(void *base, u32 off, u32 v)
{
	put_unaligned_le32(v, base + off);
}

static void put64(void *base, u32 off, u64 v)
{
	put_unaligned_le64(v, base + off);
}

/* -- fences ------------------------------------------------------------------ */

static const char *fence_driver_name(struct dma_fence *f)
{
	return "powervr";
}

static const char *fence_timeline_name(struct dma_fence *f)
{
	return "apple-gfx";
}

static const struct dma_fence_ops pvr_apple_fence_ops = {
	.get_driver_name = fence_driver_name,
	.get_timeline_name = fence_timeline_name,
};

/* -- queues ------------------------------------------------------------------ */

struct pvr_apple_queue *pvr_apple_queue_create(struct pvr_apple *a, u32 group, u32 dm)
{
	struct pvr_apple_queue *q;
	unsigned long flags;
	void *o;

	if (group > 3 || dm > 2)
		return ERR_PTR(-EINVAL);
	q = kzalloc(sizeof(*q), GFP_KERNEL);
	if (!q)
		return ERR_PTR(-ENOMEM);
	if (pvr_apple_obj_alloc(a, QB_SIZE, &q->obj)) {
		kfree(q);
		return ERR_PTR(-ENOMEM);
	}
	q->a = a;
	q->dm = dm;
	q->ch = 3 * group + dm;
	INIT_LIST_HEAD(&q->pending);
	q->fence_ctx = dma_fence_context_alloc(1);

	/* the queue object (fw 0x2167c): +0 state, +4 command ring, +0xc scheduling entries, +0x44 unit mask, +0x88 status */
	o = q->obj.cpu;
	put32(o, QB_OBJ + 0x00, q->obj.va + QB_STATE);
	put32(o, QB_OBJ + 0x04, q->obj.va + QB_CR);
	put32(o, QB_OBJ + 0x0c, q->obj.va + QB_SE);
	put32(o, QB_OBJ + 0x44, 1);
	put32(o, QB_OBJ + 0x88, q->obj.va + QB_STATUS);
	put32(o, QB_STATE + 0x50, QB_RING);

	spin_lock_irqsave(&a->job_lock, flags);
	list_add_tail(&q->node, &a->queues);
	spin_unlock_irqrestore(&a->job_lock, flags);
	return q;
}

/* the queue's pending jobs are failed first */
void pvr_apple_queue_destroy(struct pvr_apple_queue *q)
{
	struct pvr_apple *a = q->a;
	struct pvr_apple_job *job, *tmp;
	unsigned long flags;
	LIST_HEAD(dead);

	spin_lock_irqsave(&a->job_lock, flags);
	list_splice_init(&q->pending, &dead);
	q->inflight = NULL;
	list_del(&q->node);
	spin_unlock_irqrestore(&a->job_lock, flags);
	list_for_each_entry_safe(job, tmp, &dead, node) {
		dma_fence_set_error(job->fence, -ECANCELED);
		dma_fence_signal(job->fence);
		pvr_apple_job_free(job);
	}
	pvr_apple_obj_free(a, &q->obj);
	kfree(q);
}

/* -- jobs -------------------------------------------------------------------- */

static struct pvr_apple_job *job_alloc(struct pvr_apple_queue *q, u32 size)
{
	struct pvr_apple_job *job = kzalloc(sizeof(*job), GFP_KERNEL);

	if (!job)
		return NULL;
	job->q = q;
	job->vm = q->a->default_vm;
	INIT_LIST_HEAD(&job->node);
	if (pvr_apple_obj_alloc(q->a, size, &job->blk)) {
		kfree(job);
		return NULL;
	}
	return job;
}

/* the job runs in vm (a reference is taken) */
void pvr_apple_job_set_vm(struct pvr_apple_job *job, struct pvr_vm_context *vm)
{
	if (job->vm_ref)
		pvr_vm_context_put(job->vm);
	job->vm = pvr_vm_context_get(vm);
	job->vm_ref = true;
}

void pvr_apple_job_free(struct pvr_apple_job *job)
{
	if (!job)
		return;
	if (job->vm_ref)
		pvr_vm_context_put(job->vm);
	pvr_apple_obj_free(job->q->a, &job->blk);
	if (job->fence)
		dma_fence_put(job->fence);
	kfree(job);
}

/*
 * Compute (type 3, CL callback fw 0x14f38): StartCompute -> WaitIRQ
 * (COMPUTE_FINISHED) -> FinalizeCompute -> End. Block layout: the command
 * at +0 (0x1000), then the microsequence and its objects.
 */
#define CP_US	0x1000
#define CP_CTX	0x1100
#define CP_CA	0x1200
#define CP_CDR	0x1280
#define CP_FL	0x1300		/* +0 cancelled, +0x10 threshold, +0x20 "already finished", +0x30 */
#define CP_STAMP 0x1380		/* FinalizeCompute's stamp */
#define CP_OUT1	0x1400		/* CMD+0x298 / +0x29c */
#define CP_OUT2	0x1600

struct pvr_apple_job *pvr_apple_job_cp(struct pvr_apple_queue *q, const struct pvr_apple_cp_args *args)
{
	struct pvr_apple_job *job = job_alloc(q, 0x2000);
	u32 va, us;
	void *b;
	int i;

	if (!job)
		return ERR_PTR(-ENOMEM);
	b = job->blk.cpu;
	va = job->blk.va;
	for (i = 0; i < 2; i++)
		put32(b, CP_CDR + 4 * i, args->cdr[i]);
	for (i = 0; i < ARRAY_SIZE(args->ca); i++)
		put32(b, CP_CA + 4 * i, args->ca[i]);
	for (i = 0; i < ARRAY_SIZE(args->ctx); i++)
		put32(b, CP_CTX + 4 * i, args->ctx[i]);

	us = CP_US;
	put32(b, us + 0x00, 0xb);			/* StartCompute (0x4c) */
	put32(b, us + 0x04, va + CP_CDR);
	put32(b, us + 0x08, va + CP_CA);
	put32(b, us + 0x0c, pvr_apple_va(PVR_APPLE_R_UT));
	put32(b, us + 0x14, args->mmu_ctx);		/* byte -> RGX+0xf07a */
	put32(b, us + 0x2c, va + CP_CTX);
	put32(b, us + 0x3c, va + CP_FL);		/* cancelled flag */
	put32(b, us + 0x48, va + CP_FL + 0x10);		/* threshold: never cancel */
	put64(b, CP_FL + 0x10, ~0ULL);
	put32(b, us + 0x4c, 4 << 6);			/* WaitIRQ: COMPUTE_FINISHED */
	put32(b, us + 0x50, 0xc);			/* FinalizeCompute */
	put32(b, us + 0x54, pvr_apple_va(PVR_APPLE_R_UT));
	put32(b, us + 0x70, va + CP_STAMP);
	put32(b, us + 0x74, 1);
	put32(b, us + 0x78, 0x34);
	put32(b, us + 0x7c, va + CP_FL + 0x20);		/* "already finished" */
	put32(b, CP_FL + 0x20, args->fast);
	put32(b, us + 0x80, va + CP_FL + 0x30);
	put32(b, us + 0x84, 2);				/* End */

	put32(b, 0x000, 3);
	put32(b, 0x014, va + CP_CTX);
	put32(b, 0x1e8, va + us);
	put32(b, 0x1ec, 0x88);
	put32(b, 0x298, va + CP_OUT1);			/* pointers the firmware writes */
	put32(b, 0x29c, va + CP_OUT2);
	job->rec_addr_off = 0x26c;
	job->rec_val_off = 0x274;
	return job;
}

/*
 * TA (type 0, fw 0x1315a): StartTA -> WaitIRQ (TA_FINISHED) -> FinalizeTA ->
 * End. The free-list descriptor, the PM slot and the large object are the
 * render target's.
 */
#define TA_US	0x1000
#define TA_CTX	0x1100
#define TA_A	0x1200
#define TA_B	0x1240
#define TA_FL	0x1340		/* +0 cancelled, +0x10 threshold, +0x20 "already finished", +0x30 the unit word */
#define TA_C	0x1380		/* FinalizeTA's counter */
#define TA_CTX2	0x1400		/* StartTA +0x40: context state, 0 = fresh */
#define TA_STAMP 0x1500

struct pvr_apple_job *pvr_apple_job_ta(struct pvr_apple_queue *q, const struct pvr_apple_ta_args *args,
				       struct pvr_apple_hwrt *rt)
{
	struct pvr_apple_job *job = job_alloc(q, 0x2000);
	u32 va, ut = pvr_apple_va(PVR_APPLE_R_P2) + 4;	/* the firmware's TA block */
	void *b;
	int i;

	if (!job)
		return ERR_PTR(-ENOMEM);
	b = job->blk.cpu;
	va = job->blk.va;
	for (i = 0; i < ARRAY_SIZE(args->a); i++)
		put32(b, TA_A + 4 * i, args->a[i]);
	for (i = 0; i < ARRAY_SIZE(args->b); i++)
		put64(b, TA_B + 8 * i, args->b[i]);

	put32(b, TA_US + 0x00, 5);			/* StartTA (0x6c) */
	put32(b, TA_US + 0x04, va + TA_A);
	put32(b, TA_US + 0x08, va + TA_B);
	put32(b, TA_US + 0x0c, va + TA_FL + 0x30);
	put32(b, TA_US + 0x10, rt->slot_va);
	put32(b, TA_US + 0x14, ut);
	put32(b, TA_US + 0x18, 1);
	put32(b, TA_US + 0x1c, va + TA_FL);
	put32(b, TA_US + 0x20, args->mmu_ctx);		/* byte -> RGX+0xf078 */
	put32(b, TA_US + 0x40, va + TA_CTX2);
	put32(b, TA_US + 0x64, va + TA_FL + 0x10);
	put64(b, TA_FL + 0x10, ~0ULL);
	put32(b, TA_US + 0x68, 0x20 << 6);		/* (inside StartTA, unused) */
	put32(b, TA_US + 0x6c, 0x20 << 6);		/* WaitIRQ: TA_FINISHED */
	put32(b, TA_US + 0x70, 6);			/* FinalizeTA */
	put32(b, TA_US + 0x74, rt->slot_va);
	put32(b, TA_US + 0x78, va + TA_C);
	put32(b, TA_US + 0x7c, ut);
	put32(b, TA_US + 0x84, va + TA_FL + 0x20);
	put32(b, TA_FL + 0x20, 1);
	put32(b, TA_US + 0xa0, va + TA_STAMP);
	put32(b, TA_US + 0xa4, 1);
	put32(b, TA_US + 0xb0, 2);			/* End */

	put32(b, 0x000, 0);
	put32(b, 0x014, va + TA_CTX);
	put32(b, 0x020, rt->fl_va);
	put32(b, 0x024, rt->tbig_va);
	put32(b, 0x464, va + TA_US);
	put32(b, 0x468, 0xb4);
	job->rec_addr_off = 0x554;
	job->rec_val_off = 0x55c;
	return job;
}

/* 3D (type 1, fw 0x13eb0): Start3D -> WaitIRQ (PIXELBE_END_RENDER) -> Finalize3D -> End */
#define R3_US	0x1000
#define R3_CTX	0x1100
#define R3_A	0x1200
#define R3_B	0x1280
#define R3_FL	0x1380		/* +0 cancelled, +0x10 threshold, +0x20 "already finished", +0x40.. zeros */
#define R3_C	0x1400		/* Finalize3D's counter block */
#define R3_STAMP 0x1440

struct pvr_apple_job *pvr_apple_job_3d(struct pvr_apple_queue *q, const struct pvr_apple_3d_args *args,
				       struct pvr_apple_hwrt *rt)
{
	struct pvr_apple_job *job = job_alloc(q, 0x2000);
	u32 va, ut = pvr_apple_va(PVR_APPLE_R_P3) + 8;	/* the firmware's 3D block */
	void *b;
	int i;

	if (!job)
		return ERR_PTR(-ENOMEM);
	b = job->blk.cpu;
	va = job->blk.va;
	for (i = 0; i < ARRAY_SIZE(args->a); i++)
		put32(b, R3_A + 4 * i, args->a[i]);
	for (i = 0; i < ARRAY_SIZE(args->b); i++)
		put64(b, R3_B + 8 * i, args->b[i]);

	put32(b, R3_US + 0x00, 7);			/* Start3D (0x78) */
	put32(b, R3_US + 0x04, va + R3_A);
	put32(b, R3_US + 0x08, va + R3_B);
	put32(b, R3_US + 0x0c, rt->slot_va);
	put32(b, R3_US + 0x10, ut);
	put32(b, R3_US + 0x14, va + R3_FL + 0x40);	/* -> 0: no partial render */
	put32(b, R3_US + 0x18, va + R3_FL + 0x44);
	put32(b, R3_US + 0x1c, va + R3_FL);
	put32(b, R3_US + 0x20, 1);
	put32(b, R3_US + 0x24, va);			/* the command */
	put32(b, R3_US + 0x28, args->mmu_ctx);		/* byte -> RGX+0xf079 */
	put32(b, R3_US + 0x48, va + R3_FL + 0x48);	/* -> 0: no resume */
	put32(b, R3_US + 0x74, va + R3_FL + 0x10);
	put64(b, R3_FL + 0x10, ~0ULL);
	put32(b, R3_US + 0x78, 8 << 6);			/* WaitIRQ: PIXELBE_END_RENDER */
	put32(b, R3_US + 0x7c, 8);			/* Finalize3D (0x50) */
	put32(b, R3_US + 0x88, va + R3_STAMP);
	put32(b, R3_US + 0x8c, 1);
	put32(b, R3_US + 0x90, rt->slot_va);
	put32(b, R3_US + 0x94, va + R3_C);
	put32(b, R3_US + 0x98, 1);
	put32(b, R3_US + 0x9c, ut);
	put32(b, R3_US + 0xa0, va + R3_FL + 0x20);	/* "already finished" */
	put32(b, R3_FL + 0x20, 1);
	put32(b, R3_US + 0xac, va);
	put32(b, R3_US + 0xb4, va + R3_FL + 0x50);
	put32(b, R3_US + 0xc4, 0x50);
	put32(b, R3_US + 0xc8, va + R3_FL + 0x60);
	put32(b, R3_US + 0xcc, 2);			/* End */

	put32(b, 0x000, 1);
	put32(b, 0x014, va + R3_US);
	put32(b, 0x018, 0xd0);
	put32(b, 0x01c, va + R3_CTX);
	put32(b, 0x020, rt->fl_va);			/* the TA's free list */
	put32(b, 0x024, rt->slot_va);			/* the TA's PM slot */
	job->rec_addr_off = 0x89c;
	job->rec_val_off = 0x8a4;
	return job;
}

/* -- render targets ------------------------------------------------------------ */

/* block: free-list descriptor, PM slot, slot header, the TA's large object (+0x3300 used) */
#define RT_FL	0x0000
#define RT_SLOT	0x0100
#define RT_PMH	0x0200
#define RT_TBIG	0x1000
#define RT_SIZE	0x5000

struct pvr_apple_hwrt *pvr_apple_hwrt_create(struct pvr_apple *a, u64 fl_base, u32 fl_pages,
					     u64 mlist, u64 mta, u64 vheap)
{
	struct pvr_apple_hwrt *rt = kzalloc(sizeof(*rt), GFP_KERNEL);
	void *b;

	if (!rt)
		return ERR_PTR(-ENOMEM);
	if (pvr_apple_obj_alloc(a, RT_SIZE, &rt->obj)) {
		kfree(rt);
		return ERR_PTR(-ENOMEM);
	}
	rt->a = a;
	b = rt->obj.cpu;
	rt->fl_va = rt->obj.va + RT_FL;
	rt->slot_va = rt->obj.va + RT_SLOT;
	rt->pmh_va = rt->obj.va + RT_PMH;
	rt->tbig_va = rt->obj.va + RT_TBIG;
	/* free list (fw 0x17a10): +0x1c base -> RGX 0x210, +0x40 top -> 0x224; +0x50 the registration word */
	put64(b, RT_FL + 0x1c, fl_base);
	put32(b, RT_FL + 0x40, fl_pages - 1);
	put32(b, RT_FL + 0x50, rt->fl_va + 0x80);
	/* PM slot: MLIST, macrotile array, VHEAP table; +0xc0 its header */
	put64(b, RT_SLOT + 0xa0, mlist);
	put64(b, RT_SLOT + 0xa8, mta);
	put64(b, RT_SLOT + 0xb0, vheap);
	put32(b, RT_SLOT + 0xc0, rt->pmh_va);
	return rt;
}

/* the same, with the free-list descriptor of a free list of its own (fl_va) */
struct pvr_apple_hwrt *pvr_apple_hwrt_create_fl(struct pvr_apple *a, u32 fl_va, u64 mlist, u64 mta,
						u64 vheap)
{
	struct pvr_apple_hwrt *rt = pvr_apple_hwrt_create(a, 0, 1, mlist, mta, vheap);

	if (!IS_ERR(rt))
		rt->fl_va = fl_va;
	return rt;
}

/* a free list: its descriptor (fw 0x17a10) over the u32 page numbers at base */
struct pvr_apple_fl *pvr_apple_fl_create(struct pvr_apple *a, u64 base, u32 pages)
{
	struct pvr_apple_fl *fl = kzalloc(sizeof(*fl), GFP_KERNEL);

	if (!fl)
		return ERR_PTR(-ENOMEM);
	if (pvr_apple_obj_alloc(a, 0x1000, &fl->obj)) {
		kfree(fl);
		return ERR_PTR(-ENOMEM);
	}
	fl->a = a;
	fl->va = fl->obj.va;
	put64(fl->obj.cpu, 0x1c, base);			/* -> RGX 0x210 */
	put32(fl->obj.cpu, 0x40, pages - 1);		/* top -> 0x224 */
	put32(fl->obj.cpu, 0x50, fl->va + 0x80);	/* the registration word */
	return fl;
}

void pvr_apple_fl_destroy(struct pvr_apple_fl *fl)
{
	if (IS_ERR_OR_NULL(fl))
		return;
	pvr_apple_obj_free(fl->a, &fl->obj);
	kfree(fl);
}

void pvr_apple_hwrt_destroy(struct pvr_apple_hwrt *rt)
{
	if (IS_ERR_OR_NULL(rt))
		return;
	pvr_apple_obj_free(rt->a, &rt->obj);
	kfree(rt);
}

/* -- submission and completion ------------------------------------------------ */

/*
 * One command per queue in the firmware at a time. With two in flight on a
 * queue the firmware crashed at once -- "DAG: Non Sequential Stamp Updates
 * seen ... stampValue N+1 ... channelRingCommandIndex N": it takes the
 * queue's second work-ring entry before it has checked the first. Whether
 * a different protocol lets a queue hold several (one ring entry covering
 * them, a queue state the host writes) is to find; queues run in parallel.
 * Called with job_lock held; returns true when the firmware needs a kick.
 */
static bool push_locked(struct pvr_apple_job *job)
{
	struct pvr_apple_queue *q = job->q;
	struct pvr_apple *a = q->a;
	u32 wr = a->ring_wr[q->ch];
	void *w;

	q->inflight = job;
	put32(q->obj.cpu, QB_CR + 4 * (q->cmd_idx % QB_RING), job->blk.va);
	q->cmd_idx++;
	w = pvr_apple_ptr(a, PVR_APPLE_R_W, 0x1400 * q->ch + 0x14 * wr);
	put32(w, 0x00, 0);
	put32(w, 0x04, 0);
	put32(w, 0x08, q->obj.va);
	put32(w, 0x0c, q->dm);
	/*
	 * u16 write index, priority 0, flags 0. The index wraps with the
	 * command ring (state +0x50): 0x100 after 0xff left the firmware at
	 * read 0xff for good.
	 */
	put32(w, 0x10, q->cmd_idx % QB_RING);
	a->ring_wr[q->ch] = (wr + 1) % WORK_RING;
	dma_wmb();
	pvr_apple_w32(a, PVR_APPLE_R_G, 0x10 * q->ch + 4, a->ring_wr[q->ch]);
	return true;
}

/*
 * The queue's next job goes to the firmware if the queue is free and its VM
 * is the active one -- or nothing is in the firmware at all, and its VM
 * becomes the active one (one MMU context for all, pvr_apple.c). Called
 * with job_lock held; true when the firmware needs a kick.
 */
static bool try_push_locked(struct pvr_apple_queue *q)
{
	struct pvr_apple *a = q->a;
	struct pvr_apple_job *job;
	struct pvr_apple_queue *o;

	job = list_first_entry_or_null(&q->pending, struct pvr_apple_job, node);
	if (!job || q->inflight)
		return false;
	if (job->vm && job->vm != a->active_vm) {
		list_for_each_entry(o, &a->queues, node)
			if (o->inflight)
				return false;
		pvr_apple_vm_activate(a, job->vm);
	}
	return push_locked(job);
}

/*
 * Queue the job (its completion record -> its own word = its sequence
 * number) and hand it to the firmware when the queue is free. Returns the
 * job's fence (a reference for the caller); the job belongs to the queue
 * from here on and is freed when it completes.
 */
struct dma_fence *pvr_apple_submit(struct pvr_apple_job *job)
{
	struct pvr_apple_queue *q = job->q;
	struct pvr_apple *a = q->a;
	struct dma_fence *f;
	unsigned long flags;
	bool kick = false;
	int err;

	f = kzalloc(sizeof(*f), GFP_KERNEL);
	if (!f) {
		pvr_apple_job_free(job);
		return ERR_PTR(-ENOMEM);
	}

	/* page-table changes the MMU code left for the next flush (maps) */
	if (atomic_xchg(&a->pvr_dev->mmu_flush_cache_flags, 0))
		pvr_apple_mmu_flush(a);

	spin_lock_irqsave(&a->job_lock, flags);
	job->seqno = ++q->seqno;
	dma_fence_init(f, &pvr_apple_fence_ops, &a->fence_lock, q->fence_ctx, job->seqno);
	job->fence = dma_fence_get(f);
	put32(job->blk.cpu, job->rec_addr_off, job->blk.va + JOB_DONE);
	put32(job->blk.cpu, job->rec_val_off, job->seqno);
	list_add_tail(&job->node, &q->pending);
	kick = try_push_locked(q);
	spin_unlock_irqrestore(&a->job_lock, flags);

	if (kick) {
		err = pvr_apple_kick(a, q->dm);
		if (err)
			dev_warn(a->dev, "kick %u: %d\n", q->dm, err);
	}
	schedule_delayed_work(&a->done_work, msecs_to_jiffies(10));
	return f;
}

u32 pvr_apple_queue_word(struct pvr_apple_queue *q, u32 off)
{
	return get_unaligned_le32(q->obj.cpu + off);
}

/* the queue's state as the host and the firmware see it, for the logs */
int pvr_apple_queue_debug(struct pvr_apple_queue *q, char *buf, size_t len)
{
	struct pvr_apple *a = q->a;
	struct pvr_apple_job *j = q->inflight;
	u32 i, n = 0;

	n += scnprintf(buf + n, len - n, "queue ch %u: cmds %u, seqno %u, in flight %d (done word %#x); ring host %u, fw read %#x; state",
		       q->ch, q->cmd_idx, q->seqno, j ? (int)j->seqno : -1,
		       j ? get_unaligned_le32(j->blk.cpu + JOB_DONE) : 0, a->ring_wr[q->ch],
		       pvr_apple_r32(a, PVR_APPLE_R_G, 0x10 * q->ch));
	for (i = 0; i < 0x60; i += 4) {
		u32 v = get_unaligned_le32(q->obj.cpu + QB_STATE + i);

		if (v)
			n += scnprintf(buf + n, len - n, " +%#x=%#x", i, v);
	}
	n += scnprintf(buf + n, len - n, "; status");
	for (i = 0; i < 0x40; i += 4) {
		u32 v = get_unaligned_le32(q->obj.cpu + QB_STATUS + i);

		if (v)
			n += scnprintf(buf + n, len - n, " +%#x=%#x", i, v);
	}
	return n;
}

void pvr_apple_process_completions(struct pvr_apple *a)
{
	struct pvr_apple_job *job, *tmp;
	struct pvr_apple_queue *q;
	unsigned long flags;
	bool busy = false;
	u32 kicks = 0;
	LIST_HEAD(done);

	spin_lock_irqsave(&a->job_lock, flags);
	list_for_each_entry(q, &a->queues, node) {
		job = q->inflight;
		if (job && get_unaligned_le32(job->blk.cpu + JOB_DONE) == job->seqno) {
			list_move_tail(&job->node, &done);
			q->inflight = NULL;
		}
	}
	/* then every queue's next job (a VM switch may wait for all to drain) */
	list_for_each_entry(q, &a->queues, node) {
		if (try_push_locked(q))
			kicks |= BIT(q->dm);
		if (q->inflight || !list_empty(&q->pending))
			busy = true;
	}
	spin_unlock_irqrestore(&a->job_lock, flags);

	for (u32 dm = 0; dm < 3; dm++)
		if (kicks & BIT(dm))
			pvr_apple_kick(a, dm);
	list_for_each_entry_safe(job, tmp, &done, node) {
		list_del(&job->node);
		if (a->crashed)
			dma_fence_set_error(job->fence, -EIO);
		dma_fence_signal(job->fence);
		pvr_apple_job_free(job);
	}
	if (busy)
		schedule_delayed_work(&a->done_work, msecs_to_jiffies(10));
}

/* the firmware is going away: every pending job ends with err */
void pvr_apple_fail_all(struct pvr_apple *a, int err)
{
	struct pvr_apple_job *job, *tmp;
	struct pvr_apple_queue *q;
	unsigned long flags;
	LIST_HEAD(dead);

	spin_lock_irqsave(&a->job_lock, flags);
	list_for_each_entry(q, &a->queues, node) {
		list_splice_tail_init(&q->pending, &dead);
		q->inflight = NULL;
	}
	spin_unlock_irqrestore(&a->job_lock, flags);
	list_for_each_entry_safe(job, tmp, &dead, node) {
		list_del(&job->node);
		dma_fence_set_error(job->fence, err);
		dma_fence_signal(job->fence);
		pvr_apple_job_free(job);
	}
}
