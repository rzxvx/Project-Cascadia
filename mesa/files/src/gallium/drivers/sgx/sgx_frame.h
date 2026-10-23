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

#include "util/format/u_formats.h"
#include <stdbool.h>
#include <stdint.h>

/* (M43) the constant buffers a stage may have: 0 the uniforms, 1..15
 * uniform buffers */
#define SGX_FRAME_MAX_UBOS 16

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
   SGX_FRAME_PACKRT = 1 << 8,  /* "packrt": the pack's render target data (its size only) */
   SGX_FRAME_PACK = 1 << 9,    /* "pack": the pack's buffers, at its addresses, as before
                                  M17 (one process at a time); fb, blend, packpixel and
                                  packrt need it */
   SGX_FRAME_BUILT = 1 << 10,  /* "built": the frame built on a kernel before UAPI 3 too,
                                  with the pack's parameter buffer */
};
unsigned sgx_frame_options(void);

/* Loads the pack in packdir into buffers at the GPU addresses it was built
 * for; NULL (and a message) if there is none or the addresses are taken. */
struct sgx_frame *sgx_frame_create(struct sgx_device *dev, const char *packdir);
void sgx_frame_destroy(struct sgx_frame *f);

/* A render's target: a level and a layer (a cube map's face) of a
 * resource (M29), and the render's area from its top left when not all of
 * it (w, h; 0: the level's) -- a framebuffer whose attachments differ in
 * size renders over what they have in common (M43) */
struct sgx_frame_target {
   struct sgx_resource *res;
   unsigned level, layer;
   unsigned w, h;
};

/* Whether a render through the frame can fill this surface: B8G8R8A8 or
 * B8G8R8X8, up to 4096 x 4096 (M10: the render target data for each size
 * are ours, sgx_rt.c), any level or face of a 2D texture or cube map. */
bool sgx_frame_can_render(struct sgx_frame *f, const struct sgx_frame_target *t);


/* (M43) What a render target's tile holds for each pixel: the program's
 * colour as RGBA bytes (o0); four half floats (o0, o1: R G, B A); two half
 * floats (o0: R G); an F32 (o0: R); two F32 (o0, o1: R G) */
enum sgx_rt_kind { SGX_RT_UNORM8, SGX_RT_F16X4, SGX_RT_F16X2, SGX_RT_F32, SGX_RT_F32X2 };
enum sgx_rt_kind sgx_frame_rt_kind(enum pipe_format format);

/* whether the tile holds 64 bits a pixel (o0 and o1) */
static inline bool
sgx_rt_kind_64(enum sgx_rt_kind k)
{
   return k == SGX_RT_F16X4 || k == SGX_RT_F32X2;
}

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
/* the iterate source of the pixel's position, window coordinates in F32
 * (iOS's gl_FragCoord: corpus v08, control word 0x0fc0d00f) */
#define SGX_ITERATE_POSITION 13
/* texture lookups the PDS makes for a pixel program (M35) */
#define SGX_FRAME_MAX_FETCH 4

/* (M37) the first ncolours varyings (two at most) are the TA's colours:
 * flat-shaded ones (iOS's es1 e01: the base colour, TSP varying 10, and
 * the offset colour, 11 -- the only ones the TSP shades flat), each after
 * the position in the vertex as the others are */
struct sgx_frame_layout {
   unsigned nvaryings;
   unsigned f32;
   unsigned colour;
   unsigned ncolours;
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
/* the uniform words a program may have in memory (M32): constant buffer
 * 0's, as the screen says them -- a vertex shader's 265 vec4s (M43) */
#define SGX_UBUF_MAX (265 * 4)

struct sgx_pixel_program {
   uint64_t *code;
   unsigned ncode;
   unsigned ntemps;
   unsigned ninputs;
   /* what the PDS iterates for each input: varying n of the layout, or
    * SGX_ITERATE_POSITION (gl_FragCoord); nvaryings of the layout's */
   uint8_t iter_src[SGX_FRAME_MAX_VARYINGS];
   unsigned nvaryings;
   /* (M37) the layout's first ncolours varyings are the TA's colours: an
    * input iterating one of them (iter_src < ncolours) takes one register,
    * its bytes as o0's, after the inputs of four (the inputs' last) */
   unsigned ncolours;
   unsigned nsa;
   /* uniform words in memory after the nsa (M32): the draw's words go on
    * with them, and sa word ubuf_sa has their address less 4 */
   unsigned nubuf, ubuf_sa;
   /* (M43) the uniform buffers it reads (ARB_uniform_buffer_object: bit k
    * for buffer k, 1..15), each one's address less 4 in sa word ubo_sa[k] */
   uint16_t ubo_mask;
   uint8_t ubo_sa[SGX_FRAME_MAX_UBOS];
   /* lookups the PDS makes before the program runs (M35): texture k's
    * texel, as it is (four bytes), into the primary attribute after the
    * inputs' (pa4 ninputs + k), the coordinates varying fetch_src[k]'s x
    * and y, the texture's four state words sa words fetch_sa[k].. (the
    * draw's) -- the PDS program then the draw's own */
   unsigned nfetch;
   uint8_t fetch_src[SGX_FRAME_MAX_FETCH];
   uint8_t fetch_sa[SGX_FRAME_MAX_FETCH];
   uint8_t fetch_proj;          /* (M39) lookup k projective: bit k (iOS's s08) */
   uint32_t code_va, pds_va;
   unsigned pds_rows;
   bool branches;               /* its pixels each on its own: the code branches, not
                                 * alike for all (M20, M33) */
   /* (M38) its inputs iterated and its lookups' texels fetched as F16, two
    * registers each (iOS's SDL sprite, corpus s00) */
   bool f16;
};

/* Triangles into rt, over what it holds (each tile starts as the target's
 * pixels): nverts vertices laid out as l says, three a triangle, as many
 * as sgx_frame_max_vertices() a render.  With prog, the pixels are its
 * (l has its inputs, all F32), with sa[0..prog->nsa) in sa0..; without,
 * they are l's colour varying.  handles: buffers the render reads besides
 * the frame's and rt (textures, at most SGX_FRAME_MAX_HANDLES), for the
 * kernel to keep. */
#define SGX_FRAME_MAX_HANDLES 256
/* What the ISP does with a draw's pixels (M14): the depth test (a
 * pipe_compare_func; ALWAYS when GL's test is off) and whether it writes
 * depth. */
struct sgx_frame_state {
   uint8_t depth_func;
   bool depth_write;
   /* the TA's viewport (M18): scale and translate, x y z, as gallium's;
    * else the target's whole, depth 0..1 (the draw module's vertices) */
   bool viewport;
   float scale[3], translate[3];
   uint8_t cull;            /* SGX_CULL_* */
   /* the stencil test (M23): ISP state B's word (state word 3; else the
    * template's, ALWAYS and KEEP) and the reference (state word 1's low
    * byte) */
   bool stencil_on;
   uint32_t stencil;
   uint8_t stencil_ref;
   /* (M37) flat shading of the layout's colours: state word 18 bits 18:17
    * as iOS's (es1 e01: 3) -- the TSP takes a triangle's last vertex's
    * colours with any value (gldesk: the first vertex's by turning the
    * triangle round, sgx_draw.c) -- 0 when none */
   uint8_t flat;
   /* (M37) polygon offset, the ISP's: state word 2 bits 25:21 the slope's
    * factor, 20:16 the units, each a signed 5-bit integer (iOS's es1 e19,
    * gloffset --probe); a unit is 32 of 24-bit depth's at depths 0.5..1
    * (a power of two smaller below: floating point) */
   uint32_t offset;
   /* (M37) the samples that pass counted: an occlusion query's draw (ISP
    * state A bit 14, iOS's m29; bit 18 would count only whether any did) */
   bool count;
};
#define SGX_FLAT_LAST  3
/* the TA's culling (state word 18, bits 1:0): the triangles clockwise or
 * anticlockwise in the target, row 0 at the top (M18) */
#define SGX_CULL_NONE 0
#define SGX_CULL_CW   1
#define SGX_CULL_CCW  2

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
struct sgx_vs;

/* With vs (M18: the vertex shader on the GPU), l is the varyings it hands
 * the pixels, vs_sa its uniforms, and its vertex fetch reads attribute n of
 * vertex i at base[n] + i x stride (M26: one stride for all) -- addresses
 * in the application's buffers, or, with vdata (nverts vertices the CPU
 * made one stream), offsets into a vertex there -- or with vdata_va, the
 * stream already in GPU memory there (M42: the caller lists its buffer
 * among the render's).  It draws count
 * vertices: indices[0..count) from vertex 0, or 0, 1, 2, ... without
 * (count at most sgx_frame_list_max()). */
struct sgx_frame_draw {
   struct sgx_frame_layout l;
   const float *verts;
   unsigned nverts;
   const struct sgx_pixel_program *prog;
   const uint32_t *sa;
   struct sgx_frame_state st;
   struct sgx_vs *vs;
   const uint32_t *vs_sa;
   const uint8_t *vdata;
   uint32_t vdata_va;
   const uint32_t *base;
   unsigned stride, count;
   const uint16_t *indices;
};
#define SGX_FRAME_MAX_DRAWS 2048
unsigned sgx_frame_max_draws(const struct sgx_frame *f);

/* the same ISP state and viewport (field by field: padding) */
static inline bool
sgx_frame_state_equal(const struct sgx_frame_state *x, const struct sgx_frame_state *y)
{
   return x->depth_func == y->depth_func && x->depth_write == y->depth_write &&
          x->viewport == y->viewport && x->cull == y->cull &&
          x->stencil_on == y->stencil_on && x->stencil == y->stencil &&
          x->stencil_ref == y->stencil_ref && x->flat == y->flat && x->offset == y->offset &&
          x->count == y->count &&
          (!x->viewport || (x->scale[0] == y->scale[0] && x->scale[1] == y->scale[1] &&
                            x->scale[2] == y->scale[2] && x->translate[0] == y->translate[0] &&
                            x->translate[1] == y->translate[1] &&
                            x->translate[2] == y->translate[2]));
}

/* the most vertices a draw takes without indices */
unsigned sgx_frame_list_max(const struct sgx_frame *f);

/* n 32-bit words the same (musl's memcmp goes a byte at a time) */
static inline bool
sgx_words_equal(const uint32_t *a, const uint32_t *b, unsigned n)
{
   for (unsigned i = 0; i < n; i++)
      if (a[i] != b[i])
         return false;
   return true;
}
/* the vertices and indices a render's draws take, all told, in bytes (its
 * own memory grows as they need: a bound, not a size) */
#define SGX_FRAME_MAX_BYTES (8u << 20)

/* the program's code and PDS into GPU memory, once (sgx_frame_draw does it
 * itself) */
int sgx_frame_upload(struct sgx_frame *f, struct sgx_pixel_program *p);
/* a vertex shader's code, once */
int sgx_frame_upload_vs(struct sgx_frame *f, struct sgx_vs *vs);
/* a program no render to be submitted from now on runs: its places free
 * again once the renders already submitted are done (and it is to be
 * uploaded again if drawn after all) */
void sgx_frame_retire(struct sgx_frame *f, struct sgx_pixel_program *p);
void sgx_frame_retire_vs(struct sgx_frame *f, struct sgx_vs *vs);
/* How the ISP keeps depth and stencil in memory: ZLSCTL's formats (bits
 * 25:24 stored, 22:21 loaded) */
enum sgx_zls_format {
   SGX_ZLS_F32 = 0,     /* F32 depth alone, 4 bytes a pixel */
   SGX_ZLS_D24S8 = 1,   /* 24-bit depth, the stencil in the top byte */
   /* (M43) F32 depth and the stencil, 5 bytes a pixel: the stencil, then
    * the depth as a key that sorts as an integer, most significant byte
    * first, in a stream each 16 bytes of which are stored reversed
    * (sgx_resource.c) */
   SGX_ZLS_F32S8 = 3,
};

static inline enum sgx_zls_format
sgx_zls_format(enum pipe_format f)
{
   return f == PIPE_FORMAT_Z32_FLOAT_S8X24_UINT ? SGX_ZLS_F32S8 :
          f == PIPE_FORMAT_Z24_UNORM_S8_UINT ? SGX_ZLS_D24S8 : SGX_ZLS_F32;
}

/* The depth buffer a render loads its tiles' depth from, stores it to, or
 * both (M24, the ISP's z load/store): 32 x 32 tiles, 4 KiB each (5 KiB
 * with SGX_ZLS_F32S8), rows of tiles an even number long
 * (sgx_frame_zls_size()) */
struct sgx_frame_zls {
   struct sgx_bo *bo;
   unsigned w;         /* the depth buffer's width, pixels */
   bool load;          /* else the tiles start at depth_clear */
   bool store;
   bool stencil;       /* stencil too */
   enum sgx_zls_format format;
};
uint32_t sgx_frame_zls_size(unsigned w, unsigned h, enum sgx_zls_format format);

/* n draws (at most SGX_FRAME_MAX_DRAWS) in one render into t, in order,
 * over what it holds: the depth test holds across them, from depth_clear
 * -- or from zls, when it loads.  zls NULL: no depth buffer.  What the
 * render reads besides rt and the frame's buffers -- its stream, the
 * draws' state, vertices and indices -- goes in memory of its own (M27):
 * it need not wait for the renders before it. */
/* (M37) vis: where the ISP counts the samples that pass of the draws whose
 * state says so (an occlusion query's), or NULL -- a word for each core,
 * SGX_VIS_CORE_STRIDE apart, the buffer zeroed beforehand */
#define SGX_VIS_CORE_STRIDE 0x640
#define SGX_VIS_SIZE (SGX_VIS_CORE_STRIDE + 4)
int sgx_frame_render(struct sgx_frame *f, const struct sgx_frame_target *t,
                     const struct sgx_frame_draw *draws, unsigned n, const uint32_t *handles,
                     unsigned nhandles, float depth_clear, const struct sgx_frame_zls *zls,
                     struct sgx_bo *vis, struct sgx_fence *done);
/* the most vertices a draw module draw (l) takes: sgx_frame_draw's, and a
 * gathered render's without indices */
unsigned sgx_frame_max_vertices(struct sgx_frame *f, const struct sgx_frame_layout *l);

#endif
