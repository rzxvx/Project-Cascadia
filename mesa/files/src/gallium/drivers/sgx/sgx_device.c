/*
 * Copyright 2026 Project Cascadia
 * SPDX-License-Identifier: MIT
 */
#include "sgx_device.h"

#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <xf86drm.h>

#include "drm-uapi/apple_sgx_drm.h"
#include "util/log.h"
#include "util/os_time.h"
#include "util/u_memory.h"

uint64_t
sgx_device_param(struct sgx_device *dev, uint32_t param)
{
   struct drm_apple_sgx_get_param p = { .param = param };

   if (drmIoctl(dev->fd, DRM_IOCTL_APPLE_SGX_GET_PARAM, &p))
      return 0;
   return p.value;
}

bool
sgx_device_init(struct sgx_device *dev, int fd)
{
   dev->fd = fd;
   if (sgx_device_param(dev, APPLE_SGX_PARAM_UAPI_VERSION) != 1) {
      mesa_loge("sgx: the kernel's render node interface is not version 1");
      return false;
   }
   dev->core_id = sgx_device_param(dev, APPLE_SGX_PARAM_CORE_ID);
   dev->core_rev = sgx_device_param(dev, APPLE_SGX_PARAM_CORE_REVISION);
   dev->num_cores = sgx_device_param(dev, APPLE_SGX_PARAM_NUM_CORES);
   dev->va_start = sgx_device_param(dev, APPLE_SGX_PARAM_VA_START);
   dev->va_end = sgx_device_param(dev, APPLE_SGX_PARAM_VA_END);
   dev->code_base = sgx_device_param(dev, APPLE_SGX_PARAM_CODE_BASE);
   dev->code_va_start = sgx_device_param(dev, APPLE_SGX_PARAM_CODE_VA_START);
   dev->code_va_end = sgx_device_param(dev, APPLE_SGX_PARAM_CODE_VA_END);
   dev->fb_va = sgx_device_param(dev, APPLE_SGX_PARAM_FB_VA);
   dev->fb_width = sgx_device_param(dev, APPLE_SGX_PARAM_FB_WIDTH);
   dev->fb_height = sgx_device_param(dev, APPLE_SGX_PARAM_FB_HEIGHT);
   dev->fb_stride = sgx_device_param(dev, APPLE_SGX_PARAM_FB_STRIDE);
   if (!sgx_device_param(dev, APPLE_SGX_PARAM_UKERNEL_UP))
      mesa_logw("sgx: the GPU's microkernel is not running (dmesg | grep apple-sgx)");
   return true;
}

struct sgx_bo *
sgx_bo_create(struct sgx_device *dev, uint32_t size, uint32_t flags, uint32_t va)
{
   struct drm_apple_sgx_gem_create c = {
      .size = size, .flags = flags, .va = va,
   };
   struct sgx_bo *bo;

   if (drmIoctl(dev->fd, DRM_IOCTL_APPLE_SGX_GEM_CREATE, &c)) {
      if (errno == EEXIST)
         mesa_loge("sgx: GPU 0x%08x is taken (another client of the template "
                   "frame, or memory debugfs mapped there)", va);
      else
         mesa_loge("sgx: GEM_CREATE %u bytes: %s", size, strerror(errno));
      return NULL;
   }
   bo = CALLOC_STRUCT(sgx_bo);
   if (!bo) {
      struct drm_gem_close cl = { .handle = c.handle };

      drmIoctl(dev->fd, DRM_IOCTL_GEM_CLOSE, &cl);
      return NULL;
   }
   bo->dev = dev;
   bo->handle = c.handle;
   bo->va = c.va;
   bo->size = (size + 0xfff) & ~0xfffu;
   return bo;
}

void *
sgx_bo_map(struct sgx_bo *bo)
{
   struct drm_apple_sgx_gem_mmap_offset m = { .handle = bo->handle };
   void *p;

   if (bo->map)
      return bo->map;
   if (drmIoctl(bo->dev->fd, DRM_IOCTL_APPLE_SGX_GEM_MMAP_OFFSET, &m))
      return NULL;
   p = mmap(NULL, bo->size, PROT_READ | PROT_WRITE, MAP_SHARED, bo->dev->fd,
            (off_t)m.offset);
   if (p == MAP_FAILED)
      return NULL;
   bo->map = p;
   return p;
}

void
sgx_bo_destroy(struct sgx_bo *bo)
{
   struct drm_gem_close cl;

   if (!bo)
      return;
   if (bo->map)
      munmap(bo->map, bo->size);
   cl = (struct drm_gem_close){ .handle = bo->handle };
   drmIoctl(bo->dev->fd, DRM_IOCTL_GEM_CLOSE, &cl);
   FREE(bo);
}

bool
sgx_bo_wait(struct sgx_bo *bo, int64_t timeout_ns)
{
   struct drm_apple_sgx_gem_wait w = {
      .handle = bo->handle,
      .timeout_ns = timeout_ns < 0 ? INT64_MAX :
                    timeout_ns ? os_time_get_absolute_timeout(timeout_ns) : 0,
   };

   return !drmIoctl(bo->dev->fd, DRM_IOCTL_APPLE_SGX_GEM_WAIT, &w);
}

struct sgx_fence *
sgx_fence_create(struct sgx_device *dev, bool signalled)
{
   struct sgx_fence *f = CALLOC_STRUCT(sgx_fence);

   if (!f)
      return NULL;
   if (drmSyncobjCreate(dev->fd, signalled ? DRM_SYNCOBJ_CREATE_SIGNALED : 0,
                        &f->syncobj)) {
      FREE(f);
      return NULL;
   }
   pipe_reference_init(&f->reference, 1);
   f->dev = dev;
   return f;
}

void
sgx_fence_reference(struct sgx_fence **ptr, struct sgx_fence *f)
{
   struct sgx_fence *old = *ptr;

   if (pipe_reference(old ? &old->reference : NULL, f ? &f->reference : NULL)) {
      drmSyncobjDestroy(old->dev->fd, old->syncobj);
      FREE(old);
   }
   *ptr = f;
}

bool
sgx_fence_wait(struct sgx_fence *f, uint64_t timeout_ns)
{
   int64_t abs = timeout_ns == OS_TIMEOUT_INFINITE ? INT64_MAX :
                 os_time_get_absolute_timeout(timeout_ns);

   return !drmSyncobjWait(f->dev->fd, &f->syncobj, 1, abs,
                          DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, NULL);
}

int
sgx_submit(struct sgx_device *dev, const uint32_t *cmd, uint32_t pb_va,
           uint32_t details_handle, uint32_t details_offset,
           const uint32_t *handles, unsigned count, struct sgx_fence *done)
{
   struct drm_apple_sgx_submit s = {
      .cmd = (uintptr_t)cmd,
      .cmd_size = cmd[0],
      .pb_va = pb_va,
      .details_handle = details_handle,
      .details_offset = details_offset,
      .bo_handles = (uintptr_t)handles,
      .bo_handle_count = count,
      .out_sync = done ? done->syncobj : 0,
   };

   if (drmIoctl(dev->fd, DRM_IOCTL_APPLE_SGX_SUBMIT, &s)) {
      mesa_loge("sgx: SUBMIT: %s", strerror(errno));
      return -errno;
   }
   return 0;
}
