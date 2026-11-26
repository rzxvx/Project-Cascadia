// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Apple A10 GPU: the powervr UAPI's contexts and jobs on Apple's firmware.
 *
 * Userspace speaks upstream powervr (include/uapi/drm/pvr_drm.h): a context
 * carries its static context state, a job its firmware command as a
 * "stream" (pvr_stream_defs.c), both in Rogue register values. Here they
 * become the register blocks of the firmware's Start ops (pvr_apple_job.c)
 * -- a per-job CPU translation; the GPU runs the same work either way.
 *
 * Compute (G7 step 4). The CDM registers, as G4 / G5 found them in
 * StartCompute's blocks:
 *   CA[0..1]  -> RGX+0x498  CDM context state base   <- cmd regs.cdm_context_state_base_addr
 *   CA[2..3]  -> RGX+0x4b8  CDM_TERMINATE_PDS        <- ctx cdmreg_cdm_terminate_pds
 *   CA[4..5]  -> RGX+0x480  the control stream base   <- cmd regs.cdm_ctrl_stream_base
 *   CA[6..17] -> six slots through RGX+0x11040       <- the kernel's scratch (every VM maps it)
 *   CTX[0xe..0xf] -> RGX+0x4a8 CDM_CONTEXT_PDS0      <- ctx cdmreg_cdm_context_pds0
 *   CTX[0x10]     -> RGX+0x4b0 CDM_CONTEXT_PDS1      <- ctx cdmreg_cdm_context_pds1
 *   CDR[0]        -> RGX+0x4c0 CDM_TERMINATE_PDS1    <- ctx cdmreg_cdm_terminate_pds1
 * Not used yet (no register found for them on G9P, to check):
 * tpu_border_colour_table, cdm_resume_pds1, the resume PDS programs.
 */

#include <linux/dma-fence.h>
#include <linux/dma-fence-chain.h>
#include <linux/kref.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/xarray.h>

#include <drm/drm_file.h>
#include <drm/drm_syncobj.h>
#include <uapi/drm/pvr_drm.h>

#include "pvr_apple.h"
#include "pvr_device.h"
#include "pvr_gem.h"
#include "pvr_rogue_fwif_client.h"
#include "pvr_rogue_fwif_shared.h"
#include "pvr_stream.h"
#include "pvr_stream_defs.h"
#include "pvr_vm.h"

#define DM_CP	2

enum { UOBJ_CTX = 1, UOBJ_FL, UOBJ_HWRT };

struct pvr_apple_uctx;
static void uctx_free(struct pvr_apple_uctx *c);

struct pvr_apple_uctx {
	u32 kind;				/* UOBJ_CTX */
	u32 type;				/* DRM_PVR_CTX_TYPE_* */
	struct pvr_vm_context *vm;		/* a reference */
	struct rogue_fwif_cdm_registers_cswitch cdm;
};

/* a free list: the kernel's PM pages, their numbers in userspace's free-list BO */
struct pvr_apple_ufl {
	u32 kind;				/* UOBJ_FL */
	struct kref ref;			/* the file's handle, the datasets using it */
	struct pvr_gem_object *pages;
	struct pvr_apple_fl *fl;
	u32 n;
};

/*
 * An HWRT dataset: the TA / 3D register values the kernel computes from the
 * size (as upstream pvr_hwrt.c: 32x32 tiles, 4 x 4 macrotiles), the two
 * render-target data sets' PM slots, and the order of its jobs.
 */
struct pvr_apple_uhwrt {
	u32 kind;				/* UOBJ_HWRT */
	struct pvr_apple_ufl *ufl;
	struct pvr_apple_hwrt *rt[2];
	u64 rgn[2], tpc;
	u32 ppp_screen, te_screen, te_mtile1, te_mtile2, mtile_stride, isp_mtile_size, isp_rgn;
	struct dma_fence *geom[2], *frag[2];	/* the last of each, per data set */
};

static void ufl_release(struct kref *ref)
{
	struct pvr_apple_ufl *f = container_of(ref, struct pvr_apple_ufl, ref);

	pvr_apple_fl_destroy(f->fl);
	if (f->pages)
		pvr_gem_object_put(f->pages);
	kfree(f);
}

static void uhwrt_free(struct pvr_apple_uhwrt *h)
{
	int i;

	for (i = 0; i < 2; i++) {
		/* the firmware reads the slots until the jobs are done */
		if (h->geom[i])
			dma_fence_wait_timeout(h->geom[i], false, 10 * HZ);
		if (h->frag[i])
			dma_fence_wait_timeout(h->frag[i], false, 10 * HZ);
		dma_fence_put(h->geom[i]);
		dma_fence_put(h->frag[i]);
		pvr_apple_hwrt_destroy(h->rt[i]);
	}
	if (h->ufl)
		kref_put(&h->ufl->ref, ufl_release);
	kfree(h);
}

static void uobj_free(void *o)
{
	switch (*(u32 *)o) {
	case UOBJ_CTX:
		uctx_free(o);
		break;
	case UOBJ_FL:
		kref_put(&((struct pvr_apple_ufl *)o)->ref, ufl_release);
		break;
	case UOBJ_HWRT:
		uhwrt_free(o);
		break;
	}
}

static void uctx_free(struct pvr_apple_uctx *c)
{
	if (c->vm)
		pvr_vm_context_put(c->vm);
	kfree(c);
}

int pvr_apple_ioctl_create_context(struct pvr_device *pvr_dev, struct pvr_file *pvr_file,
				   struct drm_pvr_ioctl_create_context_args *args)
{
	struct pvr_apple_uctx *c;
	void *state;
	int err;

	if (args->flags || args->callstack_addr)
		return -EINVAL;
	if (args->type != DRM_PVR_CTX_TYPE_COMPUTE && args->type != DRM_PVR_CTX_TYPE_RENDER)
		return -EOPNOTSUPP;		/* transfer: later */
	if (args->type == DRM_PVR_CTX_TYPE_COMPUTE && !args->static_context_state_len)
		return -EINVAL;

	c = kzalloc(sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;
	c->kind = UOBJ_CTX;
	c->type = args->type;
	c->vm = pvr_vm_context_lookup(pvr_file, args->vm_context_handle);
	if (!c->vm) {
		kfree(c);
		return -EINVAL;
	}
	state = NULL;
	if (args->static_context_state_len) {
		state = memdup_user(u64_to_user_ptr(args->static_context_state),
				    args->static_context_state_len);
		if (IS_ERR(state)) {
			uctx_free(c);
			return PTR_ERR(state);
		}
	}
	/* render: the geometry context-switch registers are not used yet (to check) */
	err = args->type == DRM_PVR_CTX_TYPE_COMPUTE ?
	      pvr_stream_process(pvr_dev, &pvr_static_compute_context_state_stream, state,
				 args->static_context_state_len, &c->cdm) : 0;
	kfree(state);
	if (!err)
		err = xa_alloc(&pvr_file->apple_ctx_handles, &args->handle, c, xa_limit_32b,
			       GFP_KERNEL);
	if (err)
		uctx_free(c);
	return err;
}

/* contexts, free lists, HWRT datasets: one handle space (the file's xarray) */
static int pvr_apple_ioctl_destroy(struct pvr_file *pvr_file, u32 handle, u32 kind)
{
	void *o;

	xa_lock(&pvr_file->apple_ctx_handles);
	o = xa_load(&pvr_file->apple_ctx_handles, handle);
	if (o && *(u32 *)o == kind)
		__xa_erase(&pvr_file->apple_ctx_handles, handle);
	else
		o = NULL;
	xa_unlock(&pvr_file->apple_ctx_handles);
	if (!o)
		return -EINVAL;
	uobj_free(o);		/* jobs hold their own VM reference */
	return 0;
}

int pvr_apple_ioctl_destroy_context(struct pvr_file *pvr_file, u32 handle)
{
	return pvr_apple_ioctl_destroy(pvr_file, handle, UOBJ_CTX);
}

int pvr_apple_ioctl_destroy_free_list(struct pvr_file *pvr_file, u32 handle)
{
	return pvr_apple_ioctl_destroy(pvr_file, handle, UOBJ_FL);
}

int pvr_apple_ioctl_destroy_hwrt_dataset(struct pvr_file *pvr_file, u32 handle)
{
	return pvr_apple_ioctl_destroy(pvr_file, handle, UOBJ_HWRT);
}

void pvr_apple_file_close(struct pvr_file *pvr_file)
{
	unsigned long handle;
	void *o;

	/* datasets before the free lists they use (each holds a reference anyway) */
	xa_for_each(&pvr_file->apple_ctx_handles, handle, o)
		if (*(u32 *)o == UOBJ_HWRT || *(u32 *)o == UOBJ_CTX)
			uobj_free(o);
	xa_for_each(&pvr_file->apple_ctx_handles, handle, o)
		if (*(u32 *)o == UOBJ_FL)
			uobj_free(o);
	xa_destroy(&pvr_file->apple_ctx_handles);
}

/*
 * A free list: initial_num_pages PM pages (4 KiB; no growth yet: an out-of-
 * memory PM stops the TA), their page numbers written into userspace's
 * free-list BO at free_list_gpu_addr, the firmware descriptor over it.
 */
int pvr_apple_ioctl_create_free_list(struct pvr_device *pvr_dev, struct pvr_file *pvr_file,
				     struct drm_pvr_ioctl_create_free_list_args *args)
{
	struct pvr_vm_context *vm;
	struct pvr_gem_object *list;
	struct pvr_apple_ufl *f;
	u64 off = 0, size = 0;
	u32 *map, i;
	int err;

	if (!args->initial_num_pages || args->initial_num_pages > args->max_num_pages ||
	    args->initial_num_pages > 65536)
		return -EINVAL;
	vm = pvr_vm_context_lookup(pvr_file, args->vm_context_handle);
	if (!vm)
		return -EINVAL;
	f = kzalloc(sizeof(*f), GFP_KERNEL);
	if (!f) {
		pvr_vm_context_put(vm);
		return -ENOMEM;
	}
	f->kind = UOBJ_FL;
	kref_init(&f->ref);
	f->n = args->initial_num_pages;

	err = -EINVAL;
	list = pvr_vm_find_gem_object(vm, args->free_list_gpu_addr, &off, &size);
	if (!list)
		goto err;
	if (size < 4ULL * f->n) {
		pvr_gem_object_put(list);
		goto err;
	}
	f->pages = pvr_gem_object_create(pvr_dev, ALIGN(f->n * SZ_4K, PAGE_SIZE), 0);
	if (IS_ERR(f->pages)) {
		err = PTR_ERR(f->pages);
		f->pages = NULL;
		pvr_gem_object_put(list);
		goto err;
	}
	err = PTR_ERR_OR_ZERO(pvr_gem_object_get_pages_sgt(f->pages));
	map = err ? NULL : pvr_gem_object_vmap(list);
	if (IS_ERR_OR_NULL(map)) {
		err = err ?: -ENOMEM;
		pvr_gem_object_put(list);
		goto err;
	}
	for (i = 0; i < f->n && !err; i++) {
		dma_addr_t d;

		err = pvr_gem_get_dma_addr(f->pages, SZ_4K * i, &d);
		map[off / 4 + i] = d >> 12;
	}
	pvr_gem_object_vunmap(list);
	pvr_gem_object_put(list);
	if (err)
		goto err;
	err = pvr_apple_start(pvr_dev->apple);
	if (err)
		goto err;
	f->fl = pvr_apple_fl_create(pvr_dev->apple, args->free_list_gpu_addr, f->n);
	if (IS_ERR(f->fl)) {
		err = PTR_ERR(f->fl);
		f->fl = NULL;
		goto err;
	}
	err = xa_alloc(&pvr_file->apple_ctx_handles, &args->handle, f, xa_limit_32b, GFP_KERNEL);
	if (err)
		goto err;
	pvr_vm_context_put(vm);
	return 0;

err:
	pvr_vm_context_put(vm);
	kref_put(&f->ref, ufl_release);
	return err;
}

int pvr_apple_ioctl_create_hwrt_dataset(struct pvr_device *pvr_dev, struct pvr_file *pvr_file,
					struct drm_pvr_ioctl_create_hwrt_dataset_args *args)
{
	struct pvr_apple *a = pvr_dev->apple;
	struct pvr_apple_uhwrt *h;
	struct pvr_apple_ufl *f;
	u32 w = args->width, ht = args->height, tx, ty, mx, my, fh;
	int i, err;

	if (!w || !ht || w > 8192 || ht > 8192 || args->samples > 1 || args->layers > 1)
		return -EINVAL;		/* one sample, one layer for now */
	h = kzalloc(sizeof(*h), GFP_KERNEL);
	if (!h)
		return -ENOMEM;
	h->kind = UOBJ_HWRT;

	/* the free list: the global one when given, else the local one */
	fh = args->free_list_handles[1] ?: args->free_list_handles[0];
	xa_lock(&pvr_file->apple_ctx_handles);
	f = xa_load(&pvr_file->apple_ctx_handles, fh);
	if (f && f->kind == UOBJ_FL)
		kref_get(&f->ref);
	else
		f = NULL;
	xa_unlock(&pvr_file->apple_ctx_handles);
	if (!f) {
		kfree(h);
		return -EINVAL;
	}
	h->ufl = f;

	tx = DIV_ROUND_UP(w, 32);
	ty = DIV_ROUND_UP(ht, 32);
	mx = DIV_ROUND_UP(DIV_ROUND_UP(tx, 4), 4) * 4;
	my = DIV_ROUND_UP(DIV_ROUND_UP(ty, 4), 4) * 4;
	h->ppp_screen = ((ht - 1) << 16) | (w - 1);
	h->te_screen = ((ty - 1) << 12) | (tx - 1);
	h->te_mtile1 = (mx << 18) | (2 * mx << 9) | 3 * mx;
	h->te_mtile2 = (my << 18) | (2 * my << 9) | 3 * my;
	h->mtile_stride = mx * my;
	h->isp_mtile_size = (mx << 16) | my;
	h->isp_rgn = mx * my * 5 / 4;
	h->tpc = args->geom_data_args.tpc_dev_addr;

	err = pvr_apple_start(a);
	for (i = 0; i < 2 && !err; i++) {
		const struct drm_pvr_create_hwrt_rt_data_args *rd = &args->rt_data_args[i];

		h->rgn[i] = rd->region_header_dev_addr;
		h->rt[i] = pvr_apple_hwrt_create_fl(a, f->fl, rd->pm_mlist_dev_addr,
						    rd->macrotile_array_dev_addr,
						    args->geom_data_args.vheap_table_dev_addr);
		if (IS_ERR(h->rt[i])) {
			err = PTR_ERR(h->rt[i]);
			h->rt[i] = NULL;
		}
	}
	if (!err)
		err = xa_alloc(&pvr_file->apple_ctx_handles, &args->handle, h, xa_limit_32b,
			       GFP_KERNEL);
	if (err)
		uhwrt_free(h);
	return err;
}

/* the job's waits (synchronous for now) and its signals, after the submission */
static int sync_ops(struct drm_file *file, const struct drm_pvr_job *j, struct dma_fence *done)
{
	u64 p = j->sync_ops.array;
	u32 i;
	int err = 0;

	if (j->sync_ops.count && j->sync_ops.stride < sizeof(struct drm_pvr_sync_op))
		return -EINVAL;
	for (i = 0; i < j->sync_ops.count && !err; i++, p += j->sync_ops.stride) {
		struct drm_pvr_sync_op op;
		u32 type;

		if (copy_from_user(&op, u64_to_user_ptr(p), sizeof(op)))
			return -EFAULT;
		if (op.flags & ~DRM_PVR_SYNC_OP_FLAGS_MASK)
			return -EINVAL;
		type = op.flags & DRM_PVR_SYNC_OP_FLAG_HANDLE_TYPE_MASK;
		if (type != DRM_PVR_SYNC_OP_FLAG_HANDLE_TYPE_SYNCOBJ &&
		    type != DRM_PVR_SYNC_OP_FLAG_HANDLE_TYPE_TIMELINE_SYNCOBJ)
			return -EINVAL;

		if (!(op.flags & DRM_PVR_SYNC_OP_FLAG_SIGNAL)) {
			struct dma_fence *f;
			long t;

			if (!done) {
				err = drm_syncobj_find_fence(file, op.handle, op.value, 0, &f);
				if (err)
					return err;
				t = dma_fence_wait_timeout(f, true, 10 * HZ);
				dma_fence_put(f);
				if (t <= 0)
					return t ?: -ETIMEDOUT;
			}
		} else if (done) {
			struct drm_syncobj *s = drm_syncobj_find(file, op.handle);

			if (!s)
				return -ENOENT;
			if (type == DRM_PVR_SYNC_OP_FLAG_HANDLE_TYPE_TIMELINE_SYNCOBJ) {
				struct dma_fence_chain *chain = dma_fence_chain_alloc();

				if (!chain) {
					drm_syncobj_put(s);
					return -ENOMEM;
				}
				drm_syncobj_add_point(s, chain, done, op.value);
			} else {
				drm_syncobj_replace_fence(s, done);
			}
			drm_syncobj_put(s);
		}
	}
	return err;
}

static void ca64(struct pvr_apple_cp_args *a, u32 i, u64 v)
{
	a->ca[i] = lower_32_bits(v);
	a->ca[i + 1] = upper_32_bits(v);
}

static int submit_compute(struct pvr_apple *a, struct pvr_device *pvr_dev, struct drm_file *file,
			  struct pvr_apple_uctx *c, const struct drm_pvr_job *j)
{
	struct rogue_fwif_cmd_compute cmd = { 0 };
	struct pvr_apple_cp_args args = { 0 };
	struct pvr_apple_job *job;
	struct dma_fence *f;
	void *stream;
	int err, k;

	if (j->flags || j->hwrt.set_handle)
		return -EINVAL;
	stream = memdup_user(u64_to_user_ptr(j->cmd_stream), j->cmd_stream_len);
	if (IS_ERR(stream))
		return PTR_ERR(stream);
	err = pvr_stream_process(pvr_dev, &pvr_cmd_compute_stream, stream, j->cmd_stream_len, &cmd);
	kfree(stream);
	if (err)
		return err;

	ca64(&args, 0, cmd.regs.cdm_context_state_base_addr ?: PVR_APPLE_CDM_STATE_VA);
	ca64(&args, 2, c->cdm.cdmreg_cdm_terminate_pds);
	ca64(&args, 4, cmd.regs.cdm_ctrl_stream_base);
	for (k = 0; k < 6; k++)
		ca64(&args, 6 + 2 * k, PVR_APPLE_CDM_SLOTS_VA + 0x100 * k);
	args.ctx[0xe] = lower_32_bits(c->cdm.cdmreg_cdm_context_pds0);
	args.ctx[0xf] = upper_32_bits(c->cdm.cdmreg_cdm_context_pds0);
	args.ctx[0x10] = lower_32_bits(c->cdm.cdmreg_cdm_context_pds1);
	args.cdr[0] = lower_32_bits(c->cdm.cdmreg_cdm_terminate_pds1);
	args.fast = false;		/* the full FinalizeCompute: its flush writes the USC's stores out */

	err = sync_ops(file, j, NULL);		/* waits */
	if (err)
		return err;
	mutex_lock(&a->lock);		/* the idle power-off waits for this */
	err = pvr_apple_start_locked(a);
	job = err ? ERR_PTR(err) : pvr_apple_job_cp(a->q[DM_CP], &args);
	if (!IS_ERR(job))
		pvr_apple_job_set_vm(job, c->vm);
	f = IS_ERR(job) ? ERR_CAST(job) : pvr_apple_submit(job);
	mutex_unlock(&a->lock);
	if (IS_ERR(f))
		return PTR_ERR(f);
	err = sync_ops(file, j, f);		/* signals */
	dma_fence_put(f);
	return err;
}

/* wait for a fence (the order of a dataset's jobs), then drop it */
static int wait_put(struct dma_fence **f)
{
	long t = *f ? dma_fence_wait_timeout(*f, true, 10 * HZ) : 1;

	if (t > 0) {
		dma_fence_put(*f);
		*f = NULL;
	}
	return t > 0 ? 0 : t ?: -ETIMEDOUT;
}

static struct pvr_apple_uhwrt *hwrt_get(struct pvr_file *pvr_file, const struct drm_pvr_job *j)
{
	struct pvr_apple_uhwrt *h;

	if (j->hwrt.data_index > 1)
		return NULL;
	xa_lock(&pvr_file->apple_ctx_handles);
	h = xa_load(&pvr_file->apple_ctx_handles, j->hwrt.set_handle);
	if (h && h->kind != UOBJ_HWRT)
		h = NULL;
	xa_unlock(&pvr_file->apple_ctx_handles);
	return h;	/* (a dataset destroyed meanwhile: to guard) */
}

/*
 * Geometry -> StartTA (docs/gpu.md "G6 step 2" / "legalization"):
 *   A: 0xc78 = 0x88, 0xc88 <- ppp_ctrl, 0xc98 PPP_SCREEN, 0xc18 TE_SCREEN,
 *      0xc08 / 0xc10 TE_MTILE1 / 2, 0xc20 the macrotile stride (the dataset's),
 *      0xc28 <- te_psg
 *   B: 0xc90 PDS_CTRL = 0x1e3ce508 (not in the UAPI without BRN 56279; to
 *      check), 0xc38 region headers, 0xc40 TPC, 0xc00 TE_AA 0, 0xc80 0x88,
 *      0x428 / 0xcc0 the kernel's TA buffers, 0x470 the VDM sync PDS program,
 *      0x408 <- vdm_ctrl_stream_base, the 0x11030 slots the kernel's scratch
 */
static int submit_geom(struct pvr_apple *a, struct pvr_device *pvr_dev, struct drm_file *file,
		       struct pvr_apple_uctx *c, const struct drm_pvr_job *j)
{
	struct pvr_apple_uhwrt *h = hwrt_get(file->driver_priv, j);
	struct rogue_fwif_cmd_geom cmd = { 0 };
	struct pvr_apple_ta_args args = { 0 };
	u32 i = j->hwrt.data_index, k;
	struct pvr_apple_job *job;
	struct dma_fence *f;
	void *stream;
	int err;

	if (!h || j->flags)
		return -EINVAL;
	stream = memdup_user(u64_to_user_ptr(j->cmd_stream), j->cmd_stream_len);
	if (IS_ERR(stream))
		return PTR_ERR(stream);
	err = pvr_stream_process(pvr_dev, &pvr_cmd_geom_stream, stream, j->cmd_stream_len, &cmd);
	kfree(stream);
	if (err)
		return err;

	args.a[0] = 0x88;
	args.a[1] = cmd.regs.ppp_ctrl;
	args.a[2] = h->ppp_screen;
	args.a[3] = h->te_screen;
	args.a[4] = h->te_mtile1;
	args.a[5] = h->te_mtile2;
	args.a[6] = h->mtile_stride;
	args.a[7] = cmd.regs.te_psg;
	args.b[0] = 0x1e3ce508;
	args.b[1] = h->rgn[i];
	args.b[2] = h->tpc;
	args.b[4] = 0x88;
	args.b[5] = PVR_APPLE_TA_BUF_VA + 0x100;
	args.b[6] = PVR_APPLE_TA_BUF_VA + 0x200;
	args.b[13] = PVR_APPLE_VDM_SYNC_PDS;
	args.b[15] = cmd.regs.vdm_ctrl_stream_base;
	for (k = 0; k < 6; k++)
		args.b[22 + k] = PVR_APPLE_TA_SLOTS_VA + 0x100 * k;

	/* this data set's previous fragment job first (its PM pages, its slot) */
	err = wait_put(&h->frag[i]) ?: sync_ops(file, j, NULL);
	if (err)
		return err;
	mutex_lock(&a->lock);
	err = pvr_apple_start_locked(a);
	job = err ? ERR_PTR(err) : pvr_apple_job_ta(a->q[0], &args, h->rt[i]);
	if (!IS_ERR(job))
		pvr_apple_job_set_vm(job, c->vm);
	f = IS_ERR(job) ? ERR_CAST(job) : pvr_apple_submit(job);
	mutex_unlock(&a->lock);
	if (IS_ERR(f))
		return PTR_ERR(f);
	dma_fence_put(h->geom[i]);
	h->geom[i] = dma_fence_get(f);
	err = sync_ops(file, j, f);
	dma_fence_put(f);
	return err;
}

/*
 * Fragment -> Start3D:
 *   A: 0xf18 ISP_MTILE_SIZE (the dataset's), 0xf80 <- isp_bgobjdepth,
 *      0xf88 <- isp_bgobjvals, [3] <- isp_aa, 0x6d0 = 0x1f, 0x618 the EOT
 *      area's PDS code, 0x620 <- event_pixel_pds_data, 0x628 <-
 *      event_pixel_pds_info, 0xf38 <- isp_ctl, 0x3e00 = 0x408, 0xfd8 = 4,
 *      0x4070 = 0xf9 (those three to name)
 *   B: 0xfa8 = 0x88, 0xfb0 (B[1]) <- isp_scissor_base (the scissor table
 *      the ISP reads with ISPCTL scenable: found by trying the B slots, the
 *      others left every scissored pixel out), 0xf28 ISP_RGN (the
 *      dataset's), 0x6a0 / 0x6b0 / 0x6b8 <- pds_bgnd[0..2], 0x11080 x 6 =
 *      0x1d3
 *      0xf48 <- isp_zlsctl, 0xf50 / 0xf58 <- isp_zload_store_base (load /
 *      store), 0xf60 / 0xf68 <- isp_stencil_load_store_base, 0xfd0 <-
 *      isp_zls_pixels (without it a linear store had a stride of 1)
 * Not used yet: depth bias (its table base presumably 0xfb8, B[2]: to
 * check), occlusion queries, PBE words (the end-of-tile program loads its
 * state).
 */
static int submit_frag(struct pvr_apple *a, struct pvr_device *pvr_dev, struct drm_file *file,
		       struct pvr_apple_uctx *c, const struct drm_pvr_job *j)
{
	struct pvr_apple_uhwrt *h = hwrt_get(file->driver_priv, j);
	struct rogue_fwif_cmd_frag *cmd;
	struct pvr_apple_3d_args args = { 0 };
	u32 i = j->hwrt.data_index, k;
	struct pvr_apple_job *job;
	struct dma_fence *f;
	void *stream;
	int err;

	if (!h || j->flags)
		return -EINVAL;
	cmd = kzalloc(sizeof(*cmd), GFP_KERNEL);
	if (!cmd)
		return -ENOMEM;
	stream = memdup_user(u64_to_user_ptr(j->cmd_stream), j->cmd_stream_len);
	if (IS_ERR(stream)) {
		kfree(cmd);
		return PTR_ERR(stream);
	}
	err = pvr_stream_process(pvr_dev, &pvr_cmd_frag_stream, stream, j->cmd_stream_len, cmd);
	kfree(stream);
	if (!err) {
		args.a[0] = h->isp_mtile_size;
		args.a[1] = cmd->regs.isp_bgobjdepth;
		args.a[2] = cmd->regs.isp_bgobjvals;
		args.a[3] = cmd->regs.isp_aa;
		args.a[4] = 0x1f;
		args.a[5] = PVR_APPLE_EOT_PDS;
		args.a[6] = cmd->regs.event_pixel_pds_data;
		args.a[7] = cmd->regs.event_pixel_pds_info;
		args.a[8] = cmd->regs.isp_ctl;
		args.a[9] = 0x408;
		args.a[10] = 4;
		args.a[11] = 0xf9;
		args.b[0] = 0x88;
		args.b[1] = cmd->regs.isp_scissor_base;
		/* the depth / stencil load and store: ZLSCTL, the depth's load /
		 * store bases, the stencil's, ZLS_PIXELS (the stride - 1 and
		 * height - 1 of a linear buffer) */
		args.b[5] = cmd->regs.isp_zlsctl;
		args.b[6] = cmd->regs.isp_zload_store_base;
		args.b[7] = cmd->regs.isp_zload_store_base;
		args.b[8] = cmd->regs.isp_stencil_load_store_base;
		args.b[9] = cmd->regs.isp_stencil_load_store_base;
		args.b[28] = cmd->regs.isp_zls_pixels;
		args.b[10] = h->isp_rgn;
		args.b[11] = cmd->regs.pds_bgnd[0];
		args.b[13] = cmd->regs.pds_bgnd[1];
		args.b[14] = cmd->regs.pds_bgnd[2];
		for (k = 22; k < 28; k++)
			args.b[k] = 0x1d3;
	}
	kfree(cmd);
	if (err)
		return err;

	/* after this data set's geometry job */
	err = wait_put(&h->geom[i]) ?: sync_ops(file, j, NULL);
	if (err)
		return err;
	mutex_lock(&a->lock);
	err = pvr_apple_start_locked(a);
	job = err ? ERR_PTR(err) : pvr_apple_job_3d(a->q[1], &args, h->rt[i]);
	if (!IS_ERR(job))
		pvr_apple_job_set_vm(job, c->vm);
	f = IS_ERR(job) ? ERR_CAST(job) : pvr_apple_submit(job);
	mutex_unlock(&a->lock);
	if (IS_ERR(f))
		return PTR_ERR(f);
	dma_fence_put(h->frag[i]);
	h->frag[i] = dma_fence_get(f);
	err = sync_ops(file, j, f);
	dma_fence_put(f);
	return err;
}

int pvr_apple_ioctl_submit_jobs(struct pvr_device *pvr_dev, struct drm_file *file,
				struct drm_pvr_ioctl_submit_jobs_args *args)
{
	struct pvr_file *pvr_file = file->driver_priv;
	struct pvr_apple *a = pvr_dev->apple;
	u64 p = args->jobs.array;
	u32 i;
	int err;

	if (args->jobs.count && args->jobs.stride < sizeof(struct drm_pvr_job))
		return -EINVAL;
	for (i = 0; i < args->jobs.count; i++, p += args->jobs.stride) {
		struct pvr_apple_uctx *c, snap = { 0 };
		struct drm_pvr_job j;

		if (copy_from_user(&j, u64_to_user_ptr(p), sizeof(j)))
			return -EFAULT;
		/* a snapshot under the lock: destroy_context may run meanwhile */
		xa_lock(&pvr_file->apple_ctx_handles);
		c = xa_load(&pvr_file->apple_ctx_handles, j.context_handle);
		if (c && c->kind != UOBJ_CTX)
			c = NULL;
		if (c) {
			snap = *c;
			pvr_vm_context_get(snap.vm);
		}
		xa_unlock(&pvr_file->apple_ctx_handles);
		if (!c)
			return -EINVAL;
		switch (j.type) {
		case DRM_PVR_JOB_TYPE_COMPUTE:
			err = snap.type == DRM_PVR_CTX_TYPE_COMPUTE ?
			      submit_compute(a, pvr_dev, file, &snap, &j) : -EINVAL;
			break;
		case DRM_PVR_JOB_TYPE_GEOMETRY:
			err = snap.type == DRM_PVR_CTX_TYPE_RENDER ?
			      submit_geom(a, pvr_dev, file, &snap, &j) : -EINVAL;
			break;
		case DRM_PVR_JOB_TYPE_FRAGMENT:
			err = snap.type == DRM_PVR_CTX_TYPE_RENDER ?
			      submit_frag(a, pvr_dev, file, &snap, &j) : -EINVAL;
			break;
		default:
			err = -EOPNOTSUPP;	/* transfer: later */
			break;
		}
		pvr_vm_context_put(snap.vm);
		if (err)
			return err;
	}
	return 0;
}
