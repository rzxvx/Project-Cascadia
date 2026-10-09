/*
 * Copyright 2026 Project Cascadia
 * SPDX-License-Identifier: MIT
 */
#include "sgx_device.h"

#include <dlfcn.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <xf86drm.h>

#include "drm-uapi/apple_sgx_drm.h"
#include "util/hash_table.h"
#include "util/log.h"
#include "util/os_time.h"
#include "util/u_math.h"
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
   /* 2: GEM_INFO, for buffers that come as dma-bufs; 3: the kernel's
    * parameter buffer, the TA's heap */
   dev->uapi = sgx_device_param(dev, APPLE_SGX_PARAM_UAPI_VERSION);
   if (dev->uapi < 1 || dev->uapi > 3) {
      mesa_loge("sgx: the kernel's render node interface is version %u, not 1 to 3",
                dev->uapi);
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
   dev->untiled_next = UINT32_MAX;
   simple_mtx_init(&dev->bo_lock, mtx_plain);
   if (!(dev->bos = _mesa_hash_table_u64_create(NULL)))
      return false;
   if (!sgx_device_param(dev, APPLE_SGX_PARAM_UKERNEL_UP))
      mesa_logw("sgx: the GPU's microkernel is not running (dmesg | grep apple-sgx)");
   return true;
}

void
sgx_device_fini(struct sgx_device *dev)
{
   _mesa_hash_table_u64_destroy(dev->bos);
   simple_mtx_destroy(&dev->bo_lock);
}

/* SGX_NOCC=1: every buffer mapped for the GPU without the cache-consistent
 * bit -- the CPU writes them write-combined, past any cache, and a GPU read
 * that goes through one could see an older line (mesa/frame-bisect tries
 * it) */
static uint32_t
extra_bo_flags(void)
{
   static int flags = -1;

   if (flags < 0)
      flags = getenv("SGX_NOCC") && atoi(getenv("SGX_NOCC")) ?
              APPLE_SGX_BO_NOT_CACHE_CONSISTENT : 0;
   return flags;
}

/* The BIF's first tiled window (the kernel's apple_sgx.h): what the GPU
 * writes from here up the CPU reads back scrambled, in 256-byte x 16-line
 * tiles.  Kernels before 2026-10-04 gave out addresses from its top down
 * (docs/research/p105-mesa.md, M13a); a buffer put there is made again
 * below it, at an address of our own -- next fit, from where the last one
 * went. */
#define SGX_TILED_VA_START 0xe0000000u

static struct sgx_bo *
untiled_bo(struct sgx_device *dev, uint32_t size, uint32_t flags)
{
   uint32_t step = size >= 0x10000 ? 0x10000 : 0x1000;
   uint32_t top = (SGX_TILED_VA_START - align(size, 0x1000) - 0x1000) & ~(step - 1);
   uint32_t bottom = MAX2(dev->va_start, dev->code_va_end);
   uint32_t va = MIN2(dev->untiled_next, top) & ~(step - 1);

   for (unsigned wrapped = 0; wrapped < 2; wrapped++, va = top) {
      for (; va >= bottom && va <= top; va -= step) {
         struct sgx_bo *bo = sgx_bo_create(dev, size, flags | APPLE_SGX_BO_FIXED_VA, va);

         if (bo) {
            dev->untiled_next = va - 0x1000;
            return bo;
         }
         if (errno != EEXIST)
            return NULL;
      }
   }
   mesa_loge("sgx: no room for %u bytes below the tiled window", size);
   return NULL;
}

struct sgx_bo *
sgx_bo_create(struct sgx_device *dev, uint32_t size, uint32_t flags, uint32_t va)
{
   struct drm_apple_sgx_gem_create c = {
      .size = size, .flags = flags | extra_bo_flags(), .va = va,
   };
   struct sgx_bo *bo;

   /* a fixed address that is taken (EEXIST) is the caller's to report */
   sgx_ioctls[SGX_IOCTL_CREATE]++;
   if (drmIoctl(dev->fd, DRM_IOCTL_APPLE_SGX_GEM_CREATE, &c)) {
      if (errno != EEXIST)
         mesa_loge("sgx: GEM_CREATE %u bytes: %s", size, strerror(errno));
      return NULL;
   }
   if (!(flags & APPLE_SGX_BO_FIXED_VA) && c.va + size > SGX_TILED_VA_START) {
      struct drm_gem_close cl = { .handle = c.handle };

      drmIoctl(dev->fd, DRM_IOCTL_GEM_CLOSE, &cl);
      return untiled_bo(dev, size, flags);
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
   simple_mtx_lock(&dev->bo_lock);
   _mesa_hash_table_u64_insert(dev->bos, bo->handle, bo);
   simple_mtx_unlock(&dev->bo_lock);
   return bo;
}

struct sgx_bo *
sgx_bo_import(struct sgx_device *dev, uint32_t handle)
{
   struct drm_apple_sgx_gem_info info = { .handle = handle };
   struct sgx_bo *bo;

   simple_mtx_lock(&dev->bo_lock);
   if ((bo = _mesa_hash_table_u64_search(dev->bos, handle))) {
      bo->refs++;
      simple_mtx_unlock(&dev->bo_lock);
      return bo;
   }
   if (dev->uapi < 2 || drmIoctl(dev->fd, DRM_IOCTL_APPLE_SGX_GEM_INFO, &info) ||
       !(bo = CALLOC_STRUCT(sgx_bo))) {
      struct drm_gem_close cl = { .handle = handle };

      simple_mtx_unlock(&dev->bo_lock);
      mesa_logw_once("sgx: a buffer from elsewhere (a dma-buf) cannot be used%s",
                     dev->uapi < 2 ? ": the kernel has no GEM_INFO (before 2026-10-08)" : "");
      drmIoctl(dev->fd, DRM_IOCTL_GEM_CLOSE, &cl);
      return NULL;
   }
   bo->dev = dev;
   bo->handle = handle;
   bo->va = info.va;
   bo->size = info.size;
   _mesa_hash_table_u64_insert(dev->bos, handle, bo);
   simple_mtx_unlock(&dev->bo_lock);
   return bo;
}

void *
sgx_bo_map(struct sgx_bo *bo)
{
   struct drm_apple_sgx_gem_mmap_offset m = { .handle = bo->handle };
   void *p;

   if (bo->map)
      return bo->map;
   sgx_ioctls[SGX_IOCTL_MMAP]++;
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
   simple_mtx_lock(&bo->dev->bo_lock);
   if (bo->refs) {
      bo->refs--;
      simple_mtx_unlock(&bo->dev->bo_lock);
      return;
   }
   _mesa_hash_table_u64_remove(bo->dev->bos, bo->handle);
   simple_mtx_unlock(&bo->dev->bo_lock);
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

   sgx_ioctls[SGX_IOCTL_WAIT]++;
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

/* the time spent waiting for fences, all told, and the ioctls by kind
 * (SGX_DEBUG=fps) */
int64_t sgx_fence_waited;
unsigned sgx_ioctls[SGX_IOCTL_KINDS];
unsigned sgx_stat_same, sgx_stat_verts, sgx_stat_tex_changes, sgx_stat_merged;

bool
sgx_fence_wait(struct sgx_fence *f, uint64_t timeout_ns)
{
   static int trace = -1;
   int64_t abs = timeout_ns == OS_TIMEOUT_INFINITE ? INT64_MAX :
                 os_time_get_absolute_timeout(timeout_ns), t0 = os_time_get_nano();
   bool done = !drmSyncobjWait(f->dev->fd, &f->syncobj, 1, abs,
                               DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, NULL);

   sgx_fence_waited += os_time_get_nano() - t0;
   /* SGX_TRACE_WAITS: who waited a millisecond or more */
   if (trace < 0)
      trace = getenv("SGX_TRACE_WAITS") != NULL;
   if (trace && os_time_get_nano() - t0 > 1000000) {
      Dl_info di;
      void *ra = __builtin_return_address(0);

      if (dladdr(ra, &di))
         mesa_logi("sgx: waited %.1f ms for a fence, from %s+0x%lx", (os_time_get_nano() - t0) / 1e6,
                   di.dli_sname ? di.dli_sname : "?", (unsigned long)((char *)ra - (char *)di.dli_fbase));
   }
   return done;
}

/* SGX_CC: SGXMKIF_CC_* for every kick, on top of the MMU invalidation the
 * kernel adds after mapping changes -- 8 (INVAL_DATA) has the microkernel
 * drop the GPU's data caches, which know addresses, not buffers
 * (mesa/frame-bisect tries it) */
static uint32_t
submit_cache_control(void)
{
   static int cc = -1;

   if (cc < 0)
      cc = getenv("SGX_CC") ? strtoul(getenv("SGX_CC"), NULL, 0) & 0xff : 0;
   return cc;
}

int
sgx_submit(struct sgx_device *dev, const uint32_t *cmd, uint32_t pb_va,
           uint32_t details_handle, uint32_t details_offset,
           const uint32_t *handles, unsigned count, struct sgx_fence *done)
{
   struct drm_apple_sgx_submit s = {
      .cache_control = submit_cache_control(),
      .cmd = (uintptr_t)cmd,
      .cmd_size = cmd[0],
      .pb_va = pb_va,
      .details_handle = details_handle,
      .details_offset = details_offset,
      .bo_handles = (uintptr_t)handles,
      .bo_handle_count = count,
      .out_sync = done ? done->syncobj : 0,
      .in_syncs = (uintptr_t)dev->wait_syncs,
      .in_sync_count = dev->nwait_syncs,
   };
   int ret = 0;

   sgx_ioctls[SGX_IOCTL_SUBMIT]++;
   if (drmIoctl(dev->fd, DRM_IOCTL_APPLE_SGX_SUBMIT, &s)) {
      mesa_loge("sgx: SUBMIT: %s", strerror(errno));
      ret = -errno;
   }
   for (unsigned i = 0; i < dev->nwait_syncs; i++)
      drmSyncobjDestroy(dev->fd, dev->wait_syncs[i]);
   dev->nwait_syncs = 0;
   return ret;
}

void
sgx_trace_mark(const char *what)
{
   static int on = -1;
   static int64_t last;
   int64_t now;

   if (on < 0)
      on = getenv("SGX_TRACE") && atoi(getenv("SGX_TRACE"));
   if (!on)
      return;
   now = os_time_get_nano();
   mesa_logi("sgx: mark %8.3f (+%7.3f ms) %s", (now / 1000000) % 100000 / 1000.0,
             last ? (now - last) / 1e6 : 0.0, what);
   last = now;
}

int
sgx_fence_export(struct sgx_fence *f)
{
   int fd = -1;

   if (drmSyncobjExportSyncFile(f->dev->fd, f->syncobj, &fd))
      return -1;
   return fd;
}

struct sgx_fence *
sgx_fence_import(struct sgx_device *dev, int fd)
{
   struct sgx_fence *f = sgx_fence_create(dev, false);

   if (f && drmSyncobjImportSyncFile(dev->fd, f->syncobj, fd))
      sgx_fence_reference(&f, NULL);
   return f;
}
