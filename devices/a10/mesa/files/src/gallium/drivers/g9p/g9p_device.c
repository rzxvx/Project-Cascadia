/* SPDX-License-Identifier: MIT */
/*
 * The powervr UAPI as tools/gpu/g9p-render.c uses it (devices/a10/docs/gpu.md
 * "G7 step 5", "G8.1"), for the Gallium driver.
 */
#include "g9p_device.h"

#include <errno.h>
#include <inttypes.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>

#include "drm-uapi/drm.h"
#include "drm-uapi/pvr_drm.h"
#include "util/log.h"
#include "util/macros.h"
#include "util/u_math.h"
#include "util/u_memory.h"

static int
ioc(struct g9p_device *dev, unsigned long req, void *arg)
{
   int r;

   do {
      r = ioctl(dev->fd, req, arg);
   } while (r == -1 && (errno == EINTR || errno == EAGAIN));
   return r;
}

static bool
vm_map(struct g9p_device *dev, uint32_t handle, uint64_t va, uint64_t size)
{
   struct drm_pvr_ioctl_vm_map_args m = { .vm_context_handle = dev->vm, .handle = handle,
                                          .device_addr = va, .size = size };

   if (ioc(dev, DRM_IOCTL_PVR_VM_MAP, &m)) {
      mesa_loge("g9p: VM_MAP %#llx +%#llx: %s", (unsigned long long)va, (unsigned long long)size,
                strerror(errno));
      return false;
   }
   return true;
}

static void
vm_unmap(struct g9p_device *dev, uint64_t va, uint64_t size)
{
   struct drm_pvr_ioctl_vm_unmap_args u = { .vm_context_handle = dev->vm, .device_addr = va, .size = size };

   ioc(dev, DRM_IOCTL_PVR_VM_UNMAP, &u);
}

/* a GEM object of `size` bytes, CPU-mapped; *handle */
static void *
gem_create(struct g9p_device *dev, uint64_t size, uint32_t *handle)
{
   struct drm_pvr_ioctl_create_bo_args c = { .size = size, .flags = DRM_PVR_BO_ALLOW_CPU_USERSPACE_ACCESS };
   struct drm_pvr_ioctl_get_bo_mmap_offset_args o = { 0 };
   struct drm_gem_close cl = { 0 };
   void *p;

   if (ioc(dev, DRM_IOCTL_PVR_CREATE_BO, &c))
      return NULL;
   o.handle = c.handle;
   if (!ioc(dev, DRM_IOCTL_PVR_GET_BO_MMAP_OFFSET, &o)) {
      p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, dev->fd, o.offset);
      if (p != MAP_FAILED) {
         *handle = c.handle;
         return p;
      }
   }
   cl.handle = c.handle;
   ioc(dev, DRM_IOCTL_GEM_CLOSE, &cl);
   return NULL;
}

struct g9p_bo *
g9p_bo_create(struct g9p_device *dev, uint64_t size)
{
   struct g9p_bo *bo = CALLOC_STRUCT(g9p_bo);

   if (!bo)
      return NULL;
   /* (64 KiB: the kernel's 16 KiB pages and the GPU's 4 KiB both) */
   bo->size = align64(MAX2(size, 1), 0x10000);
   if (!(bo->map = gem_create(dev, bo->size, &bo->handle))) {
      mesa_logw("g9p: a buffer of %" PRIu64 " bytes not created", bo->size);
      FREE(bo);
      return NULL;
   }
   simple_mtx_lock(&dev->lock);
   bo->va = dev->va_next;
   dev->va_next += bo->size;
   simple_mtx_unlock(&dev->lock);
   if (bo->va + bo->size > dev->heap_gen + dev->heap_gen_size || !vm_map(dev, bo->handle, bo->va, bo->size)) {
      struct drm_gem_close cl = { .handle = bo->handle };

      mesa_logw("g9p: a buffer of %" PRIu64 " bytes not mapped at %#" PRIx64, bo->size, bo->va);
      munmap(bo->map, bo->size);
      ioc(dev, DRM_IOCTL_GEM_CLOSE, &cl);
      FREE(bo);
      return NULL;
   }
   return bo;
}

void
g9p_bo_destroy(struct g9p_device *dev, struct g9p_bo *bo)
{
   struct drm_gem_close cl;

   if (!bo)
      return;
   vm_unmap(dev, bo->va, bo->size);
   munmap(bo->map, bo->size);
   cl = (struct drm_gem_close){ .handle = bo->handle };
   ioc(dev, DRM_IOCTL_GEM_CLOSE, &cl);
   FREE(bo);
}

uint32_t
g9p_code_shader(struct g9p_device *dev, const void *code, unsigned size)
{
   uint32_t at;

   simple_mtx_lock(&dev->lock);
   at = align(dev->code_shaders, 256);
   if (at + size > G9P_CODE_FRAME) {
      simple_mtx_unlock(&dev->lock);
      mesa_loge("g9p: shader code space full");
      return 0;
   }
   dev->code_shaders = at + size;
   simple_mtx_unlock(&dev->lock);
   memcpy(dev->code + at, code, size);
   return at;
}

uint32_t
g9p_code_frame(struct g9p_device *dev, unsigned size, unsigned al)
{
   uint32_t at = align(dev->code_frame, al);

   if (at + size > G9P_CODE_SIZE) {
      mesa_loge("g9p: per-render code space full");
      return 0;
   }
   dev->code_frame = at + size;
   return at;
}

/* the programs every render uses, at their fixed places */
static void
static_programs(struct g9p_device *dev)
{
   uint32_t *c = (uint32_t *)dev->code;
   static const uint16_t eot[] = { 0x0198, 0x0100, 0x0101, 0x0120, 0x0154, 0x0038,
                                   0x011c, 0x4100, 0x0800, 0x8000 };
   static const uint8_t end470[] = { 0x5e, 0x01, 0x00, 0xa0 };

   for (unsigned i = 0; i < G9P_CODE_SHADERS / 4; i++)
      c[i] = 0xd3000000;                                  /* PDS HALT */
   memcpy(dev->code + G9P_USC_EOT, eot, sizeof(eot));
   memcpy(dev->code + G9P_USC_470, end470, sizeof(end470));
   /* VDM sync: data {DOUTU -> the 0x470 task}, code at +0x20 */
   c[G9P_PDS_VDM_SYNC / 4] = G9P_USC_470;
   c[G9P_PDS_VDM_SYNC / 4 + 1] = 0x10;
   for (unsigned i = 2; i < 8; i++)
      c[G9P_PDS_VDM_SYNC / 4 + i] = 0;
   c[(G9P_PDS_VDM_SYNC + 0x20) / 4] = 0xf4000002;
   c[(G9P_PDS_VDM_SYNC + 0x24) / 4] = 0xd3000000;
   /* end of tile: DOUTD the image state, DOUTU the store program, HALT */
   c[G9P_PDS_EOT_CODE / 4] = 0xf0000800;
   c[G9P_PDS_EOT_CODE / 4 + 1] = 0xf4000012;
   c[G9P_PDS_EOT_CODE / 4 + 2] = 0xd3000000;
   /* the vertex kick: DOUTW the vertex index -> r0, DOUTU (data q0) */
   c[G9P_PDS_VS_KICK / 4] = 0xf0000a01;
   c[G9P_PDS_VS_KICK / 4 + 1] = 0xfc000002;
   c[G9P_PDS_VS_KICK / 4 + 2] = 0xd3000000;
   /* DOUTD (data q0: source, control) */
   c[G9P_PDS_DOUTD / 4] = 0xf4000800;
   c[G9P_PDS_DOUTD / 4 + 1] = 0xd3000000;
}

bool
g9p_device_init(struct g9p_device *dev, int fd)
{
   struct drm_pvr_ioctl_dev_query_args q = { 0 };
   struct drm_pvr_dev_query_heap_info hi = { 0 };
   struct drm_pvr_heap heaps[DRM_PVR_HEAP_COUNT];
   struct drm_pvr_ioctl_create_vm_context_args vm = { 0 };
   struct drm_pvr_ioctl_create_free_list_args fla = { 0 };
   struct drm_pvr_ioctl_create_context_args ctx = { 0 };
   struct drm_syncobj_create sc = { 0 };

   memset(dev, 0, sizeof(*dev));
   dev->fd = fd;
   simple_mtx_init(&dev->lock, mtx_plain);
   q.type = DRM_PVR_DEV_QUERY_HEAP_INFO_GET;
   q.pointer = (uintptr_t)&hi;
   q.size = sizeof(hi);
   hi.heaps.stride = sizeof(heaps[0]);
   hi.heaps.count = DRM_PVR_HEAP_COUNT;
   hi.heaps.array = (uintptr_t)heaps;
   if (ioc(dev, DRM_IOCTL_PVR_DEV_QUERY, &q))
      return false;
   dev->heap_gen = heaps[DRM_PVR_HEAP_GENERAL].base;
   dev->heap_gen_size = heaps[DRM_PVR_HEAP_GENERAL].size;
   dev->heap_pds = heaps[DRM_PVR_HEAP_PDS_CODE_DATA].base;
   dev->heap_usc = heaps[DRM_PVR_HEAP_USC_CODE].base;
   if (heaps[DRM_PVR_HEAP_PDS_CODE_DATA].size < G9P_CODE_SIZE ||
       heaps[DRM_PVR_HEAP_USC_CODE].size < G9P_CODE_SIZE)
      return false;
   dev->va_next = dev->heap_gen + 0x100000;

   if (ioc(dev, DRM_IOCTL_PVR_CREATE_VM_CONTEXT, &vm))
      return false;
   dev->vm = vm.handle;
   if (!(dev->code = gem_create(dev, G9P_CODE_SIZE, &dev->code_handle)) ||
       !vm_map(dev, dev->code_handle, dev->heap_pds, G9P_CODE_SIZE) ||
       !vm_map(dev, dev->code_handle, dev->heap_usc, G9P_CODE_SIZE))
      return false;
   static_programs(dev);
   dev->code_shaders = G9P_CODE_SHADERS;
   dev->code_frame = G9P_CODE_FRAME;

   /* one free list for every render target: 4 MiB of parameter memory */
   if (!(dev->fl_bo = g9p_bo_create(dev, 0x10000)))
      return false;
   fla.free_list_gpu_addr = dev->fl_bo->va;
   fla.initial_num_pages = fla.max_num_pages = 1024;
   fla.vm_context_handle = dev->vm;
   if (ioc(dev, DRM_IOCTL_PVR_CREATE_FREE_LIST, &fla)) {
      mesa_loge("g9p: CREATE_FREE_LIST: %s", strerror(errno));
      return false;
   }
   dev->free_list = fla.handle;
   ctx.type = DRM_PVR_CTX_TYPE_RENDER;
   ctx.vm_context_handle = dev->vm;
   if (ioc(dev, DRM_IOCTL_PVR_CREATE_CONTEXT, &ctx))
      return false;
   dev->ctx = ctx.handle;
   if (ioc(dev, DRM_IOCTL_SYNCOBJ_CREATE, &sc))
      return false;
   dev->sync = sc.handle;
   return true;
}

void
g9p_device_fini(struct g9p_device *dev)
{
   for (unsigned i = 0; i < dev->nhwrt; i++) {
      struct drm_pvr_ioctl_destroy_hwrt_dataset_args d = { .handle = dev->hwrt[i].handle };

      ioc(dev, DRM_IOCTL_PVR_DESTROY_HWRT_DATASET, &d);
      g9p_bo_destroy(dev, dev->hwrt[i].bo);
   }
   if (dev->ctx) {
      struct drm_pvr_ioctl_destroy_context_args d = { .handle = dev->ctx };
      ioc(dev, DRM_IOCTL_PVR_DESTROY_CONTEXT, &d);
   }
   if (dev->free_list) {
      struct drm_pvr_ioctl_destroy_free_list_args d = { .handle = dev->free_list };
      ioc(dev, DRM_IOCTL_PVR_DESTROY_FREE_LIST, &d);
   }
   g9p_bo_destroy(dev, dev->fl_bo);
   if (dev->code)
      munmap(dev->code, G9P_CODE_SIZE);
   simple_mtx_destroy(&dev->lock);
}

struct g9p_hwrt *
g9p_hwrt_get(struct g9p_device *dev, unsigned w, unsigned h)
{
   struct drm_pvr_ioctl_create_hwrt_dataset_args hw = { 0 };
   struct g9p_hwrt *rt;

   for (unsigned i = 0; i < dev->nhwrt; i++)
      if (dev->hwrt[i].w == w && dev->hwrt[i].h == h)
         return &dev->hwrt[i];
   if (dev->nhwrt == ARRAY_SIZE(dev->hwrt)) {
      /* the oldest goes */
      struct drm_pvr_ioctl_destroy_hwrt_dataset_args d = { .handle = dev->hwrt[0].handle };

      ioc(dev, DRM_IOCTL_PVR_DESTROY_HWRT_DATASET, &d);
      g9p_bo_destroy(dev, dev->hwrt[0].bo);
      memmove(dev->hwrt, dev->hwrt + 1, sizeof(dev->hwrt[0]) * --dev->nhwrt);
   }
   rt = &dev->hwrt[dev->nhwrt];
   memset(rt, 0, sizeof(*rt));
   /* region headers +0, TPC +0x40000, macrotile array +0x80000, MLIST
    * +0x90000, VHEAP table +0xa0000 (G7's sizes: enough for the screen) */
   if (!(rt->bo = g9p_bo_create(dev, 0x100000)))
      return NULL;
   memset(rt->bo->map, 0, rt->bo->size);
   hw.geom_data_args.tpc_dev_addr = rt->bo->va + 0x40000;
   hw.geom_data_args.tpc_size = 0x40000;
   hw.geom_data_args.vheap_table_dev_addr = rt->bo->va + 0xa0000;
   for (unsigned i = 0; i < 2; i++) {
      hw.rt_data_args[i].pm_mlist_dev_addr = rt->bo->va + 0x90000;
      hw.rt_data_args[i].macrotile_array_dev_addr = rt->bo->va + 0x80000;
      hw.rt_data_args[i].region_header_dev_addr = rt->bo->va;
   }
   hw.free_list_handles[0] = dev->free_list;
   hw.width = w;
   hw.height = h;
   hw.samples = hw.layers = 1;
   if (ioc(dev, DRM_IOCTL_PVR_CREATE_HWRT_DATASET, &hw)) {
      mesa_loge("g9p: CREATE_HWRT_DATASET %ux%u: %s", w, h, strerror(errno));
      g9p_bo_destroy(dev, rt->bo);
      return NULL;
   }
   rt->w = w;
   rt->h = h;
   rt->handle = hw.handle;
   dev->nhwrt++;
   return rt;
}

bool
g9p_render(struct g9p_device *dev, struct g9p_hwrt *rt, const void *geom, unsigned geom_len,
           const void *frag, unsigned frag_len)
{
   struct drm_pvr_job jobs[2] = { 0 };
   struct drm_pvr_sync_op op = { .handle = dev->sync, .flags = DRM_PVR_SYNC_OP_FLAG_SIGNAL };
   struct drm_pvr_ioctl_submit_jobs_args sub = { 0 };
   struct drm_syncobj_wait sw = { 0 };
   struct timespec t;

   for (unsigned i = 0; i < 2; i++) {
      jobs[i].type = i ? DRM_PVR_JOB_TYPE_FRAGMENT : DRM_PVR_JOB_TYPE_GEOMETRY;
      jobs[i].context_handle = dev->ctx;
      jobs[i].cmd_stream = (uintptr_t)(i ? frag : geom);
      jobs[i].cmd_stream_len = i ? frag_len : geom_len;
      jobs[i].hwrt.set_handle = rt->handle;
      jobs[i].hwrt.data_index = 0;
   }
   jobs[1].sync_ops = (struct drm_pvr_obj_array){ .stride = sizeof(op), .count = 1, .array = (uintptr_t)&op };
   sub.jobs = (struct drm_pvr_obj_array){ .stride = sizeof(jobs[0]), .count = 2, .array = (uintptr_t)jobs };
   if (ioc(dev, DRM_IOCTL_PVR_SUBMIT_JOBS, &sub)) {
      mesa_loge("g9p: SUBMIT_JOBS: %s", strerror(errno));
      return false;
   }
   clock_gettime(CLOCK_MONOTONIC, &t);
   sw.handles = (uintptr_t)&dev->sync;
   sw.count_handles = 1;
   sw.timeout_nsec = t.tv_sec * 1000000000LL + t.tv_nsec + 10000000000LL;
   sw.flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT;
   if (ioc(dev, DRM_IOCTL_SYNCOBJ_WAIT, &sw)) {
      mesa_loge("g9p: render wait: %s", strerror(errno));
      return false;
   }
   return true;
}
