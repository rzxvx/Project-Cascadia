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

#include "util/u_inlines.h"

struct sgx_device {
   int fd;
   uint32_t core_id, core_rev, num_cores;
   uint32_t va_start, va_end;
   uint32_t code_base, code_va_start, code_va_end;
   uint32_t fb_va, fb_width, fb_height, fb_stride;
};

struct sgx_bo {
   struct sgx_device *dev;
   uint32_t handle;
   uint32_t va;      /* its GPU address */
   uint32_t size;
   uint8_t *map;     /* write-combined; NULL until sgx_bo_map() */
};

/* A fence: one syncobj, signalled when the render it was given to is done
 * (or created signalled). */
struct sgx_fence {
   struct pipe_reference reference;
   struct sgx_device *dev;
   uint32_t syncobj;
};

bool sgx_device_init(struct sgx_device *dev, int fd);
uint64_t sgx_device_param(struct sgx_device *dev, uint32_t param);

/* flags: APPLE_SGX_BO_*; va only with APPLE_SGX_BO_FIXED_VA */
struct sgx_bo *sgx_bo_create(struct sgx_device *dev, uint32_t size, uint32_t flags,
                             uint32_t va);
void sgx_bo_destroy(struct sgx_bo *bo);
void *sgx_bo_map(struct sgx_bo *bo);
/* until no render that listed it is running (timeout_ns < 0: however long
 * it takes); false on timeout */
bool sgx_bo_wait(struct sgx_bo *bo, int64_t timeout_ns);

struct sgx_fence *sgx_fence_create(struct sgx_device *dev, bool signalled);
void sgx_fence_reference(struct sgx_fence **ptr, struct sgx_fence *f);
bool sgx_fence_wait(struct sgx_fence *f, uint64_t timeout_ns);

/* One render.  handles[] lists every buffer it touches (the render
 * details' among them); done is signalled when its 3D pass is over. */
int sgx_submit(struct sgx_device *dev, const uint32_t *cmd, uint32_t pb_va,
               uint32_t details_handle, uint32_t details_offset,
               const uint32_t *handles, unsigned count, struct sgx_fence *done);

#endif
