/* SPDX-License-Identifier: MIT */
/*
 * Apple S5L8940X (A5) PowerVR SGX543MP2: the render node's interface.
 *
 * EXPERIMENTAL.  This is the first interface Mesa and sgx2d talk to the GPU
 * through (docs/research/p105-mesa.md, M9); it will change while the Mesa
 * driver is written, and nothing outside this repository should depend on
 * it yet.  APPLE_SGX_PARAM_UAPI_VERSION says which revision a kernel has.
 *
 * The model, kept as small as the hardware allows:
 *
 *  - One GPU address space, shared by every client (the microkernel's own
 *    buffers are in it too, at 0x80000000).  Buffers are GEM objects mapped
 *    into it for their whole life; the kernel picks the address (64 KiB
 *    aligned for buffers of 64 KiB or more: a render target needs it), or
 *    the caller asks for one (APPLE_SGX_BO_FIXED_VA).  The GPU's own pointer
 *    encodings decide where things may go -- PDS data pointers only reach
 *    0x80000000 and up, and USSE code is addressed relative to a code base
 *    register -- so the usable range and the code zone are parameters.
 *
 *  - USE_CODE_BASE_3 and USE_CODE_BASE_5 (the ones iOS's GL driver uses)
 *    point at APPLE_SGX_PARAM_CODE_BASE.  A DOUTU word for code at GPU
 *    address A is ((A - CODE_BASE) / 8) << 4 | 3, the index in 20 bits:
 *    code has to be within 8 MiB of the base.  Buffers created with
 *    APPLE_SGX_BO_USSE_CODE land there (kernels before 2026-10-04 gave
 *    CODE_VA_END as base + 16 MiB and used the top of it: out of reach).
 *
 *  - A render (the TA, then the 3D pass) is submitted as the TA command iOS
 *    puts in a render context's CCB, built by the caller as for debugfs's
 *    "rkick" (docs/research/p105-gpu.md, M4): the kernel copies it into the
 *    render queue, points its completion at its own memory, writes the
 *    render context into the render details (+0x20, +0xa4), sets the
 *    parameter buffer descriptor, and kicks.  The job is done when the
 *    microkernel clears the render details' word at +0x24 after the 3D pass.
 *    Renders run one at a time, in submission order across all clients.
 *
 *  - Synchronisation is syncobjs: a submit waits for in_syncs and signals
 *    out_sync when its 3D pass is over.  Every buffer the render touches
 *    must be listed in bo_handles; they stay alive and mapped until then.
 */
#ifndef _APPLE_SGX_DRM_H_
#define _APPLE_SGX_DRM_H_

#include "drm.h"

#if defined(__cplusplus)
extern "C" {
#endif

#define DRM_APPLE_SGX_GET_PARAM			0x00
#define DRM_APPLE_SGX_GEM_CREATE		0x01
#define DRM_APPLE_SGX_GEM_MMAP_OFFSET		0x02
#define DRM_APPLE_SGX_GEM_WAIT			0x03
#define DRM_APPLE_SGX_SUBMIT			0x04
#define DRM_APPLE_SGX_GEM_INFO			0x05

#define DRM_IOCTL_APPLE_SGX_GET_PARAM \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_APPLE_SGX_GET_PARAM, struct drm_apple_sgx_get_param)
#define DRM_IOCTL_APPLE_SGX_GEM_CREATE \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_APPLE_SGX_GEM_CREATE, struct drm_apple_sgx_gem_create)
#define DRM_IOCTL_APPLE_SGX_GEM_MMAP_OFFSET \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_APPLE_SGX_GEM_MMAP_OFFSET, struct drm_apple_sgx_gem_mmap_offset)
#define DRM_IOCTL_APPLE_SGX_GEM_WAIT \
	DRM_IOW(DRM_COMMAND_BASE + DRM_APPLE_SGX_GEM_WAIT, struct drm_apple_sgx_gem_wait)
#define DRM_IOCTL_APPLE_SGX_SUBMIT \
	DRM_IOW(DRM_COMMAND_BASE + DRM_APPLE_SGX_SUBMIT, struct drm_apple_sgx_submit)
#define DRM_IOCTL_APPLE_SGX_GEM_INFO \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_APPLE_SGX_GEM_INFO, struct drm_apple_sgx_gem_info)

/* ---- GET_PARAM ---------------------------------------------------------- */

enum drm_apple_sgx_param {
	APPLE_SGX_PARAM_UAPI_VERSION = 0,	/* 2 (1: no GEM_INFO) */
	APPLE_SGX_PARAM_CORE_ID = 1,		/* MASTER_CORE_ID: 0x01194201 */
	APPLE_SGX_PARAM_CORE_REVISION = 2,	/* 0x00010202 = 1.2.2 */
	APPLE_SGX_PARAM_NUM_CORES = 3,		/* 2 */
	APPLE_SGX_PARAM_CLOCK_KHZ = 4,		/* the GPU clock */
	APPLE_SGX_PARAM_UKERNEL_UP = 5,		/* 1 while the microkernel runs */

	/* GPU addresses: [VA_START, VA_END) is where buffers may be mapped,
	 * [CODE_VA_START, CODE_VA_END) the code zone, CODE_BASE what
	 * USE_CODE_BASE_3 and _5 hold (see above). */
	APPLE_SGX_PARAM_VA_START = 6,
	APPLE_SGX_PARAM_VA_END = 7,
	APPLE_SGX_PARAM_CODE_BASE = 8,
	APPLE_SGX_PARAM_CODE_VA_START = 9,
	APPLE_SGX_PARAM_CODE_VA_END = 10,

	/* The framebuffer (simplefb's memory), mapped by the kernel for the 2D
	 * engine; a render can write straight into it.  0 if there is none.
	 * Linear a8r8g8b8; stride in bytes. */
	APPLE_SGX_PARAM_FB_VA = 11,
	APPLE_SGX_PARAM_FB_WIDTH = 12,
	APPLE_SGX_PARAM_FB_HEIGHT = 13,
	APPLE_SGX_PARAM_FB_STRIDE = 14,

	/* Renders submitted, completed, and timed out since the module loaded;
	 * microkernel (re)starts. */
	APPLE_SGX_PARAM_RENDERS_SUBMITTED = 15,
	APPLE_SGX_PARAM_RENDERS_DONE = 16,
	APPLE_SGX_PARAM_RENDERS_TIMED_OUT = 17,
	APPLE_SGX_PARAM_UKERNEL_BOOTS = 18,
};

struct drm_apple_sgx_get_param {
	__u32 param;		/* in: enum drm_apple_sgx_param */
	__u32 pad;		/* in: 0 */
	__u64 value;		/* out */
};

/* ---- GEM_CREATE --------------------------------------------------------- */

/* Map at drm_apple_sgx_gem_create.va instead of an address the kernel
 * picks.  Fails with EEXIST if anything is mapped there already. */
#define APPLE_SGX_BO_FIXED_VA			(1 << 0)
/* USSE code: in the code zone, where a DOUTU can reach it. */
#define APPLE_SGX_BO_USSE_CODE			(1 << 1)
/* The GPU may only read it (PTE READONLY). */
#define APPLE_SGX_BO_GPU_READONLY		(1 << 2)
/* Without the PTE's CACHECONSISTENT bit, which every other buffer has (the
 * microkernel only sees CPU writes to cache-consistent pages; the
 * framebuffer is mapped without it). */
#define APPLE_SGX_BO_NOT_CACHE_CONSISTENT	(1 << 3)

#define APPLE_SGX_BO_FLAGS			0xf

struct drm_apple_sgx_gem_create {
	__u64 size;		/* in: bytes, rounded up to 4 KiB */
	__u32 flags;		/* in: APPLE_SGX_BO_* */
	__u32 handle;		/* out */
	__u32 va;		/* in with APPLE_SGX_BO_FIXED_VA, out: the GPU address */
	__u32 pad;		/* in: 0 */
};

/* ---- GEM_MMAP_OFFSET: the offset to mmap() the buffer at (write-combined) */

struct drm_apple_sgx_gem_mmap_offset {
	__u32 handle;		/* in */
	__u32 flags;		/* in: 0 */
	__u64 offset;		/* out */
};

/* ---- GEM_WAIT: until no render that lists the buffer is running -------- */

struct drm_apple_sgx_gem_wait {
	__u32 handle;		/* in */
	__u32 pad;		/* in: 0 */
	__s64 timeout_ns;	/* in: absolute, CLOCK_MONOTONIC; 0 just polls */
};

/* ---- SUBMIT: one render, the TA and then the 3D pass -------------------- */

#define APPLE_SGX_TA_CMD_MIN			0x11c
#define APPLE_SGX_TA_CMD_MAX			0x200

struct drm_apple_sgx_submit {
	/* in: user pointer to the TA command, as iOS's render submit builds
	 * it (0x80bfca20): word 0 its size in bytes, a multiple of 8 between
	 * APPLE_SGX_TA_CMD_MIN and _MAX; the kernel rewrites words 0, 0x64,
	 * 0x68 and 0x6c. */
	__u64 cmd;
	__u32 cmd_size;		/* in: bytes copied from cmd; >= word 0 */
	__u32 flags;		/* in: 0 */

	__u32 pb_va;		/* in: the parameter buffer's descriptor */
	__u32 details_handle;	/* in: the buffer holding the render details */
	__u32 details_offset;	/* in: where in it, 4-byte aligned */
	__u32 cache_control;	/* in: SGXMKIF_CC_* to add to the kick, normally 0 */

	__u64 bo_handles;	/* in: user pointer to __u32 handles */
	__u32 bo_handle_count;
	__u32 in_sync_count;
	__u64 in_syncs;		/* in: user pointer to __u32 syncobj handles */
	__u32 out_sync;		/* in: syncobj to signal when it is done, or 0 */
	__u32 pad;		/* in: 0 */
};

/* ---- GEM_INFO: what a handle is, for one that came from a dma-buf ------ */

/* Every buffer is the render node's own (dma-bufs of other devices are not
 * taken), so a dma-buf's handle names a buffer already mapped: this says
 * where.  UAPI version 2 on. */
struct drm_apple_sgx_gem_info {
	__u32 handle;		/* in */
	__u32 flags;		/* out: the APPLE_SGX_BO_* it was made with */
	__u32 va;		/* out: its GPU address */
	__u32 pad;		/* in: 0 */
	__u64 size;		/* out: bytes */
};

#if defined(__cplusplus)
}
#endif

#endif /* _APPLE_SGX_DRM_H_ */
