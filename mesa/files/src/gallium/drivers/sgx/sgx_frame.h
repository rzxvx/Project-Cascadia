/*
 * Copyright 2026 Project Cascadia
 * SPDX-License-Identifier: MIT
 *
 * The template frame (docs/research/p105-mesa.md, M12): until the driver
 * builds renders of its own, it borrows sgx2d's pack (tools/sgx/rpack.py)
 * -- iOS's render target data for the full screen, the state blocks and
 * USSE programs of a textured quad -- and draws one full-screen quad with
 * it.  That is enough for a clear.
 */
#ifndef SGX_FRAME_H
#define SGX_FRAME_H

#include <stdbool.h>
#include <stdint.h>

struct sgx_device;
struct sgx_fence;
struct sgx_resource;

struct sgx_frame;

/* SGX_FRAME=a,b,...: pieces of the frame swapped for the pack's, or moved,
 * to find on the device which one is wrong (docs/research/p105-mesa.md,
 * M12; mesa/frame-bisect runs them all) */
enum {
   SGX_FRAME_FB = 1 << 0,     /* "fb": the pack's end of tile and background (the screen) */
   SGX_FRAME_BLEND = 1 << 1,  /* "blend": the pack's pixel program, texel x colour blended */
   SGX_FRAME_SCREEN = 1 << 2, /* "screen": ours, aimed at the framebuffer, not the target */
   SGX_FRAME_CODEBO = 1 << 3, /* "codebo": our programs in a buffer of their own */
   SGX_FRAME_SOP2 = 1 << 4,   /* "sop2": replace by SOP2 (cmod1, amod1), not MOV */
   SGX_FRAME_ALIGN = 1 << 5,  /* "align": render targets at 1 MiB-aligned addresses */
};
unsigned sgx_frame_options(void);

/* Loads the pack in packdir into buffers at the GPU addresses it was built
 * for; NULL (and a message) if there is none or the addresses are taken. */
struct sgx_frame *sgx_frame_create(struct sgx_device *dev, const char *packdir);
void sgx_frame_destroy(struct sgx_frame *f);

/* Whether a render through the frame can fill this surface: B8G8R8A8 or
 * B8G8R8X8, the pack's size (the screen's), level 0, one layer. */
bool sgx_frame_can_render(struct sgx_frame *f, struct sgx_resource *rt);

/* The whole of rt set to rgba, on the GPU; done is signalled when it is.
 * One render at a time: the caller serialises (sgx_screen.frame_lock). */
int sgx_frame_clear(struct sgx_frame *f, struct sgx_resource *rt, const float rgba[4],
                    struct sgx_fence *done);

#endif
