/* SPDX-License-Identifier: MIT */
/*
 * The context: state objects, shader variants and the render. A render is
 * the frame of tools/gpu/g9p-render.c (devices/a10/docs/gpu.md "G8.1",
 * "G9") for any target size: the background object clears, each draw puts
 * its vertex / PDS state and PPP blocks into one VDM stream, the end of
 * tile writes the BGRA8 target. Renders are sent at a flush (or when the
 * target changes, or before a CPU map) and waited for.
 */
#include "g9p_context.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler/nir/nir.h"
#include "compiler/nir/nir_serialize.h"
#include "pipe/p_screen.h"
#include "util/format/u_format.h"
#include "util/log.h"
#include "util/u_framebuffer.h"
#include "util/u_helpers.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"
#include "util/u_surface.h"
#include "util/u_upload_mgr.h"
#include "util/blend.h"

#include "g9p_resource.h"
#include "g9p_screen.h"

#define FRAME_BO_SIZE (16u << 20)

/* ---- fences: renders are waited for, so every fence is signalled ------- */

struct g9p_fence {
   struct pipe_reference reference;
};

static void
g9p_fence_reference(struct pipe_screen *s, struct pipe_fence_handle **ptr, struct pipe_fence_handle *f)
{
   struct g9p_fence *old = (struct g9p_fence *)*ptr;

   if (pipe_reference(old ? &old->reference : NULL, f ? &((struct g9p_fence *)f)->reference : NULL))
      FREE(old);
   *ptr = f;
}

static bool
g9p_fence_finish(struct pipe_screen *s, struct pipe_context *ctx, struct pipe_fence_handle *f, uint64_t timeout)
{
   return true;
}

void
g9p_context_screen_init(struct pipe_screen *pscreen)
{
   pscreen->fence_reference = g9p_fence_reference;
   pscreen->fence_finish = g9p_fence_finish;
}

/* ---- the per-render data buffer ------------------------------------------------ */

static uint32_t
frame_alloc(struct g9p_context *ctx, unsigned size, unsigned al)
{
   struct g9p_frame *f = &ctx->frame;
   uint32_t at = align(f->pos, al);

   if (at + size > f->bo->size) {
      mesa_loge("g9p: a render's data past %u MiB (to do: split it)", FRAME_BO_SIZE >> 20);
      return 0;
   }
   f->pos = at + size;
   return at;
}

static inline void *frame_ptr(struct g9p_context *ctx, uint32_t off) { return ctx->frame.bo->map + off; }
static inline uint64_t frame_va(struct g9p_context *ctx, uint32_t off) { return ctx->frame.bo->va + off; }

static void
stream_put(struct g9p_context *ctx, const uint32_t *w, unsigned n)
{
   struct g9p_frame *f = &ctx->frame;

   memcpy(f->stream + f->nstream, w, 4 * n);
   f->nstream += n;
}

#define STREAM(ctx, ...) do { const uint32_t w_[] = { __VA_ARGS__ }; stream_put(ctx, w_, ARRAY_SIZE(w_)); } while (0)

/* a PPP state block (header word first) and its two stream words */
static void
ppp_block(struct g9p_context *ctx, const uint32_t *w, unsigned n)
{
   uint32_t at = frame_alloc(ctx, 4 * n, 64);
   uint64_t va = frame_va(ctx, at);

   memcpy(frame_ptr(ctx, at), w, 4 * n);
   STREAM(ctx, n << 8 | (uint32_t)(va >> 32), (uint32_t)va & 0xfffffffc);
}

#define PPP(ctx, ...) do { const uint32_t w_[] = { __VA_ARGS__ }; ppp_block(ctx, w_, ARRAY_SIZE(w_)); } while (0)

static uint32_t f32b(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

/* the stream's room: the start of the per-render buffer */
#define STREAM_WORDS (1u << 20)

static void
frame_reset(struct g9p_context *ctx)
{
   struct g9p_frame *f = &ctx->frame;

   f->ndraws = 0;
   f->nstream = 0;
   f->pos = 4 * STREAM_WORDS;
   ctx->dev->code_frame = G9P_CODE_FRAME;
}

/* ---- the render --------------------------------------------------------------- */

void
g9p_flush_frame(struct g9p_context *ctx)
{
   struct g9p_frame *f = &ctx->frame;
   struct g9p_device *dev = ctx->dev;
   struct g9p_resource *rt;
   unsigned w, h, tx, ty;

   if (!f->target || (!f->ndraws && !f->cleared))
      return;
   rt = g9p_resource(f->target);
   w = u_minify(f->target->width0, f->level);
   h = u_minify(f->target->height0, f->level);
   tx = DIV_ROUND_UP(w, 32);
   ty = DIV_ROUND_UP(h, 32);

   /* the background: the clear colour (to do: the target's content when
    * nothing cleared it) */
   const float zero[4] = { 0, 0, 0, 0 };
   uint16_t bgcode[64];
   unsigned bgsize = g9p_clear_program(bgcode, f->cleared ? f->clear : zero);
   uint32_t bg_usc = g9p_code_frame(dev, bgsize, 256);
   memcpy(g9p_code_ptr(dev, bg_usc), bgcode, bgsize);
   uint32_t bg_pds = g9p_code_frame(dev, 0x40, 64);
   uint32_t *p = g9p_code_ptr(dev, bg_pds);
   memset(p, 0, 0x40);
   p[0] = bg_usc;
   p[1] = 0x10;
   p[8] = 0xf4000002;
   p[9] = 0xd3000000;
   /* (the background's colour DMA, as G6's: harmless, its program reads none) */
   uint32_t col = frame_alloc(ctx, 16, 16);
   memcpy(frame_ptr(ctx, col), f->clear, 16);
   uint32_t texu = g9p_code_frame(dev, 16, 16);
   uint64_t q[2] = { frame_va(ctx, col), 0x90004004 };
   memcpy(g9p_code_ptr(dev, texu), q, 16);

   /* the end of tile: the target's image state, DOUTD'd, and the store */
   uint64_t va = rt->bo->va + rt->offset[f->level];
   unsigned pitch = rt->stride[f->level];
   uint32_t img = frame_alloc(ctx, 32, 32);
   uint32_t im[8] = { 0x03050502 | ((w - 1) & 0x1f) << 27, ((w - 1) >> 5 & 0x1ff) | (h - 1) << 9,
                      (uint32_t)(va >> 1), 0x80000000 | (pitch / 64 - 1) << 15 | (uint32_t)(va >> 33),
                      0, 0xc0000000, 0, 0 };
   memcpy(frame_ptr(ctx, img), im, 32);
   uint32_t eot = g9p_code_frame(dev, 32, 16);
   uint64_t e[2] = { frame_va(ctx, img), 0x90000007 };
   memcpy(g9p_code_ptr(dev, eot), e, 16);
   ((uint32_t *)g9p_code_ptr(dev, eot))[4] = G9P_USC_EOT;
   ((uint32_t *)g9p_code_ptr(dev, eot))[5] = 0x10;

   /* the stream's end: clip to the last tile, terminate */
   PPP(ctx, 0x02000000, (tx - 1) << 18 | (ty - 1), 0);
   STREAM(ctx, 0xc0000000);

   if (!bg_usc || !bg_pds || !texu || !eot) {
      frame_reset(ctx);
      return;
   }
   uint64_t geom[5] = { 40, frame_va(ctx, 0), 0, 2 | (0x20000ULL << 32), 0 };
   uint8_t frag[376] = { 0 };
   uint32_t *f32p = (uint32_t *)(frag + 336);
   uint64_t *bg = (uint64_t *)(frag + 256);

   *(uint32_t *)frag = sizeof(frag);
   bg[0] = (uint64_t)G9P_PDS_DOUTD << 32 | bg_pds;
   bg[1] = texu;
   bg[2] = 0x0080000000800000ULL;
   f32p[1] = 0x3f800000;
   f32p[2] = 0x300;
   f32p[3] = 0xf;
   f32p[4] = 0x0023000f;
   f32p[5] = 0x402;
   f32p[7] = eot;

   struct g9p_hwrt *hw = g9p_hwrt_get(dev, w, h);
   if (hw)
      g9p_render(dev, hw, geom, sizeof(geom), frag, sizeof(frag));
   f->cleared = false;
   frame_reset(ctx);
}

/* the frame's target follows the framebuffer's first colour buffer */
static bool
frame_target(struct g9p_context *ctx)
{
   const struct pipe_surface *s = &ctx->fb.cbufs[0];

   if (ctx->fb.nr_cbufs < 1 || !s->texture || !g9p_rt_format(s->format))
      return false;
   if (ctx->frame.target != s->texture || ctx->frame.level != s->level) {
      g9p_flush_frame(ctx);
      pipe_resource_reference(&ctx->frame.target, s->texture);
      ctx->frame.level = s->level;
   }
   return true;
}

/* ---- shaders ------------------------------------------------------------------- */

static void *
g9p_create_shader_state(struct pipe_context *pctx, const struct pipe_shader_state *s)
{
   struct g9p_cso_shader *sh = CALLOC_STRUCT(g9p_cso_shader);

   if (!sh)
      return NULL;
   assert(s->type == PIPE_SHADER_IR_NIR);
   sh->nir = s->ir.nir;
   return sh;
}

static void
g9p_delete_shader_state(struct pipe_context *pctx, void *p)
{
   struct g9p_cso_shader *sh = p;

   while (sh->variants) {
      struct g9p_variant *v = sh->variants;
      sh->variants = v->next;
      FREE(v);
   }
   ralloc_free(sh->nir);
   FREE(sh);
}

static void g9p_bind_vs_state(struct pipe_context *p, void *s) { g9p_context(p)->vs = s; }
static void g9p_bind_fs_state(struct pipe_context *p, void *s) { g9p_context(p)->fs = s; }

static struct g9p_variant *
variant(struct g9p_context *ctx, struct g9p_cso_shader *sh, const void *key, unsigned key_size,
        struct g9p_options *opts, struct g9p_varyings *vary)
{
   struct g9p_variant *v;

   for (v = sh->variants; v; v = v->next)
      if (v->key_size == key_size && !memcmp(v->key, key, key_size))
         return v->failed ? NULL : v;
   if (!(v = CALLOC_STRUCT(g9p_variant)))
      return NULL;
   assert(key_size <= sizeof(v->key));
   memcpy(v->key, key, key_size);
   v->key_size = key_size;
   v->next = sh->variants;
   sh->variants = v;

   nir_shader *nir = nir_shader_clone(NULL, sh->nir);
   struct g9p_shader out;
   if (vary)
      v->vary = *vary;
   opts->varyings = &v->vary;
   const char *err = g9p_compile(nir, getenv("G9P_NIR") != NULL, opts, &out);
   ralloc_free(nir);
   if (err) {
      mesa_loge("g9p: %s shader: %s", sh->nir->info.stage == MESA_SHADER_VERTEX ? "vertex" : "fragment", err);
      if (getenv("G9P_NIR"))
         nir_print_shader(sh->nir, stderr);
      v->failed = true;
      return NULL;
   }
   v->code = g9p_code_shader(ctx->dev, out.code, out.size);
   v->nregs = out.nregs;
   v->noutputs = out.noutputs;
   free(out.code);
   if (!v->code) {
      v->failed = true;
      return NULL;
   }
   return v;
}

static unsigned
format_comps(enum pipe_format f)
{
   switch (f) {
   case PIPE_FORMAT_R32_FLOAT: return 1;
   case PIPE_FORMAT_R32G32_FLOAT: return 2;
   case PIPE_FORMAT_R32G32B32_FLOAT: return 3;
   case PIPE_FORMAT_R32G32B32A32_FLOAT: return 4;
   default: return 0;
   }
}

/* ---- state objects ------------------------------------------------------------- */

#define CSO_COPY(name, type)                                                       \
   static void *g9p_create_##name(struct pipe_context *p, const type *s)            \
   {                                                                                \
      return mem_dup(s, sizeof(*s));                                                \
   }                                                                                \
   static void g9p_delete_##name(struct pipe_context *p, void *s) { FREE(s); }

CSO_COPY(blend_state, struct pipe_blend_state)
CSO_COPY(rasterizer_state, struct pipe_rasterizer_state)
CSO_COPY(depth_stencil_alpha_state, struct pipe_depth_stencil_alpha_state)
CSO_COPY(sampler_state, struct pipe_sampler_state)

static void g9p_bind_blend_state(struct pipe_context *p, void *s) { g9p_context(p)->blend = s; }
static void g9p_bind_rasterizer_state(struct pipe_context *p, void *s) { g9p_context(p)->rast = s; }
static void g9p_bind_depth_stencil_alpha_state(struct pipe_context *p, void *s) { g9p_context(p)->dsa = s; }
static void g9p_bind_sampler_states(struct pipe_context *p, mesa_shader_stage sh, unsigned start, unsigned n,
                                    void **s) {}

static void *
g9p_create_vertex_elements_state(struct pipe_context *p, unsigned count, const struct pipe_vertex_element *e)
{
   struct g9p_velems *v = CALLOC_STRUCT(g9p_velems);

   if (!v)
      return NULL;
   v->count = MIN2(count, G9P_MAX_ATTRIBS);
   memcpy(v->e, e, sizeof(e[0]) * v->count);
   return v;
}

static void g9p_bind_vertex_elements_state(struct pipe_context *p, void *s) { g9p_context(p)->velems = s; }
static void g9p_delete_vertex_elements_state(struct pipe_context *p, void *s) { FREE(s); }

static void
g9p_set_vertex_buffers(struct pipe_context *pctx, unsigned count, const struct pipe_vertex_buffer *buffers)
{
   struct g9p_context *ctx = g9p_context(pctx);

   util_set_vertex_buffers_count(ctx->vb, &ctx->nvb, buffers, count);
}

static void
g9p_set_framebuffer_state(struct pipe_context *pctx, const struct pipe_framebuffer_state *fb)
{
   struct g9p_context *ctx = g9p_context(pctx);

   util_copy_framebuffer_state(&ctx->fb, fb);
   if (ctx->frame.target && (fb->nr_cbufs < 1 || fb->cbufs[0].texture != ctx->frame.target ||
                             fb->cbufs[0].level != ctx->frame.level)) {
      g9p_flush_frame(ctx);
      pipe_resource_reference(&ctx->frame.target, NULL);
   }
}

static void
g9p_set_viewport_states(struct pipe_context *p, unsigned start, unsigned n, const struct pipe_viewport_state *vp)
{
   if (start == 0 && n)
      g9p_context(p)->viewport = vp[0];
}

static void g9p_set_scissor_states(struct pipe_context *p, unsigned s, unsigned n, const struct pipe_scissor_state *x) {}
static void g9p_set_blend_color(struct pipe_context *p, const struct pipe_blend_color *c) {}
static void g9p_set_stencil_ref(struct pipe_context *p, const struct pipe_stencil_ref r) {}
static void g9p_set_sample_mask(struct pipe_context *p, unsigned m) {}
static void g9p_set_clip_state(struct pipe_context *p, const struct pipe_clip_state *c) {}
static void g9p_set_polygon_stipple(struct pipe_context *p, const struct pipe_poly_stipple *s) {}
static void g9p_set_constant_buffer(struct pipe_context *p, mesa_shader_stage s, uint i,
                                    const struct pipe_constant_buffer *cb) {}
static void g9p_set_sampler_views(struct pipe_context *p, mesa_shader_stage s, unsigned start, unsigned n,
                                  unsigned trailing, struct pipe_sampler_view **v) {}
static void g9p_texture_barrier(struct pipe_context *p, unsigned flags) {}
static void g9p_memory_barrier(struct pipe_context *p, unsigned flags) {}

static struct pipe_sampler_view *
g9p_create_sampler_view(struct pipe_context *pctx, struct pipe_resource *tex, const struct pipe_sampler_view *t)
{
   struct pipe_sampler_view *v = CALLOC_STRUCT(pipe_sampler_view);

   if (!v)
      return NULL;
   *v = *t;
   pipe_reference_init(&v->reference, 1);
   v->texture = NULL;
   pipe_resource_reference(&v->texture, tex);
   v->context = pctx;
   return v;
}

static void
g9p_sampler_view_destroy(struct pipe_context *pctx, struct pipe_sampler_view *v)
{
   pipe_resource_reference(&v->texture, NULL);
   FREE(v);
}

/* ---- clears and draws ---------------------------------------------------------- */

static void
g9p_clear(struct pipe_context *pctx, unsigned buffers, uint32_t color_clear_mask, uint8_t stencil_clear_mask,
          const struct pipe_scissor_state *scissor, const union pipe_color_union *color, double depth,
          unsigned stencil)
{
   struct g9p_context *ctx = g9p_context(pctx);

   if (!(buffers & PIPE_CLEAR_COLOR0) || !frame_target(ctx))
      return;
   if (scissor)
      mesa_logw_once("g9p: a scissored clear cleared everything (to do)");
   /* the whole target: what was drawn so far goes */
   frame_reset(ctx);
   ctx->frame.cleared = true;
   memcpy(ctx->frame.clear, color->f, sizeof(ctx->frame.clear));
}

static unsigned
topology(enum mesa_prim mode)
{
   switch (mode) {
   case MESA_PRIM_TRIANGLES: return 6;
   case MESA_PRIM_TRIANGLE_STRIP: return 9;
   case MESA_PRIM_TRIANGLE_FAN: return 11;
   default: return ~0u;
   }
}

/* temps word of a DOUTU: registers in units of 4, << 3 */
static uint32_t temps(unsigned nregs) { return MAX2(DIV_ROUND_UP(nregs, 4), 2) << 3; }

/* VDM_STATE5 for a vertex shader of nregs registers: registers x instances
 * within 256 (G8.2) */
static uint32_t
state5(unsigned nregs, unsigned cs_dwords)
{
   uint32_t cs = DIV_ROUND_UP(4 * cs_dwords, 64) << 16;

   if (nregs <= 8)
      return 0x1081 | cs;
   if (nregs <= 16)
      return 0x2081 | 16 << 25 | cs;
   return 0x4081 | 8 << 25 | cs;
}

static void
g9p_draw_vbo(struct pipe_context *pctx, const struct pipe_draw_info *info, unsigned drawid_offset,
             const struct pipe_draw_indirect_info *indirect, const struct pipe_draw_start_count_bias *draws,
             unsigned num_draws)
{
   struct g9p_context *ctx = g9p_context(pctx);
   struct g9p_device *dev = ctx->dev;
   const unsigned topo = topology(info->mode);

   if (indirect || !ctx->vs || !ctx->fs || !ctx->velems || !frame_target(ctx))
      return;
   if (topo == ~0u) {
      mesa_logw_once("g9p: points and lines are not drawn yet");
      return;
   }
   if (info->instance_count > 1)
      mesa_logw_once("g9p: instances past the first are not drawn yet");

   /* the vertex shader for these vertex elements */
   struct g9p_vertex_attrib attribs[G9P_MAX_ATTRIBS];
   const struct g9p_velems *ve = ctx->velems;
   for (unsigned i = 0; i < ve->count; i++) {
      attribs[i].stride = ve->e[i].src_stride;
      attribs[i].comps = format_comps(ve->e[i].src_format);
      if (!attribs[i].comps) {
         mesa_logw_once("g9p: a vertex format other than 32-bit floats (to do)");
         return;
      }
   }
   struct g9p_options vo = { .attribs = attribs, .num_attribs = ve->count };
   struct g9p_variant *vs = variant(ctx, ctx->vs, attribs, sizeof(attribs[0]) * ve->count, &vo, NULL);
   if (!vs)
      return;

   /* the fragment shader for those varyings and this blend state */
   struct {
      struct g9p_varyings vary;
      struct g9p_blend blend;
      unsigned colormask;
      bool blending;
   } fkey;
   memset(&fkey, 0, sizeof(fkey));
   fkey.vary = vs->vary;
   const struct pipe_rt_blend_state *rb = ctx->blend ? &ctx->blend->rt[0] : NULL;
   fkey.colormask = rb ? rb->colormask : 0xf;
   if (rb && (rb->blend_enable || fkey.colormask != 0xf)) {
      fkey.blending = true;
      fkey.blend = (struct g9p_blend){
         rb->blend_enable ? rb->rgb_func : PIPE_BLEND_ADD,
         rb->blend_enable ? rb->rgb_src_factor : PIPE_BLENDFACTOR_ONE,
         rb->blend_enable ? rb->rgb_dst_factor : PIPE_BLENDFACTOR_ZERO,
         rb->blend_enable ? rb->alpha_func : PIPE_BLEND_ADD,
         rb->blend_enable ? rb->alpha_src_factor : PIPE_BLENDFACTOR_ONE,
         rb->blend_enable ? rb->alpha_dst_factor : PIPE_BLENDFACTOR_ZERO,
         ctx->fb.cbufs[0].format };
   }
   struct g9p_options fo = { .blend = fkey.blending ? &fkey.blend : NULL, .colormask = fkey.colormask };
   struct g9p_variant *fs = variant(ctx, ctx->fs, &fkey, sizeof(fkey), &fo, &vs->vary);
   if (!fs)
      return;

   const struct g9p_varyings *vy = &fs->vary;
   const struct g9p_resource *rt = g9p_resource(ctx->frame.target);
   const unsigned w = u_minify(rt->base.width0, ctx->frame.level), h = u_minify(rt->base.height0, ctx->frame.level);
   const unsigned tx = DIV_ROUND_UP(w, 32), ty = DIV_ROUND_UP(h, 32);

   /* the index buffer */
   uint64_t index_va = 0;
   if (info->index_size) {
      if (info->has_user_indices) {
         unsigned n = 0;
         for (unsigned d = 0; d < num_draws; d++)
            n = MAX2(n, draws[d].start + draws[d].count);
         uint32_t at = frame_alloc(ctx, n * info->index_size, 16);
         memcpy(frame_ptr(ctx, at), info->index.user, n * info->index_size);
         index_va = frame_va(ctx, at);
      } else {
         index_va = g9p_resource(info->index.resource)->bo->va;
      }
   }

   for (unsigned d = 0; d < num_draws; d++) {
      const struct pipe_draw_start_count_bias *dr = &draws[d];
      const int first = info->index_size ? dr->index_bias : (int)dr->start;

      if (!dr->count)
         continue;
      /* the attributes' addresses: common-store dwords 2i, 2i + 1 */
      uint32_t cs = frame_alloc(ctx, 8 * MAX2(ve->count, 1), 16);
      uint64_t *addr = frame_ptr(ctx, cs);
      for (unsigned i = 0; i < ve->count; i++) {
         const struct pipe_vertex_buffer *vb = &ctx->vb[ve->e[i].vertex_buffer_index];
         uint64_t base = vb->buffer.resource ? g9p_resource(vb->buffer.resource)->bo->va : 0;

         addr[i] = base + vb->buffer_offset + ve->e[i].src_offset + (int64_t)first * ve->e[i].src_stride;
      }
      /* PDS: the vertex kick's data, the common store's DOUTD, the pixel
       * kick, the varyings' iterates */
      uint32_t vkick = g9p_code_frame(dev, 16, 16);
      uint32_t csload = g9p_code_frame(dev, 16, 16);
      uint32_t fkick = g9p_code_frame(dev, 0x40, 64);
      if (!vkick || !csload || !fkick)
         return;
      uint32_t *k = g9p_code_ptr(dev, vkick);
      k[0] = vs->code;
      k[1] = temps(vs->nregs);
      k[2] = 0x80000000;
      k[3] = 0;
      uint64_t cq[2] = { frame_va(ctx, cs), 0x90000000 | (2 * MAX2(ve->count, 1)) };
      memcpy(g9p_code_ptr(dev, csload), cq, 16);
      k = g9p_code_ptr(dev, fkick);
      memset(k, 0, 0x40);
      k[0] = fs->code;
      k[1] = temps(fs->nregs);
      k[8] = 0xf4000002;
      k[9] = 0xd3000000;
      uint32_t vpds = 0, vpds_data = 0;
      if (vy->n) {
         /* {03000000, 0} (1 / w), then per varying its iterate; DOUTI each */
         uint32_t data[4 + 2 * G9P_MAX_VARYINGS] = { 0x03000000, 0 };
         unsigned nd = 2;
         for (unsigned i = 0; i < vy->n; i++) {
            const unsigned c = vy->offset[i], n = vy->comps[i], L = 2 + 2 * c;
            data[nd++] = 0x03u << 24 | ((n - 1) << 2 | vy->persp[i]) << 16 | L << 8 | L;
            data[nd++] = (vy->persp[i] ? 0x00400000 : 0) | (c / 2) << 23 | (i == vy->n - 1 ? 0x80000000 : 0);
         }
         while (nd % 4)
            data[nd++] = 0;
         vpds_data = 4 * nd;
         const unsigned ncode = 1 + vy->n + 1;
         vpds = g9p_code_frame(dev, vpds_data + 4 * ncode, 64);
         if (!vpds)
            return;
         k = g9p_code_ptr(dev, vpds);
         memcpy(k, data, vpds_data);
         for (unsigned q = 0; q < 1 + vy->n; q++)
            k[nd + q] = 0xf0000004 | q << 3 | (q == vy->n ? 0x04000000 : 0);
         k[nd + 1 + vy->n] = 0xd3000000;
      }

      /* the VDM stream: vertex state, varyings, the common store's
       * program, the vertex kick, then the PPP state and the indices */
      STREAM(ctx, 0x44002f80, G9P_PDS_VS_KICK, vs->noutputs, state5(vs->nregs, 2 * ve->count));
      PPP(ctx, 0x001c0000, vy->total, 0, 0);
      STREAM(ctx, 0x20020001, csload, G9P_PDS_DOUTD, 0x48002f80, vkick);
      const uint32_t sz1 = 0x00800000 | (vpds_data / 16) << 10 | DIV_ROUND_UP(vy->total, 2) << 4;
      PPP(ctx, 0x000000c0, fkick, 0xc0, sz1, 0, vpds ? vpds : 0xc0);
      PPP(ctx, 0x00020000, vs->noutputs << 24 | 0x00010000);
      /* ISP: no depth yet (ALWAYS, no write), both faces; blending's
       * passes translucent (the hidden-surface removal would run only the
       * top object's shader) */
      const uint32_t pass = fkey.blending ? 1u << 24 : 0;
      PPP(ctx, 0x0000001f, 0x03c40000, 0x003e0f00 | pass, 0x0e000000, 0x003e0f00 | pass, 0);
      const struct pipe_viewport_state *vp = &ctx->viewport;
      PPP(ctx, 0x00000c00, 0x80000000 | (tx - 1), ty - 1, f32b(vp->translate[0]), f32b(vp->scale[0]),
          f32b(vp->translate[1]), f32b(vp->scale[1]), f32b(vp->translate[2]), f32b(vp->scale[2]));
      PPP(ctx, 0x00210000, 0x1e3ce508, 0x000004a0);
      PPP(ctx, 0x00000020, 0);
      if (info->index_size) {
         const uint64_t ia = index_va + (uint64_t)dr->start * info->index_size;
         const uint32_t isz = info->index_size == 1 ? 0 : info->index_size == 2 ? 1 : 2;
         STREAM(ctx, 0x7e400000 | isz << 17 | topo << 8 | (uint32_t)(ia >> 32), (uint32_t)ia, dr->count, 0, 0,
                0xaa);
      } else {
         STREAM(ctx, 0x6e440000 | topo << 8, dr->count, 0, 0, 0xaa);
      }
      ctx->frame.ndraws++;
   }
}

/* ---- flush, blit, the context ---------------------------------------------------- */

static void
g9p_flush(struct pipe_context *pctx, struct pipe_fence_handle **fence, unsigned flags)
{
   g9p_flush_frame(g9p_context(pctx));
   if (fence) {
      struct g9p_fence *f = CALLOC_STRUCT(g9p_fence);

      pipe_reference_init(&f->reference, 1);
      pctx->screen->fence_reference(pctx->screen, fence, NULL);
      *fence = (struct pipe_fence_handle *)f;
   }
}

static void
g9p_flush_resource(struct pipe_context *p, struct pipe_resource *r)
{
}

/* copies by the CPU (after the render that may write the source) */
static void
g9p_blit(struct pipe_context *pctx, const struct pipe_blit_info *info)
{
   if (info->src.box.width != info->dst.box.width || info->src.box.height != info->dst.box.height ||
       info->mask != PIPE_MASK_RGBA || info->scissor_enable) {
      mesa_logw_once("g9p: a scaled / masked blit (to do)");
      return;
   }
   util_resource_copy_region(pctx, info->dst.resource, info->dst.level, info->dst.box.x, info->dst.box.y,
                             info->dst.box.z, info->src.resource, info->src.level, &info->src.box);
}

static void
g9p_context_destroy(struct pipe_context *pctx)
{
   struct g9p_context *ctx = g9p_context(pctx);

   g9p_flush_frame(ctx);
   pipe_resource_reference(&ctx->frame.target, NULL);
   util_unreference_framebuffer_state(&ctx->fb);
   util_set_vertex_buffers_count(ctx->vb, &ctx->nvb, NULL, 0);
   if (pctx->stream_uploader)
      u_upload_destroy(pctx->stream_uploader);
   g9p_bo_destroy(ctx->dev, ctx->frame.bo);
   slab_destroy_child(&ctx->transfer_pool);
   FREE(ctx);
}

struct pipe_context *
g9p_context_create(struct pipe_screen *pscreen, void *priv, unsigned flags)
{
   struct g9p_screen *screen = g9p_screen(pscreen);
   struct g9p_context *ctx = CALLOC_STRUCT(g9p_context);
   struct pipe_context *p;

   if (!ctx)
      return NULL;
   p = &ctx->base;
   ctx->dev = &screen->dev;
   if (!(ctx->frame.bo = g9p_bo_create(ctx->dev, FRAME_BO_SIZE))) {
      FREE(ctx);
      return NULL;
   }
   ctx->frame.stream = (uint32_t *)ctx->frame.bo->map;
   frame_reset(ctx);
   slab_create_child(&ctx->transfer_pool, &screen->transfer_pool);

   p->screen = pscreen;
   p->priv = priv;
   p->destroy = g9p_context_destroy;
   p->flush = g9p_flush;
   p->flush_resource = g9p_flush_resource;
   p->clear = g9p_clear;
   p->draw_vbo = g9p_draw_vbo;
   p->blit = g9p_blit;
   p->resource_copy_region = util_resource_copy_region;
   p->texture_barrier = g9p_texture_barrier;
   p->memory_barrier = g9p_memory_barrier;

   p->create_blend_state = g9p_create_blend_state;
   p->bind_blend_state = g9p_bind_blend_state;
   p->delete_blend_state = g9p_delete_blend_state;
   p->create_rasterizer_state = g9p_create_rasterizer_state;
   p->bind_rasterizer_state = g9p_bind_rasterizer_state;
   p->delete_rasterizer_state = g9p_delete_rasterizer_state;
   p->create_depth_stencil_alpha_state = g9p_create_depth_stencil_alpha_state;
   p->bind_depth_stencil_alpha_state = g9p_bind_depth_stencil_alpha_state;
   p->delete_depth_stencil_alpha_state = g9p_delete_depth_stencil_alpha_state;
   p->create_sampler_state = g9p_create_sampler_state;
   p->bind_sampler_states = g9p_bind_sampler_states;
   p->delete_sampler_state = g9p_delete_sampler_state;
   p->create_vertex_elements_state = g9p_create_vertex_elements_state;
   p->bind_vertex_elements_state = g9p_bind_vertex_elements_state;
   p->delete_vertex_elements_state = g9p_delete_vertex_elements_state;
   p->create_vs_state = g9p_create_shader_state;
   p->bind_vs_state = g9p_bind_vs_state;
   p->delete_vs_state = g9p_delete_shader_state;
   p->create_fs_state = g9p_create_shader_state;
   p->bind_fs_state = g9p_bind_fs_state;
   p->delete_fs_state = g9p_delete_shader_state;

   p->set_vertex_buffers = g9p_set_vertex_buffers;
   p->set_framebuffer_state = g9p_set_framebuffer_state;
   p->set_viewport_states = g9p_set_viewport_states;
   p->set_scissor_states = g9p_set_scissor_states;
   p->set_blend_color = g9p_set_blend_color;
   p->set_stencil_ref = g9p_set_stencil_ref;
   p->set_sample_mask = g9p_set_sample_mask;
   p->set_clip_state = g9p_set_clip_state;
   p->set_polygon_stipple = g9p_set_polygon_stipple;
   p->set_constant_buffer = g9p_set_constant_buffer;
   p->set_sampler_views = g9p_set_sampler_views;
   p->create_sampler_view = g9p_create_sampler_view;
   p->sampler_view_destroy = g9p_sampler_view_destroy;

   g9p_resource_context_init(p);
   p->stream_uploader = u_upload_create_default(p);
   if (!p->stream_uploader) {
      g9p_context_destroy(p);
      return NULL;
   }
   p->const_uploader = p->stream_uploader;
   return p;
}
