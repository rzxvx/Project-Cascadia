// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple S5L8940X (A5) PowerVR SGX543MP2 -- the render node.
 *
 * What Mesa (and sgx2d) talk to the GPU through: GEM buffers mapped into
 * the GPU's address space, and renders queued to the microkernel with
 * syncobjs for synchronisation.  The interface is include/uapi/drm/
 * apple_sgx_drm.h; docs/research/p105-mesa.md is the plan it belongs to.
 *
 * Buffers are shmem objects, write-combined on the CPU side, pinned and
 * mapped page by page into the one page directory the hardware side keeps
 * (apple_sgx_hw.c), at an address the caller chose or one picked here.
 * Addresses the kernel picks get a guard page after them: the VDM reads
 * ahead, and a read past a buffer should fault rather than land in the
 * next one.
 *
 * Renders go through a DRM scheduler with room for one job: the TA command
 * is copied into the render queue and TA sent, and the job is done when
 * both of these have happened, in this order:
 *
 *   - the microkernel has written the command's completion value (it does
 *     when the TA is done, into the scratch buffer at SGX_SCRATCH_RENDER);
 *   - it has cleared the render details' word at +0x24, which it sets to
 *     the 3D register block's address when the TA starts and clears when
 *     the 3D pass ends (found for sgx2d's frame fence, p105-gpu.md M6).
 *
 * The SGX's interrupt has never been seen to fire, so both are polled, by
 * an hrtimer that runs only while a render is on the GPU.  A render that
 * does not finish in render_timeout_ms restarts the microkernel.
 *
 * Two things the microkernel does that the render node works around, both
 * found with Mesa's template-frame clears (docs/research/p105-mesa.md, M12):
 *
 *   - it keeps a parameter buffer's state between renders (the DPM's), so
 *     a new buffer at the same address -- every new client of the template
 *     frame loads one -- has it hand out pages that are not there: a fault
 *     at page 0 above TA_REQ_BASE, a lockup.  A render whose parameter
 *     buffer is another buffer object than the last one's starts the
 *     microkernel again first (pb_restart);
 *   - now and then it takes the TA command and leaves the render in the
 *     queue, unread and unstarted; the poller sends TA again after
 *     rekick_ms.
 *
 * Addresses picked here for buffers of 64 KiB or more are 64 KiB aligned.
 * That came from a fault at 0xefcf0000 for a render target at 0xefcff000,
 * read then as the address losing its low 16 bits; it was the BIF's tiled
 * window (apple_sgx.h), which moves accesses around within 64 KiB, and the
 * kernel now picks addresses below it.  The alignment stays: it costs
 * nothing.
 */

#include <linux/dma-fence.h>
#include <linux/dma-resv.h>
#include <linux/hrtimer.h>
#include <linux/iosys-map.h>
#include <linux/module.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include <drm/apple_sgx_drm.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_managed.h>
#include <drm/drm_mm.h>
#include <drm/drm_print.h>
#include <drm/drm_syncobj.h>
#include <drm/drm_utils.h>
#include <drm/gpu_scheduler.h>

#include "apple_sgx.h"

static unsigned int render_timeout_ms = 2000;
module_param(render_timeout_ms, uint, 0444);
MODULE_PARM_DESC(render_timeout_ms, "a render not done after this restarts the microkernel");

static unsigned int poll_us = 200;
module_param(poll_us, uint, 0644);
MODULE_PARM_DESC(poll_us, "how often a running render is checked on, in microseconds");

static unsigned int rekick_ms = 20;
module_param(rekick_ms, uint, 0644);
MODULE_PARM_DESC(rekick_ms, "TA again for a render left unread this long (0: never)");

static bool pb_restart = true;
module_param(pb_restart, bool, 0644);
MODULE_PARM_DESC(pb_restart, "start the microkernel again for a new parameter buffer");

#define SGX_MAX_REKICKS			5

/* The render details are read up to here (+0xa4 is written). */
#define SGX_DETAILS_SIZE		0xa8
#define SGX_MAX_BOS			4096
#define SGX_MAX_IN_SYNCS		256
#define SGX_MAX_BO_SIZE			SZ_256M

struct apple_sgx_job;

struct apple_sgx_drm {
	struct drm_device drm;
	struct apple_sgx *sgx;

	/* the GPU addresses buffers may take; what debugfs maps there too */
	struct mutex va_lock;
	struct drm_mm va;
	struct drm_mm_node fb_window;
	struct drm_mm_node extra[SGX_MAX_EXTRA];

	atomic_t clients;

	struct drm_gpu_scheduler sched;
	struct mutex sched_lock;	/* arm and push in one order */
	spinlock_t fence_lock;
	u64 fence_context;
	u64 fence_seqno;		/* run_job is serialised by the scheduler */
	u32 render_seq;			/* the completion value of the last render */

	spinlock_t job_lock;		/* active, against the poller */
	struct apple_sgx_job *active;	/* the render on the GPU */
	struct hrtimer poll;
	struct work_struct rekick;

	/* the parameter buffer the microkernel last worked with: its buffer
	 * object's serial, and the microkernel start that saw it */
	atomic64_t bo_serial;
	u64 pb_serial;
	unsigned int pb_boots;

	atomic_t submitted, done, timed_out;
};

struct apple_sgx_bo {
	struct drm_gem_shmem_object base;
	struct drm_mm_node node;	/* its GPU address, and a guard page */
	u32 flags;			/* APPLE_SGX_BO_* */
	u32 mapped;			/* bytes the PTEs cover */
	u64 serial;			/* one per buffer object ever made */
};

struct apple_sgx_file {
	struct drm_sched_entity entity;
};

struct apple_sgx_job {
	struct drm_sched_job base;
	struct apple_sgx_drm *sdrm;

	u32 *cmd;			/* the TA command */
	u32 cmd_len;
	u32 pb_va, cache_control;
	u64 pb_serial;			/* the parameter buffer's object */

	struct drm_gem_object *details_obj;
	struct iosys_map details_map;
	u32 *details;			/* the render details, in details_obj */

	struct drm_gem_object **bos;	/* everything the render touches */
	u32 bo_count;

	struct dma_fence *done_fence;	/* the scheduler's: out_sync, the buffers */
	struct dma_fence *hw_fence;	/* ours, while on the GPU (job_lock) */
	u32 seq;
	bool ta_done;
	u32 ccb_at;			/* where in the render queue it went */
	ktime_t kicked;			/* when TA was last sent for it */
	unsigned int rekicks;
};

static struct apple_sgx_drm *to_sgx_drm(struct drm_device *drm)
{
	return container_of(drm, struct apple_sgx_drm, drm);
}

static struct apple_sgx_bo *to_sgx_bo(struct drm_gem_object *obj)
{
	return container_of(to_drm_gem_shmem_obj(obj), struct apple_sgx_bo, base);
}

static struct apple_sgx_job *to_sgx_job(struct drm_sched_job *job)
{
	return container_of(job, struct apple_sgx_job, base);
}

/* ---- buffers --------------------------------------------------------------- */

static void sgx_gem_free(struct drm_gem_object *obj)
{
	struct apple_sgx_drm *sdrm = to_sgx_drm(obj->dev);
	struct apple_sgx_bo *bo = to_sgx_bo(obj);

	if (drm_mm_node_allocated(&bo->node)) {
		apple_sgx_mmu_unmap(sdrm->sgx, bo->node.start, bo->mapped);
		mutex_lock(&sdrm->va_lock);
		drm_mm_remove_node(&bo->node);
		mutex_unlock(&sdrm->va_lock);
	}
	drm_gem_shmem_free(&bo->base);
}

static const struct drm_gem_object_funcs sgx_gem_funcs = {
	.free = sgx_gem_free,
	.print_info = drm_gem_shmem_object_print_info,
	.pin = drm_gem_shmem_object_pin,
	.unpin = drm_gem_shmem_object_unpin,
	.get_sg_table = drm_gem_shmem_object_get_sg_table,
	.vmap = drm_gem_shmem_object_vmap,
	.vunmap = drm_gem_shmem_object_vunmap,
	.mmap = drm_gem_shmem_object_mmap,
	.vm_ops = &drm_gem_shmem_vm_ops,
};

static struct drm_gem_object *sgx_gem_create_object(struct drm_device *drm, size_t size)
{
	struct apple_sgx_bo *bo = kzalloc(sizeof(*bo), GFP_KERNEL);

	if (!bo)
		return ERR_PTR(-ENOMEM);
	bo->base.base.funcs = &sgx_gem_funcs;
	/* the CPU never caches what the GPU reads: no flushes anywhere */
	bo->base.map_wc = true;
	return &bo->base.base;
}

/* Pin the pages, find the buffer a GPU address, and write its PTEs. */
static int sgx_bo_map(struct apple_sgx_drm *sdrm, struct apple_sgx_bo *bo, u32 va)
{
	size_t size = bo->base.base.size;
	struct scatterlist *sg;
	struct sg_table *sgt;
	unsigned int i;
	u32 pte, at;
	int ret;

	sgt = drm_gem_shmem_get_pages_sgt(&bo->base);
	if (IS_ERR(sgt))
		return PTR_ERR(sgt);

	mutex_lock(&sdrm->va_lock);
	if (bo->flags & APPLE_SGX_BO_FIXED_VA) {
		bo->node.start = va;
		bo->node.size = size;
		ret = drm_mm_reserve_node(&sdrm->va, &bo->node);
		if (ret == -ENOSPC)
			ret = -EEXIST;
	} else if (bo->flags & APPLE_SGX_BO_USSE_CODE) {
		ret = drm_mm_insert_node_in_range(&sdrm->va, &bo->node,
						  size + SGX_PAGE_SIZE, SGX_PAGE_SIZE, 0,
						  SGX_CODE_BASE, SGX_CODE_VA_END,
						  DRM_MM_INSERT_HIGH);
	} else {
		/* below the tiled windows; 64 KiB aligned from 64 KiB up */
		ret = drm_mm_insert_node_in_range(&sdrm->va, &bo->node,
						  size + SGX_PAGE_SIZE,
						  size >= SZ_64K ? SZ_64K : SGX_PAGE_SIZE, 0,
						  SGX_AUTO_VA_START, SGX_TILED_VA_START,
						  DRM_MM_INSERT_HIGH);
	}
	mutex_unlock(&sdrm->va_lock);
	if (ret)
		return ret;

	pte = (bo->flags & APPLE_SGX_BO_GPU_READONLY ? SGX_PTE_READONLY : 0) |
	      (bo->flags & APPLE_SGX_BO_NOT_CACHE_CONSISTENT ? 0 : SGX_PTE_CACHECONSISTENT);
	at = bo->node.start;
	for_each_sgtable_dma_sg(sgt, sg, i) {
		ret = apple_sgx_mmu_map(sdrm->sgx, at, sg_dma_address(sg), sg_dma_len(sg), pte);
		if (ret)
			break;
		at += sg_dma_len(sg);
	}
	bo->mapped = at - bo->node.start;
	if (!ret && bo->mapped != size)
		ret = -EIO;
	if (ret) {
		apple_sgx_mmu_unmap(sdrm->sgx, bo->node.start, size);
		mutex_lock(&sdrm->va_lock);
		drm_mm_remove_node(&bo->node);
		mutex_unlock(&sdrm->va_lock);
	}
	return ret;
}

static int sgx_ioctl_gem_create(struct drm_device *drm, void *data, struct drm_file *file)
{
	struct apple_sgx_drm *sdrm = to_sgx_drm(drm);
	struct drm_apple_sgx_gem_create *args = data;
	struct drm_gem_shmem_object *shmem;
	struct apple_sgx_bo *bo;
	u64 size;
	int ret;

	if (args->pad || (args->flags & ~APPLE_SGX_BO_FLAGS) || !args->size ||
	    args->size > SGX_MAX_BO_SIZE)
		return -EINVAL;
	size = ALIGN(args->size, PAGE_SIZE);
	if ((args->flags & APPLE_SGX_BO_FIXED_VA) &&
	    ((args->va & (SGX_PAGE_SIZE - 1)) || args->va < SGX_USER_VA_START ||
	     size > SGX_USER_VA_END - args->va))
		return -EINVAL;

	shmem = drm_gem_shmem_create(drm, size);
	if (IS_ERR(shmem))
		return PTR_ERR(shmem);
	bo = to_sgx_bo(&shmem->base);
	bo->flags = args->flags;
	bo->serial = atomic64_inc_return(&sdrm->bo_serial);
	ret = sgx_bo_map(sdrm, bo, args->va);
	if (!ret)
		ret = drm_gem_handle_create(file, &shmem->base, &args->handle);
	if (!ret)
		args->va = bo->node.start;
	/* the handle holds it now, or it goes */
	drm_gem_object_put(&shmem->base);
	return ret;
}

static int sgx_ioctl_gem_mmap_offset(struct drm_device *drm, void *data,
				     struct drm_file *file)
{
	struct drm_apple_sgx_gem_mmap_offset *args = data;
	struct drm_gem_object *obj;
	int ret;

	if (args->flags)
		return -EINVAL;
	obj = drm_gem_object_lookup(file, args->handle);
	if (!obj)
		return -ENOENT;
	ret = drm_gem_create_mmap_offset(obj);
	if (!ret)
		args->offset = drm_vma_node_offset_addr(&obj->vma_node);
	drm_gem_object_put(obj);
	return ret;
}

static int sgx_ioctl_gem_wait(struct drm_device *drm, void *data, struct drm_file *file)
{
	struct drm_apple_sgx_gem_wait *args = data;
	unsigned long timeout = drm_timeout_abs_to_jiffies(args->timeout_ns);
	struct drm_gem_object *obj;
	long ret;

	if (args->pad)
		return -EINVAL;
	obj = drm_gem_object_lookup(file, args->handle);
	if (!obj)
		return -ENOENT;
	ret = dma_resv_wait_timeout(obj->resv, DMA_RESV_USAGE_READ, true, timeout);
	if (!ret)
		ret = timeout ? -ETIMEDOUT : -EBUSY;
	else if (ret > 0)
		ret = 0;
	drm_gem_object_put(obj);
	return ret;
}

/* ---- parameters ------------------------------------------------------------ */

static int sgx_ioctl_get_param(struct drm_device *drm, void *data, struct drm_file *file)
{
	struct apple_sgx_drm *sdrm = to_sgx_drm(drm);
	struct drm_apple_sgx_get_param *args = data;
	struct apple_sgx *sgx = sdrm->sgx;

	if (args->pad)
		return -EINVAL;
	switch (args->param) {
	case APPLE_SGX_PARAM_UAPI_VERSION:
		args->value = 1;
		break;
	case APPLE_SGX_PARAM_CORE_ID:
		args->value = sgx->core_id;
		break;
	case APPLE_SGX_PARAM_CORE_REVISION:
		args->value = sgx->core_rev;
		break;
	case APPLE_SGX_PARAM_NUM_CORES:
		args->value = sgx->ncores;
		break;
	case APPLE_SGX_PARAM_CLOCK_KHZ:
		args->value = SGX_CLOCK_KHZ;
		break;
	case APPLE_SGX_PARAM_UKERNEL_UP:
		args->value = apple_sgx_up(sgx);
		break;
	case APPLE_SGX_PARAM_VA_START:
		args->value = SGX_USER_VA_START;
		break;
	case APPLE_SGX_PARAM_VA_END:
		args->value = SGX_USER_VA_END;
		break;
	case APPLE_SGX_PARAM_CODE_BASE:
	case APPLE_SGX_PARAM_CODE_VA_START:
		args->value = SGX_CODE_BASE;
		break;
	case APPLE_SGX_PARAM_CODE_VA_END:
		args->value = SGX_CODE_VA_END;
		break;
	case APPLE_SGX_PARAM_FB_VA:
		args->value = READ_ONCE(sgx->fb_w) ? SGX_FB_VA : 0;
		break;
	case APPLE_SGX_PARAM_FB_WIDTH:
		args->value = READ_ONCE(sgx->fb_w);
		break;
	case APPLE_SGX_PARAM_FB_HEIGHT:
		args->value = READ_ONCE(sgx->fb_w) ? sgx->fb_h : 0;
		break;
	case APPLE_SGX_PARAM_FB_STRIDE:
		args->value = READ_ONCE(sgx->fb_w) ? sgx->fb_stride * 4 : 0;
		break;
	case APPLE_SGX_PARAM_RENDERS_SUBMITTED:
		args->value = atomic_read(&sdrm->submitted);
		break;
	case APPLE_SGX_PARAM_RENDERS_DONE:
		args->value = atomic_read(&sdrm->done);
		break;
	case APPLE_SGX_PARAM_RENDERS_TIMED_OUT:
		args->value = atomic_read(&sdrm->timed_out);
		break;
	case APPLE_SGX_PARAM_UKERNEL_BOOTS:
		args->value = READ_ONCE(sgx->boots);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

/* ---- renders --------------------------------------------------------------- */

static const char *sgx_fence_driver_name(struct dma_fence *fence)
{
	return "apple_sgx";
}

static const char *sgx_fence_timeline_name(struct dma_fence *fence)
{
	return "render";
}

static const struct dma_fence_ops sgx_fence_ops = {
	.get_driver_name = sgx_fence_driver_name,
	.get_timeline_name = sgx_fence_timeline_name,
};

static void sgx_job_release(struct apple_sgx_job *job)
{
	unsigned int i;

	if (job->details)
		drm_gem_vunmap_unlocked(job->details_obj, &job->details_map);
	if (job->details_obj)
		drm_gem_object_put(job->details_obj);
	for (i = 0; i < job->bo_count; i++)
		if (job->bos[i])
			drm_gem_object_put(job->bos[i]);
	kvfree(job->bos);
	kfree(job->cmd);
	if (job->done_fence)
		dma_fence_put(job->done_fence);
	if (job->hw_fence)
		dma_fence_put(job->hw_fence);
	kfree(job);
}

/* Done is the TA's completion value, then the 3D pass's clear of the render
 * details' +0x24 (see the top of this file). */
static enum hrtimer_restart sgx_poll(struct hrtimer *timer)
{
	struct apple_sgx_drm *sdrm = container_of(timer, struct apple_sgx_drm, poll);
	u32 *done = sdrm->sgx->buf[B_SCRATCH].cpu + SGX_SCRATCH_RENDER / 4;
	struct dma_fence *fence = NULL;
	struct apple_sgx_job *job;
	unsigned long flags;

	spin_lock_irqsave(&sdrm->job_lock, flags);
	job = sdrm->active;
	if (job) {
		if (!job->ta_done && READ_ONCE(*done) == job->seq)
			job->ta_done = true;
		if (job->ta_done && !READ_ONCE(job->details[0x24 / 4])) {
			fence = job->hw_fence;
			job->hw_fence = NULL;
			sdrm->active = NULL;
		} else if (!job->ta_done && rekick_ms && job->rekicks < SGX_MAX_REKICKS &&
			   ktime_ms_delta(ktime_get(), job->kicked) >= rekick_ms &&
			   apple_sgx_render_waiting(sdrm->sgx, job->ccb_at, job->details)) {
			job->kicked = ktime_get();
			job->rekicks++;
			schedule_work(&sdrm->rekick);
		}
	}
	spin_unlock_irqrestore(&sdrm->job_lock, flags);

	if (fence) {
		atomic_inc(&sdrm->done);
		dma_fence_signal(fence);
		dma_fence_put(fence);
		return HRTIMER_NORESTART;
	}
	if (!job)
		return HRTIMER_NORESTART;
	hrtimer_forward_now(timer, ns_to_ktime((u64)poll_us * NSEC_PER_USEC));
	return HRTIMER_RESTART;
}

/* TA again for a render the microkernel left in its queue (the poller
 * decides; sending takes the microkernel's lock, so not from the timer) */
static void sgx_rekick_work(struct work_struct *work)
{
	struct apple_sgx_drm *sdrm = container_of(work, struct apple_sgx_drm, rekick);
	struct apple_sgx *sgx = sdrm->sgx;
	struct apple_sgx_job *job;
	unsigned long flags;
	bool waiting = false;
	u32 seq = 0;

	mutex_lock(&sgx->lock);
	spin_lock_irqsave(&sdrm->job_lock, flags);
	job = sdrm->active;
	if (job && !job->ta_done) {
		seq = job->seq;
		waiting = apple_sgx_render_waiting(sgx, job->ccb_at, job->details);
	}
	spin_unlock_irqrestore(&sdrm->job_lock, flags);
	if (waiting) {
		drm_info(&sdrm->drm, "render %u left unread in the queue: TA sent again\n", seq);
		apple_sgx_render_rekick(sgx);
	}
	mutex_unlock(&sgx->lock);
}

static struct dma_fence *sgx_job_run(struct drm_sched_job *sched_job)
{
	struct apple_sgx_job *job = to_sgx_job(sched_job);
	struct apple_sgx_drm *sdrm = job->sdrm;
	struct apple_sgx *sgx = sdrm->sgx;
	struct dma_fence *fence;
	unsigned long flags;
	int ret;

	/* a dependency failed, or the job was cancelled */
	if (unlikely(job->base.s_fence->finished.error))
		return NULL;

	fence = kzalloc(sizeof(*fence), GFP_KERNEL);
	if (!fence)
		return ERR_PTR(-ENOMEM);
	dma_fence_init(fence, &sgx_fence_ops, &sdrm->fence_lock, sdrm->fence_context,
		       ++sdrm->fence_seqno);

	/* another parameter buffer than the one the running microkernel last
	 * used: start it again, or it goes on from the old one's state */
	if (pb_restart && sdrm->pb_serial && sdrm->pb_serial != job->pb_serial &&
	    sdrm->pb_boots == READ_ONCE(sgx->boots)) {
		drm_info(&sdrm->drm, "render with a new parameter buffer at 0x%08x: "
			 "the microkernel starts again\n", job->pb_va);
		apple_sgx_restart(sgx);
	}

	mutex_lock(&sgx->lock);
	if (!apple_sgx_up(sgx)) {
		ret = -ENODEV;
	} else {
		u32 *done = sgx->buf[B_SCRATCH].cpu + SGX_SCRATCH_RENDER / 4;

		job->seq = ++sdrm->render_seq ? sdrm->render_seq : ++sdrm->render_seq;
		job->ta_done = false;
		WRITE_ONCE(*done, 0);
		ret = apple_sgx_render_queue(sgx, job->cmd, job->cmd_len, job->pb_va,
					     job->details,
					     sgx->buf[B_SCRATCH].va + SGX_SCRATCH_RENDER,
					     job->seq, job->cache_control, &job->ccb_at);
	}
	if (!ret) {
		sdrm->pb_serial = job->pb_serial;
		sdrm->pb_boots = sgx->boots;
		spin_lock_irqsave(&sdrm->job_lock, flags);
		job->kicked = ktime_get();
		job->hw_fence = dma_fence_get(fence);
		sdrm->active = job;
		spin_unlock_irqrestore(&sdrm->job_lock, flags);
		hrtimer_start(&sdrm->poll, ns_to_ktime((u64)poll_us * NSEC_PER_USEC),
			      HRTIMER_MODE_REL_SOFT);
	}
	mutex_unlock(&sgx->lock);

	if (ret) {
		drm_dbg(&sdrm->drm, "render not queued: %d\n", ret);
		dma_fence_set_error(fence, ret);
		dma_fence_signal(fence);
	}
	return fence;
}

/* A render that never ended: the microkernel is started again (every queue
 * empty, every mapping kept), and the render fails with ETIMEDOUT. */
static enum drm_gpu_sched_stat sgx_job_timedout(struct drm_sched_job *sched_job)
{
	struct apple_sgx_job *job = to_sgx_job(sched_job);
	struct apple_sgx_drm *sdrm = job->sdrm;
	struct dma_fence *parent = job->base.s_fence->parent;
	struct dma_fence *hw = NULL;
	unsigned long flags;
	int ret;

	if (!parent || dma_fence_is_signaled(parent))
		return DRM_GPU_SCHED_STAT_NOMINAL;

	drm_sched_stop(&sdrm->sched, sched_job);
	hrtimer_cancel(&sdrm->poll);
	spin_lock_irqsave(&sdrm->job_lock, flags);
	if (sdrm->active == job) {
		hw = job->hw_fence;
		job->hw_fence = NULL;
		sdrm->active = NULL;
	}
	spin_unlock_irqrestore(&sdrm->job_lock, flags);

	drm_err(&sdrm->drm, "render %u not done after %u ms (TA %s)\n", job->seq,
		render_timeout_ms, job->ta_done ? "done" : "not done");
	apple_sgx_report(sdrm->sgx, "render timed out");
	drm_sched_increase_karma(sched_job);
	atomic_inc(&sdrm->timed_out);
	ret = apple_sgx_restart(sdrm->sgx);
	if (ret)
		drm_err(&sdrm->drm, "microkernel did not come back: %d\n", ret);

	if (hw) {
		dma_fence_set_error(hw, -ETIMEDOUT);
		dma_fence_signal(hw);
		dma_fence_put(hw);
	}
	drm_sched_start(&sdrm->sched);
	return DRM_GPU_SCHED_STAT_NOMINAL;
}

static void sgx_job_free(struct drm_sched_job *sched_job)
{
	struct apple_sgx_job *job = to_sgx_job(sched_job);

	drm_sched_job_cleanup(sched_job);
	sgx_job_release(job);
}

static const struct drm_sched_backend_ops sgx_sched_ops = {
	.run_job = sgx_job_run,
	.timedout_job = sgx_job_timedout,
	.free_job = sgx_job_free,
};

static int sgx_job_in_syncs(struct apple_sgx_job *job, struct drm_file *file,
			    struct drm_apple_sgx_submit *args)
{
	u32 *handles;
	unsigned int i;
	int ret = 0;

	if (!args->in_sync_count)
		return 0;
	handles = kvmalloc_array(args->in_sync_count, sizeof(*handles), GFP_KERNEL);
	if (!handles)
		return -ENOMEM;
	if (copy_from_user(handles, u64_to_user_ptr(args->in_syncs),
			   args->in_sync_count * sizeof(*handles)))
		ret = -EFAULT;
	for (i = 0; !ret && i < args->in_sync_count; i++)
		ret = drm_sched_job_add_syncobj_dependency(&job->base, file, handles[i], 0);
	kvfree(handles);
	return ret;
}

/* Into the scheduler, behind every render that touches one of its buffers;
 * its fence goes on all of them (as a write: the uAPI does not say which
 * are only read). */
static int sgx_job_push(struct apple_sgx_job *job)
{
	struct apple_sgx_drm *sdrm = job->sdrm;
	struct ww_acquire_ctx ctx;
	unsigned int i;
	int ret;

	ret = drm_gem_lock_reservations(job->bos, job->bo_count, &ctx);
	if (ret)
		return ret;

	mutex_lock(&sdrm->sched_lock);
	drm_sched_job_arm(&job->base);
	job->done_fence = dma_fence_get(&job->base.s_fence->finished);
	for (i = 0; i < job->bo_count; i++) {
		ret = dma_resv_reserve_fences(job->bos[i]->resv, 1);
		if (ret)
			break;
		ret = drm_sched_job_add_implicit_dependencies(&job->base, job->bos[i], true);
		if (ret)
			break;
	}
	if (ret) {
		mutex_unlock(&sdrm->sched_lock);
		goto unlock;
	}
	atomic_inc(&sdrm->submitted);
	drm_sched_entity_push_job(&job->base);
	mutex_unlock(&sdrm->sched_lock);

	for (i = 0; i < job->bo_count; i++)
		dma_resv_add_fence(job->bos[i]->resv, job->done_fence, DMA_RESV_USAGE_WRITE);
unlock:
	drm_gem_unlock_reservations(job->bos, job->bo_count, &ctx);
	return ret;
}

static int sgx_ioctl_submit(struct drm_device *drm, void *data, struct drm_file *file)
{
	struct apple_sgx_drm *sdrm = to_sgx_drm(drm);
	struct apple_sgx_file *fpriv = file->driver_priv;
	struct drm_apple_sgx_submit *args = data;
	struct drm_syncobj *sync_out = NULL;
	struct drm_gem_object *det;
	struct apple_sgx_job *job;
	bool listed = false;
	unsigned int i;
	int ret;

	if (args->flags || args->pad ||
	    args->cmd_size < APPLE_SGX_TA_CMD_MIN || args->cmd_size > APPLE_SGX_TA_CMD_MAX ||
	    (args->details_offset & 3) || !args->bo_handle_count ||
	    args->bo_handle_count > SGX_MAX_BOS || args->in_sync_count > SGX_MAX_IN_SYNCS)
		return -EINVAL;
	if (!apple_sgx_up(sdrm->sgx))
		return -ENODEV;

	if (args->out_sync) {
		sync_out = drm_syncobj_find(file, args->out_sync);
		if (!sync_out)
			return -ENOENT;
	}

	job = kzalloc(sizeof(*job), GFP_KERNEL);
	if (!job) {
		ret = -ENOMEM;
		goto out_sync;
	}
	job->sdrm = sdrm;
	job->pb_va = args->pb_va;
	job->cache_control = args->cache_control;

	job->cmd = memdup_user(u64_to_user_ptr(args->cmd), args->cmd_size);
	if (IS_ERR(job->cmd)) {
		ret = PTR_ERR(job->cmd);
		job->cmd = NULL;
		goto out_job;
	}
	job->cmd_len = job->cmd[0];
	if (job->cmd_len < APPLE_SGX_TA_CMD_MIN || job->cmd_len > args->cmd_size ||
	    (job->cmd_len & 7)) {
		ret = -EINVAL;
		goto out_job;
	}

	/* on a bad handle the ones found before it are held: release puts
	 * every entry that is set */
	ret = drm_gem_objects_lookup(file, u64_to_user_ptr(args->bo_handles),
				     args->bo_handle_count, &job->bos);
	if (job->bos)
		job->bo_count = args->bo_handle_count;
	if (ret)
		goto out_job;

	/* the parameter buffer: in one of the listed buffers, whose serial
	 * tells a new one at the same address (pb_restart) */
	for (i = 0; i < job->bo_count && !job->pb_serial; i++) {
		struct apple_sgx_bo *bo = to_sgx_bo(job->bos[i]);

		if (drm_mm_node_allocated(&bo->node) && args->pb_va >= bo->node.start &&
		    args->pb_va - bo->node.start < job->bos[i]->size)
			job->pb_serial = bo->serial;
	}
	if (!job->pb_serial) {
		ret = -EINVAL;
		goto out_job;
	}

	/* the render details: in one of the listed buffers, and mapped for the
	 * poller */
	det = drm_gem_object_lookup(file, args->details_handle);
	if (!det) {
		ret = -ENOENT;
		goto out_job;
	}
	job->details_obj = det;
	for (i = 0; i < job->bo_count; i++)
		listed |= job->bos[i] == det;
	if (!listed || det->size < SGX_DETAILS_SIZE ||
	    args->details_offset > det->size - SGX_DETAILS_SIZE) {
		ret = -EINVAL;
		goto out_job;
	}
	ret = drm_gem_vmap_unlocked(det, &job->details_map);
	if (ret)
		goto out_job;
	job->details = job->details_map.vaddr + args->details_offset;

	ret = drm_sched_job_init(&job->base, &fpriv->entity, 1, NULL);
	if (ret)
		goto out_job;
	ret = sgx_job_in_syncs(job, file, args);
	if (!ret)
		ret = sgx_job_push(job);
	if (ret) {
		drm_sched_job_cleanup(&job->base);
		goto out_job;
	}
	if (sync_out)
		drm_syncobj_replace_fence(sync_out, job->done_fence);
	goto out_sync;		/* the scheduler owns the job now */

out_job:
	sgx_job_release(job);
out_sync:
	if (sync_out)
		drm_syncobj_put(sync_out);
	return ret;
}

/* ---- the device ------------------------------------------------------------ */

static int sgx_open(struct drm_device *drm, struct drm_file *file)
{
	struct apple_sgx_drm *sdrm = to_sgx_drm(drm);
	struct drm_gpu_scheduler *sched = &sdrm->sched;
	struct apple_sgx *sgx = sdrm->sgx;
	struct apple_sgx_file *fpriv;
	int ret;

	/* with autoboot off, the first client starts the microkernel */
	flush_work(&sgx->boot_work);
	if (!apple_sgx_up(sgx) && !READ_ONCE(sgx->boot_result))
		apple_sgx_restart(sgx);

	fpriv = kzalloc(sizeof(*fpriv), GFP_KERNEL);
	if (!fpriv)
		return -ENOMEM;
	ret = drm_sched_entity_init(&fpriv->entity, DRM_SCHED_PRIORITY_NORMAL, &sched, 1, NULL);
	if (ret) {
		kfree(fpriv);
		return ret;
	}
	file->driver_priv = fpriv;
	atomic_inc(&sdrm->clients);
	return 0;
}

static void sgx_postclose(struct drm_device *drm, struct drm_file *file)
{
	struct apple_sgx_drm *sdrm = to_sgx_drm(drm);
	struct apple_sgx_file *fpriv = file->driver_priv;

	drm_sched_entity_destroy(&fpriv->entity);
	kfree(fpriv);
	atomic_dec(&sdrm->clients);
}

static const struct drm_ioctl_desc sgx_ioctls[] = {
	DRM_IOCTL_DEF_DRV(APPLE_SGX_GET_PARAM, sgx_ioctl_get_param, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(APPLE_SGX_GEM_CREATE, sgx_ioctl_gem_create, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(APPLE_SGX_GEM_MMAP_OFFSET, sgx_ioctl_gem_mmap_offset, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(APPLE_SGX_GEM_WAIT, sgx_ioctl_gem_wait, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(APPLE_SGX_SUBMIT, sgx_ioctl_submit, DRM_RENDER_ALLOW),
};

DEFINE_DRM_GEM_FOPS(sgx_drm_fops);

static const struct drm_driver sgx_drm_driver = {
	.driver_features = DRIVER_GEM | DRIVER_RENDER | DRIVER_SYNCOBJ,
	.open = sgx_open,
	.postclose = sgx_postclose,
	.ioctls = sgx_ioctls,
	.num_ioctls = ARRAY_SIZE(sgx_ioctls),
	.fops = &sgx_drm_fops,
	.gem_create_object = sgx_gem_create_object,
	.name = "apple_sgx",
	.desc = "Apple S5L8940X PowerVR SGX543MP2",
	.date = "20261003",
	.major = 0,
	.minor = 1,
};

bool apple_sgx_drm_busy(struct apple_sgx *sgx)
{
	return sgx->drm && atomic_read(&sgx->drm->clients) > 0;
}

/* debugfs "map": memory mapped at a chosen address, which no buffer of the
 * render node may then take (and the other way round).  Slot is its index
 * in sgx->extra; anything outside the render node's range is not its
 * business. */
int apple_sgx_drm_reserve(struct apple_sgx *sgx, int slot, u32 va, u32 size)
{
	struct apple_sgx_drm *sdrm = sgx->drm;
	u64 end = (u64)va + size;
	struct drm_mm_node *n;
	int ret;

	if (!sdrm || end <= SGX_USER_VA_START || va >= SGX_USER_VA_END)
		return 0;
	if (slot < 0 || slot >= SGX_MAX_EXTRA || va < SGX_USER_VA_START ||
	    end > SGX_USER_VA_END)
		return -EINVAL;
	n = &sdrm->extra[slot];
	memset(n, 0, sizeof(*n));
	n->start = va;
	n->size = size;
	mutex_lock(&sdrm->va_lock);
	ret = drm_mm_reserve_node(&sdrm->va, n);
	mutex_unlock(&sdrm->va_lock);
	return ret == -ENOSPC ? -EEXIST : ret;
}

void apple_sgx_drm_unreserve(struct apple_sgx *sgx, int slot)
{
	struct apple_sgx_drm *sdrm = sgx->drm;

	if (!sdrm || slot < 0 || slot >= SGX_MAX_EXTRA)
		return;
	mutex_lock(&sdrm->va_lock);
	if (drm_mm_node_allocated(&sdrm->extra[slot]))
		drm_mm_remove_node(&sdrm->extra[slot]);
	mutex_unlock(&sdrm->va_lock);
}

static void sgx_drm_release(struct drm_device *drm, void *data)
{
	struct apple_sgx_drm *sdrm = data;
	struct drm_mm_node *n, *next;

	drm_mm_for_each_node_safe(n, next, &sdrm->va)
		drm_mm_remove_node(n);
	drm_mm_takedown(&sdrm->va);
}

int apple_sgx_drm_init(struct apple_sgx *sgx)
{
	struct apple_sgx_drm *sdrm;
	int ret;

	sdrm = devm_drm_dev_alloc(sgx->dev, &sgx_drm_driver, struct apple_sgx_drm, drm);
	if (IS_ERR(sdrm))
		return PTR_ERR(sdrm);
	sdrm->sgx = sgx;
	mutex_init(&sdrm->va_lock);
	mutex_init(&sdrm->sched_lock);
	spin_lock_init(&sdrm->fence_lock);
	spin_lock_init(&sdrm->job_lock);
	sdrm->fence_context = dma_fence_context_alloc(1);
	hrtimer_init(&sdrm->poll, CLOCK_MONOTONIC, HRTIMER_MODE_REL_SOFT);
	sdrm->poll.function = sgx_poll;
	INIT_WORK(&sdrm->rekick, sgx_rekick_work);

	drm_mm_init(&sdrm->va, SGX_USER_VA_START, SGX_USER_VA_END - SGX_USER_VA_START);
	ret = drmm_add_action_or_reset(&sdrm->drm, sgx_drm_release, sdrm);
	if (ret)
		return ret;
	/* the framebuffer's window, whatever the framebuffer turns out to be */
	sdrm->fb_window.start = SGX_FB_VA;
	sdrm->fb_window.size = SGX_FB_WINDOW;
	ret = drm_mm_reserve_node(&sdrm->va, &sdrm->fb_window);
	if (ret)
		return ret;

	ret = drm_sched_init(&sdrm->sched, &sgx_sched_ops, NULL, DRM_SCHED_PRIORITY_COUNT,
			     1, 0, msecs_to_jiffies(render_timeout_ms), NULL, NULL,
			     "apple-sgx-render", sgx->dev);
	if (ret)
		return ret;

	sgx->drm = sdrm;
	ret = drm_dev_register(&sdrm->drm, 0);
	if (ret) {
		sgx->drm = NULL;
		drm_sched_fini(&sdrm->sched);
		return ret;
	}
	drm_info(&sdrm->drm, "render node: buffers at GPU 0x%08x-0x%08x, code base 0x%08x\n",
		 SGX_USER_VA_START, SGX_USER_VA_END, SGX_CODE_BASE);
	return 0;
}

void apple_sgx_drm_fini(struct apple_sgx *sgx)
{
	struct apple_sgx_drm *sdrm = sgx->drm;

	if (!sdrm)
		return;
	drm_dev_unplug(&sdrm->drm);
	drm_sched_fini(&sdrm->sched);
	hrtimer_cancel(&sdrm->poll);
	cancel_work_sync(&sdrm->rekick);
}
