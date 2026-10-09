/*
 * Copyright 2026 Project Cascadia
 * SPDX-License-Identifier: MIT
 *
 * The render node (include/drm-uapi/apple_sgx_drm.h), thinly: what the
 * kernel reports, buffers, submits, fences.
 */
#ifndef SGX_DEVICE_H
#define SGX_DEVICE_H

#include <stdbool.h>
#include <stdint.h>

#include "util/list.h"
#include "util/simple_mtx.h"
#include "util/u_inlines.h"

struct hash_table_u64;

struct sgx_device {
   int fd;
   uint32_t uapi, core_id, core_rev, num_cores;
   uint32_t va_start, va_end;
   uint32_t code_base, code_va_start, code_va_end;
   uint32_t fb_va, fb_width, fb_height, fb_stride;
   uint32_t untiled_next;   /* sgx_bo_create, on kernels that pick tiled addresses */

   /* every buffer object by its handle: a dma-buf of ours imported again
    * comes back as the same handle, and has to be the same sgx_bo */
   simple_mtx_t bo_lock;
   struct hash_table_u64 *bos;

   /* fences the next submit waits for on the GPU (fence_server_sync: a
    * compositor's or the display's, through sync files) */
   uint32_t wait_syncs[16];
   unsigned nwait_syncs;

   /* buffer objects of buffers no resource holds any more, kept to be
    * used again once no render reads them (M26: a vertex buffer made anew
    * each frame cost an ioctl and the pages cleared and cleaned), oldest
    * first */
   simple_mtx_t cache_lock;
   struct list_head cache;
   uint64_t cache_size;
};

struct sgx_bo {
   struct sgx_device *dev;
   uint32_t handle;
   uint32_t va;      /* its GPU address */
   uint32_t size;
   uint8_t *map;     /* write-combined; NULL until sgx_bo_map() */
   unsigned refs;    /* references besides the first (sgx_bo_ref(), imports); under bo_lock */
   /* a buffer's (sgx_bo_cache_get()): back into the cache when the last
    * reference goes, when it went there, its place in it */
   bool cached;
   int64_t freed;
   struct list_head cache_link;
   /* the gathered draws that read it, if they are not rendered yet: their
    * context and batch number (sgx_draw.c) */
   const void *batch_ctx;
   unsigned batch_seq;
   /* a cached one's: the last render that listed it (under bo_lock) */
   struct sgx_fence *busy;
};

/* A fence: one syncobj, signalled when the render it was given to is done
 * (or created signalled). */
struct sgx_fence {
   struct pipe_reference reference;
   struct sgx_device *dev;
   uint32_t syncobj;
};

bool sgx_device_init(struct sgx_device *dev, int fd);
void sgx_device_fini(struct sgx_device *dev);
uint64_t sgx_device_param(struct sgx_device *dev, uint32_t param);

/* flags: APPLE_SGX_BO_*; va only with APPLE_SGX_BO_FIXED_VA */
struct sgx_bo *sgx_bo_create(struct sgx_device *dev, uint32_t size, uint32_t flags,
                             uint32_t va);
/* a handle on this device (from a dma-buf, say): its buffer object, the
 * one there is already if the handle has one (another reference to it);
 * NULL on a kernel without GEM_INFO */
struct sgx_bo *sgx_bo_import(struct sgx_device *dev, uint32_t handle);
/* drops a reference: the last one closes the handle (or puts a cached one
 * back in the cache) */
void sgx_bo_destroy(struct sgx_bo *bo);
void sgx_bo_ref(struct sgx_bo *bo);
/* a cached buffer object (sgx_bo_cache_get) listed by the render done
 * signals, and whether the last one that listed it is done -- without the
 * kernel's GEM_WAIT, whose zero timeout waits a jiffy for a busy one */
void sgx_bo_set_busy(struct sgx_bo *bo, struct sgx_fence *done);
bool sgx_bo_idle(struct sgx_bo *bo);
/* A mapped buffer object of at least size bytes for a buffer resource: the
 * cache's oldest of its size that no render reads any more, or a new one */
struct sgx_bo *sgx_bo_cache_get(struct sgx_device *dev, uint32_t size);
void *sgx_bo_map(struct sgx_bo *bo);
/* until no render that listed it is running (timeout_ns < 0: however long
 * it takes); false on timeout */
bool sgx_bo_wait(struct sgx_bo *bo, int64_t timeout_ns);

struct sgx_fence *sgx_fence_create(struct sgx_device *dev, bool signalled);
void sgx_fence_reference(struct sgx_fence **ptr, struct sgx_fence *f);
bool sgx_fence_wait(struct sgx_fence *f, uint64_t timeout_ns);
/* a sync file of the fence (-1: none), and a fence from one */
int sgx_fence_export(struct sgx_fence *f);
struct sgx_fence *sgx_fence_import(struct sgx_device *dev, int fd);
extern int64_t sgx_fence_waited;
/* SGX_TRACE=1: a line with the time since the last mark */
void sgx_trace_mark(const char *what);
enum { SGX_IOCTL_CREATE, SGX_IOCTL_MMAP, SGX_IOCTL_WAIT, SGX_IOCTL_SUBMIT, SGX_IOCTL_KINDS };
extern unsigned sgx_ioctls[SGX_IOCTL_KINDS];
/* SGX_DEBUG=fps: draws whose state was the last one's, vertices, textures */
extern unsigned sgx_stat_same, sgx_stat_verts, sgx_stat_tex_changes, sgx_stat_merged;

/* One render.  handles[] lists every buffer it touches (the render
 * details' among them); done is signalled when its 3D pass is over. */
int sgx_submit(struct sgx_device *dev, const uint32_t *cmd, uint32_t pb_va,
               uint32_t details_handle, uint32_t details_offset,
               const uint32_t *handles, unsigned count, struct sgx_fence *done);

#endif
