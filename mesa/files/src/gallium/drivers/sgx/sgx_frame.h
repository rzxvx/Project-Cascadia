/*
 * Copyright 2026 Project Cascadia
 * SPDX-License-Identifier: MIT
 *
 * The template frame (docs/research/p105-mesa.md, M12): until the driver
 * builds renders of its own, it borrows sgx2d's pack (tools/sgx/rpack.py)
 * -- iOS's render target data for the full screen, the state blocks and
 * USSE programs of a textured quad.  A clear is the pack's quad as it is
 * (texel x colour with a white texture).  A draw has a vertex side of our
 * own (M13b: vertex fetch, vertex program, the state words that describe
 * the varyings) and iOS's pixel side for gl_FragColor = v (M13a).
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
   SGX_FRAME_PACKPIX = 1 << 6, /* "packpixel": draws through the pack's pixel side too
                                  (and its vertex side, which that needs) */
   SGX_FRAME_PACKVTX = 1 << 7, /* "packvertex": draws through the pack's vertex side */
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

/* A vertex as a draw hands it over (M13b): the position, x y z w -- x and y
 * in [-1, 1] over the whole target, -1 being its first row and column (z
 * and w are not used yet: 0 and 1) -- then nvaryings varyings of four
 * floats each.  Each is kept for the pixels as F16, or as F32 if its bit in
 * f32 is set; varying colour is the pixel's colour, interpolated between
 * the vertices and packed to 8 bits a channel, the way iOS's GL driver
 * colours gl_FragColor = v (the corpus's v00_vec4 and v07_highp). */
#define SGX_FRAME_MAX_VARYINGS 8
struct sgx_frame_layout {
   unsigned nvaryings;
   unsigned f32;
   unsigned colour;
};

static inline unsigned
sgx_frame_vertex_floats(const struct sgx_frame_layout *l)
{
   return 4 * (1 + l->nvaryings);
}

/* A pixel program of our own (M13c, sgx_compiler.c): its USSE code, the
 * temporaries it uses, the varyings it reads -- F32 vec4s the PDS iterates
 * into pa0.., varying i of the layout into pa4i -- and how many words of
 * secondary attributes it reads from sa0.. (uniforms, texture states: the
 * draw hands them over).  The frame puts the code and its PDS program in
 * GPU memory at the first draw, once, and keeps where in code_va, pds_va. */
struct sgx_pixel_program {
   uint64_t *code;
   unsigned ncode;
   unsigned ntemps;
   unsigned ninputs;
   unsigned nsa;
   uint32_t code_va, pds_va;
   unsigned pds_rows;
};

/* Triangles into rt, over what it holds (each tile starts as the target's
 * pixels): nverts vertices laid out as l says, three a triangle, as many
 * as sgx_frame_max_vertices() a render.  With prog, the pixels are its
 * (l has its inputs, all F32), with sa[0..prog->nsa) in sa0..; without,
 * they are l's colour varying.  handles: buffers the render reads besides
 * the frame's and rt (textures, at most SGX_FRAME_MAX_HANDLES), for the
 * kernel to keep. */
#define SGX_FRAME_MAX_HANDLES 16
/* What the ISP does with a draw's pixels (M14): the depth test (a
 * pipe_compare_func; ALWAYS when GL's test is off) and whether it writes
 * depth. */
struct sgx_frame_state {
   uint8_t depth_func;
   bool depth_write;
};

int sgx_frame_draw(struct sgx_frame *f, struct sgx_resource *rt,
                   const struct sgx_frame_layout *l, const float *verts, unsigned nverts,
                   struct sgx_pixel_program *prog, const uint32_t *sa,
                   const uint32_t *handles, unsigned nhandles,
                   const struct sgx_frame_state *st, struct sgx_fence *done);

/* waits for the last render through the frame */
void sgx_frame_finish(struct sgx_frame *f);

/* One draw of a render (M14: draws are gathered into renders): vertices
 * laid out as l says, the pixels prog's (uploaded with sgx_frame_upload,
 * reading sa) or, without, l's colour iterated; what the ISP does. */
struct sgx_frame_draw {
   struct sgx_frame_layout l;
   const float *verts;
   unsigned nverts;
   const struct sgx_pixel_program *prog;
   const uint32_t *sa;
   struct sgx_frame_state st;
};
#define SGX_FRAME_MAX_DRAWS 200

/* the program's code and PDS into GPU memory, once (sgx_frame_draw does it
 * itself) */
int sgx_frame_upload(struct sgx_frame *f, struct sgx_pixel_program *p);

/* Room for a draw's vertices among a render's: *cursor (0 for the first)
 * moves past them; false when they do not fit. */
bool sgx_frame_place(struct sgx_frame *f, unsigned *cursor, const struct sgx_frame_layout *l,
                     unsigned nverts, unsigned *first);

/* n draws (at most SGX_FRAME_MAX_DRAWS, their vertices placed one after
 * another as sgx_frame_place says) in one render into rt, in order, over
 * what it holds: the depth test holds across them, from depth_clear. */
int sgx_frame_render(struct sgx_frame *f, struct sgx_resource *rt,
                     const struct sgx_frame_draw *draws, unsigned n, const uint32_t *handles,
                     unsigned nhandles, float depth_clear, struct sgx_fence *done);
unsigned sgx_frame_max_vertices(struct sgx_frame *f, const struct sgx_frame_layout *l);

#endif
