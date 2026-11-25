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
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/xarray.h>

#include <drm/drm_file.h>
#include <drm/drm_syncobj.h>
#include <uapi/drm/pvr_drm.h>

#include "pvr_apple.h"
#include "pvr_device.h"
#include "pvr_rogue_fwif_client.h"
#include "pvr_rogue_fwif_shared.h"
#include "pvr_stream.h"
#include "pvr_stream_defs.h"
#include "pvr_vm.h"

#define DM_CP	2

struct pvr_apple_uctx {
	u32 type;				/* DRM_PVR_CTX_TYPE_* */
	struct pvr_vm_context *vm;		/* a reference */
	struct rogue_fwif_cdm_registers_cswitch cdm;
};

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
	if (args->type != DRM_PVR_CTX_TYPE_COMPUTE)
		return -EOPNOTSUPP;		/* render: G7 step 5; transfer: later */
	if (!args->static_context_state_len)
		return -EINVAL;

	c = kzalloc(sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;
	c->type = args->type;
	c->vm = pvr_vm_context_lookup(pvr_file, args->vm_context_handle);
	if (!c->vm) {
		kfree(c);
		return -EINVAL;
	}
	state = memdup_user(u64_to_user_ptr(args->static_context_state),
			    args->static_context_state_len);
	if (IS_ERR(state)) {
		uctx_free(c);
		return PTR_ERR(state);
	}
	err = pvr_stream_process(pvr_dev, &pvr_static_compute_context_state_stream, state,
				 args->static_context_state_len, &c->cdm);
	kfree(state);
	if (!err)
		err = xa_alloc(&pvr_file->apple_ctx_handles, &args->handle, c, xa_limit_32b,
			       GFP_KERNEL);
	if (err)
		uctx_free(c);
	return err;
}

int pvr_apple_ioctl_destroy_context(struct pvr_file *pvr_file, u32 handle)
{
	struct pvr_apple_uctx *c = xa_erase(&pvr_file->apple_ctx_handles, handle);

	if (!c)
		return -EINVAL;
	uctx_free(c);		/* its jobs hold their own VM reference */
	return 0;
}

void pvr_apple_file_close(struct pvr_file *pvr_file)
{
	struct pvr_apple_uctx *c;
	unsigned long handle;

	xa_for_each(&pvr_file->apple_ctx_handles, handle, c)
		uctx_free(c);
	xa_destroy(&pvr_file->apple_ctx_handles);
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
	job = pvr_apple_job_cp(a->q[DM_CP], &args);
	if (IS_ERR(job))
		return PTR_ERR(job);
	pvr_apple_job_set_vm(job, c->vm);
	f = pvr_apple_submit(job);
	if (IS_ERR(f))
		return PTR_ERR(f);
	err = sync_ops(file, j, f);		/* signals */
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
	err = pvr_apple_start(a);		/* (again, after a firmware crash) */
	if (err)
		return err;

	for (i = 0; i < args->jobs.count; i++, p += args->jobs.stride) {
		struct pvr_apple_uctx *c, snap = { 0 };
		struct drm_pvr_job j;

		if (copy_from_user(&j, u64_to_user_ptr(p), sizeof(j)))
			return -EFAULT;
		/* a snapshot under the lock: destroy_context may run meanwhile */
		xa_lock(&pvr_file->apple_ctx_handles);
		c = xa_load(&pvr_file->apple_ctx_handles, j.context_handle);
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
		default:
			err = -EOPNOTSUPP;	/* geometry / fragment: G7 step 5 */
			break;
		}
		pvr_vm_context_put(snap.vm);
		if (err)
			return err;
	}
	return 0;
}
