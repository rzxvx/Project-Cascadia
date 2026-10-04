/*
 * Copyright 2026 Project Cascadia
 * SPDX-License-Identifier: MIT
 *
 * A pretend apple_sgx render node, for running the driver where there is no
 * iPad (Mesa's drm-shim):
 *
 *   LD_PRELOAD=libsgx_noop_drm_shim.so glclear
 *
 * Buffers are memory, renders do nothing and are done at once.  What it
 * reports is what the iPad mini's kernel reports, but without a
 * framebuffer.
 */
#include <stdio.h>
#include <stdlib.h>

#include "drm-shim/drm_shim.h"
#include "drm-uapi/apple_sgx_drm.h"
#include "util/u_math.h"

bool drm_shim_driver_prefers_first_render_node = true;

/* below the BIF's tiled window, as the kernel picks them since 2026-10-04 */
static uint32_t next_va = 0xe0000000, next_code = 0x9b000000;

static int
sgx_ioctl_noop(int fd, unsigned long request, void *arg)
{
   return 0;
}

static int
sgx_ioctl_get_param(int fd, unsigned long request, void *arg)
{
   struct drm_apple_sgx_get_param *gp = arg;

   switch (gp->param) {
   case APPLE_SGX_PARAM_UAPI_VERSION:  gp->value = 1; break;
   case APPLE_SGX_PARAM_CORE_ID:       gp->value = 0x01194201; break;
   case APPLE_SGX_PARAM_CORE_REVISION: gp->value = 0x00010202; break;
   case APPLE_SGX_PARAM_NUM_CORES:     gp->value = 2; break;
   case APPLE_SGX_PARAM_CLOCK_KHZ:     gp->value = 102600; break;
   case APPLE_SGX_PARAM_UKERNEL_UP:    gp->value = 1; break;
   case APPLE_SGX_PARAM_VA_START:      gp->value = 0x80800000; break;
   case APPLE_SGX_PARAM_VA_END:        gp->value = 0xf0000000; break;
   case APPLE_SGX_PARAM_CODE_BASE:
   case APPLE_SGX_PARAM_CODE_VA_START: gp->value = 0x9a000000; break;
   case APPLE_SGX_PARAM_CODE_VA_END:   gp->value = 0x9b000000; break;
   default:                            gp->value = 0; break;
   }
   return 0;
}

static int
sgx_ioctl_gem_create(int fd, unsigned long request, void *arg)
{
   struct drm_apple_sgx_gem_create *create = arg;
   struct shim_fd *shim_fd = drm_shim_fd_lookup(fd);
   struct shim_bo *bo = calloc(1, sizeof(*bo));
   size_t size = align(create->size, 4096);

   drm_shim_bo_init(bo, size);
   create->handle = drm_shim_bo_get_handle(shim_fd, bo);
   drm_shim_bo_put(bo);
   /* top down, a guard page each, as the kernel does it */
   if (!(create->flags & APPLE_SGX_BO_FIXED_VA)) {
      uint32_t *top = create->flags & APPLE_SGX_BO_USSE_CODE ? &next_code : &next_va;

      *top -= size + 4096;
      create->va = *top;
   }
   return 0;
}

static int
sgx_ioctl_gem_mmap_offset(int fd, unsigned long request, void *arg)
{
   struct drm_apple_sgx_gem_mmap_offset *m = arg;
   struct shim_fd *shim_fd = drm_shim_fd_lookup(fd);
   struct shim_bo *bo = drm_shim_bo_lookup(shim_fd, m->handle);

   if (!bo)
      return -1;
   m->offset = drm_shim_bo_get_mmap_offset(shim_fd, bo);
   drm_shim_bo_put(bo);
   return 0;
}

static ioctl_fn_t driver_ioctls[] = {
   [DRM_APPLE_SGX_GET_PARAM] = sgx_ioctl_get_param,
   [DRM_APPLE_SGX_GEM_CREATE] = sgx_ioctl_gem_create,
   [DRM_APPLE_SGX_GEM_MMAP_OFFSET] = sgx_ioctl_gem_mmap_offset,
   [DRM_APPLE_SGX_GEM_WAIT] = sgx_ioctl_noop,
   [DRM_APPLE_SGX_SUBMIT] = sgx_ioctl_noop,
};

void
drm_shim_driver_init(void)
{
   shim_device.bus_type = DRM_BUS_PLATFORM;
   shim_device.driver_name = "apple_sgx";
   shim_device.driver_ioctls = driver_ioctls;
   shim_device.driver_ioctl_count = ARRAY_SIZE(driver_ioctls);
   shim_device.version_major = 0;
   shim_device.version_minor = 1;
   shim_device.version_patchlevel = 0;

   drm_shim_override_file("DRIVER=apple-sgx\n"
                          "OF_FULLNAME=/arm-io/gpu@5100000\n"
                          "OF_COMPATIBLE_0=apple,s5l8940x-sgx\n"
                          "OF_COMPATIBLE_N=1\n",
                          "/sys/dev/char/%d:%d/device/uevent", DRM_MAJOR,
                          render_node_minor);
}
