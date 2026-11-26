/* SPDX-License-Identifier: MIT */
/*
 * The iPad 7's GPU through the powervr UAPI (Cascadia's Apple backend,
 * devices/a10/docs/gpu.md "G7"): buffers in one VM, the code buffer at the
 * PDS and USC heaps' bases, render contexts, free list and render-target
 * datasets, render submission.
 */
#ifndef G9P_DEVICE_H
#define G9P_DEVICE_H

#include <stdbool.h>
#include <stdint.h>

#include "util/simple_mtx.h"

struct g9p_bo {
   uint32_t handle;
   uint64_t size;
   uint64_t va;          /* in the general heap */
   uint8_t *map;
};

/* the code buffer: static programs, shader code (kept), per-render
 * programs and their data (a ring, reset after each render: renders are
 * waited for) */
#define G9P_CODE_SIZE        (8u << 20)
#define G9P_CODE_SHADERS     0x10000u
#define G9P_CODE_FRAME       (4u << 20)

/* static programs, offsets in the code buffer = PDS / USC addresses */
#define G9P_PDS_VDM_SYNC     0x0040u   /* RGX+0x470's task (VDM sync static area) */
#define G9P_PDS_EOT_CODE     0x0080u   /* the end-of-tile PDS code (EOT static area) */
#define G9P_USC_EOT          0x0400u
#define G9P_USC_470          0x0500u
#define G9P_PDS_VS_KICK      0x0600u   /* DOUTW the vertex index -> r0, DOUTU */
#define G9P_PDS_DOUTD        0x0640u   /* DOUTD data q0 -> common store, HALT */

struct g9p_hwrt {
   unsigned w, h;
   uint32_t handle;
   struct g9p_bo *bo;    /* region headers, TPC, macrotile array, MLIST, VHEAP table */
};

struct g9p_device {
   int fd;
   uint64_t heap_gen, heap_gen_size, heap_pds, heap_usc;
   uint32_t vm;
   uint64_t va_next;
   uint32_t code_handle;
   uint8_t *code;              /* CPU map of the code buffer */
   uint32_t code_shaders;      /* next free shader byte */
   uint32_t code_frame;        /* next free per-render byte */
   uint32_t free_list;
   struct g9p_bo *fl_bo;
   uint32_t ctx;               /* render context */
   struct g9p_hwrt hwrt[8];
   unsigned nhwrt;
   uint32_t sync;              /* the render's syncobj */
   simple_mtx_t lock;
};

bool g9p_device_init(struct g9p_device *dev, int fd);
void g9p_device_fini(struct g9p_device *dev);

struct g9p_bo *g9p_bo_create(struct g9p_device *dev, uint64_t size);
void g9p_bo_destroy(struct g9p_device *dev, struct g9p_bo *bo);

/* code: kept (shaders) or for the next render; returns the offset (the
 * PDS / USC address), 0 when full */
uint32_t g9p_code_shader(struct g9p_device *dev, const void *code, unsigned size);
uint32_t g9p_code_frame(struct g9p_device *dev, unsigned size, unsigned al);
static inline void *g9p_code_ptr(struct g9p_device *dev, uint32_t off) { return dev->code + off; }

/* the render-target dataset for a size */
struct g9p_hwrt *g9p_hwrt_get(struct g9p_device *dev, unsigned w, unsigned h);

/* a geometry + fragment job pair, waited for */
bool g9p_render(struct g9p_device *dev, struct g9p_hwrt *rt, const void *geom, unsigned geom_len,
                const void *frag, unsigned frag_len);

#endif
