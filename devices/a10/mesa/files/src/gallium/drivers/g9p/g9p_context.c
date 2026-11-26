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
#include "compiler/nir/nir_builder.h"
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
#include "util/bitscan.h"

#include "g9p_resource.h"
#include "indices/u_indices.h"
#include "util/u_prim.h"
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

   for (unsigned i = 0; i < f->nreads; i++)
      pipe_resource_reference(&f->reads[i], NULL);
   f->nreads = 0;
   f->reads_overflow = false;
   f->ndraws = 0;
   f->nstream = 0;
   f->nsc = 0;
   f->sc_at = 0;
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
   if (f->nsc)
      *(uint64_t *)(frag + 8) = frame_va(ctx, f->sc_at);     /* isp_scissor_base */
   bg[0] = (uint64_t)G9P_PDS_DOUTD << 32 | bg_pds;
   bg[1] = texu;
   bg[2] = 0x0080000000800000ULL;
   f32p[1] = f32b(f->zclear);                   /* ISP background depth */
   f32p[2] = 0x300 | f->sclear;                 /* background stencil, mask, tag enable */
   f32p[3] = 0xf;
   f32p[4] = 0x0023000f;
   f32p[5] = 0x402;
   f32p[7] = eot;

   if (getenv("G9P_DEBUG") && atoi(getenv("G9P_DEBUG")) >= 4)
      mesa_logi("g9p: render %ux%u: %u draws, %s", w, h, f->ndraws, f->cleared ? "cleared" : "not cleared");
   struct g9p_hwrt *hw = g9p_hwrt_get(dev, w, h);
   if (hw)
      g9p_render(dev, hw, geom, sizeof(geom), frag, sizeof(frag));
   rt->seq++;                                   /* (a texture's twiddled copy is old now) */
   f->cleared = false;
   f->zclear = 1.0f;
   f->sclear = 0;
   frame_reset(ctx);
}

static void
frame_reads(struct g9p_context *ctx, struct pipe_resource *prsc)
{
   struct g9p_frame *f = &ctx->frame;

   if (!prsc)
      return;
   for (unsigned i = 0; i < f->nreads; i++)
      if (f->reads[i] == prsc)
         return;
   if (f->nreads == ARRAY_SIZE(f->reads)) {
      f->reads_overflow = true;
      return;
   }
   pipe_resource_reference(&f->reads[f->nreads++], prsc);
}

bool
g9p_frame_uses(struct g9p_context *ctx, struct pipe_resource *prsc)
{
   struct g9p_frame *f = &ctx->frame;

   if (!f->ndraws && !f->cleared)
      return false;
   if (prsc == f->target || f->reads_overflow)
      return true;
   for (unsigned i = 0; i < f->nreads; i++)
      if (f->reads[i] == prsc)
         return true;
   return false;
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
   if (sh->nir->info.stage == MESA_SHADER_FRAGMENT) {
      nir_foreach_shader_in_variable(var, sh->nir)
         sh->flat_inputs |= var->data.interpolation == INTERP_MODE_FLAT;
      nir_foreach_function_impl(impl, sh->nir)
         nir_foreach_block(block, impl)
            nir_foreach_instr(instr, block)
               sh->flat_inputs |= instr->type == nir_instr_type_intrinsic &&
                                  nir_instr_as_intrinsic(instr)->intrinsic == nir_intrinsic_load_input;
   }
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
   v->psiz = out.psiz;
   if (getenv("G9P_DEBUG")) {
      /* G9P_DEBUG=1: each variant's numbers, its code in /tmp/g9p-<stage>-<code>.bin */
      char path[64];
      FILE *f;

      snprintf(path, sizeof(path), "/tmp/g9p-%s-%x.bin", sh->nir->info.stage == MESA_SHADER_VERTEX ? "vs" : "fs",
               v->code);
      if ((f = fopen(path, "wb"))) {
         fwrite(out.code, 1, out.size, f);
         fclose(f);
      }
      mesa_logi("g9p: %s variant at %#x: %u bytes, %u registers, %u outputs", path, v->code, out.size,
                out.nregs, out.noutputs);
   }
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
static void
g9p_bind_sampler_states(struct pipe_context *p, mesa_shader_stage sh, unsigned start, unsigned n, void **s)
{
   struct g9p_context *ctx = g9p_context(p);

   if (sh != MESA_SHADER_FRAGMENT)
      return;
   for (unsigned i = 0; i < n && start + i < G9P_MAX_TEXTURES; i++)
      ctx->samplers[start + i] = s ? s[i] : NULL;
}

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

static void
g9p_set_scissor_states(struct pipe_context *p, unsigned s, unsigned n, const struct pipe_scissor_state *x)
{
   if (!s && n)
      g9p_context(p)->scissor = x[0];
}
static void g9p_set_blend_color(struct pipe_context *p, const struct pipe_blend_color *c) {}
static void g9p_set_stencil_ref(struct pipe_context *p, const struct pipe_stencil_ref r) { g9p_context(p)->stencil_ref = r; }
static void g9p_set_sample_mask(struct pipe_context *p, unsigned m) {}
static void g9p_set_clip_state(struct pipe_context *p, const struct pipe_clip_state *c) {}
static void g9p_set_polygon_stipple(struct pipe_context *p, const struct pipe_poly_stipple *s) {}
static void
g9p_set_constant_buffer(struct pipe_context *p, mesa_shader_stage s, uint i, const struct pipe_constant_buffer *cb)
{
   struct g9p_context *ctx = g9p_context(p);
   struct pipe_constant_buffer *dst;

   if (i != 0 || (s != MESA_SHADER_VERTEX && s != MESA_SHADER_FRAGMENT))
      return;
   dst = &ctx->cb[s == MESA_SHADER_FRAGMENT];
   pipe_resource_reference(&dst->buffer, cb ? cb->buffer : NULL);
   if (cb) {
      dst->buffer_offset = cb->buffer_offset;
      dst->buffer_size = cb->buffer_size;
      dst->user_buffer = cb->user_buffer;
   } else {
      dst->buffer_size = 0;
      dst->user_buffer = NULL;
   }
}

static void
g9p_set_sampler_views(struct pipe_context *p, mesa_shader_stage s, unsigned start, unsigned n,
                      unsigned trailing, struct pipe_sampler_view **v)
{
   struct g9p_context *ctx = g9p_context(p);

   if (s != MESA_SHADER_FRAGMENT)
      return;
   for (unsigned i = 0; i < n + trailing && start + i < G9P_MAX_TEXTURES; i++)
      pipe_sampler_view_reference(&ctx->views[start + i], v && i < n ? v[i] : NULL);
}

/* the words of constant buffer 0 for a stage, and their count */
static const void *
cb_words(struct g9p_context *ctx, unsigned stage, unsigned *n)
{
   const struct pipe_constant_buffer *cb = &ctx->cb[stage];

   *n = cb->buffer_size / 4;
   if (cb->user_buffer)
      return cb->user_buffer;
   if (cb->buffer)
      return g9p_resource(cb->buffer)->bo->map + cb->buffer_offset;
   *n = 0;
   return NULL;
}
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

   if (!frame_target(ctx))
      return;
   if (scissor)
      mesa_logw_once("g9p: a scissored clear cleared everything (to do)");
   /* depth and stencil: the background's values (after draws: to do) */
   if (buffers & (PIPE_CLEAR_DEPTH | PIPE_CLEAR_STENCIL)) {
      if (ctx->frame.ndraws && !(buffers & PIPE_CLEAR_COLOR0))
         mesa_logw_once("g9p: a depth / stencil clear after draws (to do)");
      if (buffers & PIPE_CLEAR_DEPTH)
         ctx->frame.zclear = depth;
      if (buffers & PIPE_CLEAR_STENCIL)
         ctx->frame.sclear = stencil;
   }
   if (!(buffers & PIPE_CLEAR_COLOR0))
      return;
   /* the whole target: what was drawn so far goes */
   frame_reset(ctx);
   ctx->frame.cleared = true;
   memcpy(ctx->frame.clear, color->f, sizeof(ctx->frame.clear));
}

/* the source channel of a view's swizzle (0-3), one 4, zero 5 */
static unsigned
tex_swizzle(const struct util_format_description *d, unsigned sw)
{
   if (sw == PIPE_SWIZZLE_0)
      return 5;
   if (sw == PIPE_SWIZZLE_1)
      return 4;
   /* the format's channel for that component */
   const unsigned c = d->swizzle[sw];
   return c == PIPE_SWIZZLE_0 ? 5 : c == PIPE_SWIZZLE_1 ? 4 : c;
}

/* a texture state (8 dwords): word 0 = type 2 (2D) | twiddled (bit 3) |
 * format (RGBA8 5, from bit 8) | swizzle R, G, B, A (3 bits each from bit
 * 15: a memory channel, 4 one, 5 zero) | width - 1 (from bit 27, on into
 * word 1) ; word 1 height - 1 from bit 9, base level from bit 25; words
 * 2-3 the address >> 1 (a chain: the last level | 0x10 in its low bits),
 * a linear image's pitch / 64 - 1 from word 3 bit 15. Single-level
 * textures are read where they are, linear; chains from the twiddled copy */
static void
texture_state(struct g9p_context *ctx, struct pipe_sampler_view *sv, uint32_t *w)
{
   struct g9p_resource *res = sv && sv->texture ? g9p_resource(sv->texture) : NULL;
   const struct util_format_description *d = res ? util_format_description(res->base.format) : NULL;

   if (!res || util_format_get_blocksize(res->base.format) != 4 || d->channel[0].size != 8) {
      mesa_logw_once("g9p: a texture other than 4-byte texels, or none bound (to do)");
      return;
   }
   const unsigned tw = res->base.width0, th = res->base.height0;
   const uint32_t swz = tex_swizzle(d, sv->swizzle_r) << 15 | tex_swizzle(d, sv->swizzle_g) << 18 |
                        tex_swizzle(d, sv->swizzle_b) << 21 | tex_swizzle(d, sv->swizzle_a) << 24;

   if (!res->base.last_level) {
      const uint64_t va = res->bo->va + res->offset[0];

      frame_reads(ctx, &res->base);
      w[0] = 0x2 | 5 << 8 | swz | (tw - 1) << 27;
      w[1] = (tw - 1) >> 5 | (th - 1) << 9;
      w[2] = (uint32_t)(va >> 1);
      w[3] = (uint32_t)(va >> 33) | (res->stride[0] / 64 - 1) << 15;
      return;
   }
   unsigned nlev = 1;
   struct g9p_bo *tb = g9p_resource_twiddled(ctx->dev, res, &nlev);
   if (!tb)
      return;
   /* the twiddled copy is RGBA in memory: the view's swizzle alone */
   const struct util_format_description *rgba = util_format_description(PIPE_FORMAT_R8G8B8A8_UNORM);
   const uint32_t tswz = tex_swizzle(rgba, sv->swizzle_r) << 15 | tex_swizzle(rgba, sv->swizzle_g) << 18 |
                         tex_swizzle(rgba, sv->swizzle_b) << 21 | tex_swizzle(rgba, sv->swizzle_a) << 24;
   const unsigned last = MIN2(sv->u.tex.last_level, nlev - 1), base = MIN2(sv->u.tex.first_level, last);
   w[0] = 0x2 | 1 << 3 | 5 << 8 | tswz | (tw - 1) << 27;
   w[1] = (tw - 1) >> 5 | (th - 1) << 9 | base << 25;
   w[2] = (uint32_t)(tb->va >> 1) | (nlev > 1 ? 0x10 | last : 0);
   w[3] = (uint32_t)(tb->va >> 33);
}

static unsigned
wrap_mode(unsigned w)
{
   switch (w) {
   case PIPE_TEX_WRAP_REPEAT: return 1;
   case PIPE_TEX_WRAP_MIRROR_REPEAT: return 2;
   case PIPE_TEX_WRAP_CLAMP_TO_BORDER: return 3;
   default: return 0;                                      /* clamp to edge */
   }
}

/* the sampler's two words: LOD clamps (1/64, up to 14), filters, address
 * modes (0 clamp, 1 repeat, 2 mirror, 3 border); the high word's other
 * bits as Apple's (0x00c00780) */
static void
sampler_words(const struct pipe_sampler_state *ss, uint32_t *w)
{
   if (!ss) {
      w[0] = 0x000e0000;
      w[1] = 0x00c00780;
      return;
   }
   const unsigned minlod = CLAMP((int)(ss->min_lod * 64), 0, 896);
   const unsigned maxlod = CLAMP((int)(ss->max_lod * 64), (int)minlod, 896);
   w[0] = minlod | maxlod << 10 | (ss->mag_img_filter == PIPE_TEX_FILTER_LINEAR) << 23 |
          (ss->min_img_filter == PIPE_TEX_FILTER_LINEAR) << 25 |
          (ss->min_mip_filter == PIPE_TEX_MIPFILTER_NEAREST) << 27 |
          (ss->min_mip_filter == PIPE_TEX_MIPFILTER_LINEAR) << 28 | wrap_mode(ss->wrap_s) << 29;
   w[1] = 0x00c00780 | wrap_mode(ss->wrap_t) | wrap_mode(ss->wrap_r) << 3;
   if (ss->lod_bias != 0.0f)
      mesa_logw_once("g9p: a LOD bias (to do)");
}

static uint32_t
stencil_op(unsigned op)
{
   /* pipe: KEEP ZERO REPLACE INCR(sat) DECR(sat) INCR_WRAP DECR_WRAP INVERT;
    * Rogue's ISPB: ... INCR_SAT DECR_SAT INVERT INCR DECR */
   static const uint8_t map[8] = { 0, 1, 2, 3, 4, 6, 7, 5 };
   return map[op & 7];
}

static uint32_t
ispb_word(const struct pipe_stencil_state *s)
{
   if (!s->enabled)
      return 0x0e000000;                                   /* ALWAYS, KEEP */
   return s->func << 25 | stencil_op(s->fail_op) << 22 | stencil_op(s->zfail_op) << 19 |
          stencil_op(s->zpass_op) << 16 | s->valuemask << 8 | s->writemask;
}

/* the ISP words of both faces: depth compare / write, stencil; returns
 * whether the faces differ (ISPCTL two_sided) */
static bool
isp_state(struct g9p_context *ctx, unsigned passtype, uint32_t obj, uint32_t ispa[2], uint32_t ispb[2])
{
   const struct pipe_depth_stencil_alpha_state *dsa = ctx->dsa;
   const struct pipe_surface *zs = &ctx->fb.zsbuf;
   const bool has_z = zs->texture && util_format_has_depth(util_format_description(zs->format));
   const bool has_s = zs->texture && util_format_has_stencil(util_format_description(zs->format));
   uint32_t a = 0x00100000 | passtype << 24 | obj;                   /* dfbztestenable, the object */

   if (has_z && dsa && dsa->depth_enabled)
      a |= dsa->depth_func << 17 | (dsa->depth_writemask ? 0 : 1u << 21);
   else
      a |= 7u << 17 | 1u << 21;                                       /* ALWAYS, no write */
   for (unsigned f = 0; f < 2; f++) {
      const struct pipe_stencil_state *s = dsa ? &dsa->stencil[f && dsa->stencil[1].enabled] : NULL;

      ispa[f] = a | (s && has_s ? ctx->stencil_ref.ref_value[f && dsa->stencil[1].enabled] : 0);
      ispb[f] = s && has_s ? ispb_word(s) : 0x0e000000;
   }
   return ispa[0] != ispa[1] || ispb[0] != ispb[1];
}

static unsigned
topology(enum mesa_prim mode)
{
   switch (mode) {
   case MESA_PRIM_POINTS: return 0;
   case MESA_PRIM_LINES: return 1;
   case MESA_PRIM_LINE_STRIP: return 3;
   case MESA_PRIM_LINE_LOOP: return 5;
   case MESA_PRIM_TRIANGLES: return 6;
   case MESA_PRIM_TRIANGLE_STRIP: return 9;
   case MESA_PRIM_TRIANGLE_FAN: return 11;
   default: return ~0u;
   }
}

/* temps word of a DOUTU: registers in units of 4, << 3 */
static uint32_t temps(unsigned nregs) { return MAX2(DIV_ROUND_UP(nregs, 4), 2) << 3; }

/* a vertex shader's register budget: 8, 16 or 32 (its DOUTU temps and
 * VDM_STATE5's unified size must agree: a 20-register shader with temps
 * for 20 and a unified size of 32 crashed the firmware) */
static unsigned vs_regs(unsigned nregs) { return nregs <= 8 ? 8 : nregs <= 16 ? 16 : 32; }

/* VDM_STATE5 for a vertex shader of nregs registers: registers x instances
 * within 256 (G8.2) */
static uint32_t
state5(unsigned nregs, unsigned cs_dwords)
{
   /* (the common store's size is PDS_STATE0's: a size here crashed the
    * firmware with 32 registers) */
   uint32_t cs = 0;

   if (nregs <= 8)
      return 0x1081 | cs;
   if (nregs <= 16)
      return 0x2081 | 16 << 25 | cs;
   return 0x4081 | 8 << 25 | cs;
}

static void frame_load(struct g9p_context *ctx);

static void g9p_draw_vbo(struct pipe_context *pctx, const struct pipe_draw_info *info, unsigned drawid_offset,
                         const struct pipe_draw_indirect_info *indirect,
                         const struct pipe_draw_start_count_bias *draws, unsigned num_draws);

/* a draw made a triangle list, if it needs to be: quads and polygons, and
 * with flat varyings every draw but a list whose provoking vertex is the
 * first. The varying program's flat shade model 0 takes the triangle's
 * first vertex; a strip's odd triangles reach the hardware with their
 * last two vertices swapped (vertex i + 1 its third), so GL's last
 * provoking vertex has no shade model of its own -- the list's triangles
 * start with it instead (u_indices) */
static bool
draw_converted(struct g9p_context *ctx, const struct pipe_draw_info *info, unsigned drawid_offset,
               const struct pipe_draw_start_count_bias *draws, unsigned num_draws)
{
   const unsigned in_pv = ctx->rast && ctx->rast->flatshade_first ? PV_FIRST : PV_LAST;
   const bool flat = ctx->fs->flat_inputs;
   const bool quads = info->mode == MESA_PRIM_QUADS || info->mode == MESA_PRIM_QUAD_STRIP ||
                      info->mode == MESA_PRIM_POLYGON;
   const bool list = info->mode == MESA_PRIM_TRIANGLES || info->mode == MESA_PRIM_LINES;
   const unsigned hw = BITFIELD_BIT(MESA_PRIM_TRIANGLES) | BITFIELD_BIT(MESA_PRIM_LINES) |
                       BITFIELD_BIT(MESA_PRIM_POINTS);

   if (ctx->converted || (!quads && (topology(info->mode) == ~0u || !flat || info->mode == MESA_PRIM_POINTS ||
                                     (list && in_pv == PV_FIRST))))
      return false;
   const unsigned out_pv = flat ? PV_FIRST : in_pv;
   for (unsigned d = 0; d < num_draws; d++) {
      const struct pipe_draw_start_count_bias *dr = &draws[d];
      struct pipe_draw_info ni = *info;
      struct pipe_draw_start_count_bias nd = { 0 };
      enum mesa_prim prim;
      unsigned isz, nr;
      void *buf;

      if (info->index_size) {
         u_translate_func tr;
         if (u_index_translator(hw, info->mode, info->index_size, dr->count, in_pv, out_pv,
                                info->primitive_restart ? PR_ENABLE : PR_DISABLE, &prim, &isz, &nr,
                                &tr) == U_TRANSLATE_ERROR || !nr || !(buf = malloc(nr * isz)))
            continue;
         const void *src = info->has_user_indices ? info->index.user : g9p_resource(info->index.resource)->bo->map;
         tr(src, dr->start, dr->count, nr, info->restart_index, buf);
         nd.index_bias = dr->index_bias;
      } else {
         u_generate_func gen;
         if (u_index_generator(hw, info->mode, dr->start, dr->count, in_pv, out_pv, &prim,
                               &isz, &nr, &gen) == U_TRANSLATE_ERROR || !nr || !(buf = malloc(nr * isz)))
            continue;
         gen(dr->start, nr, buf);
      }
      ni.mode = prim;
      ni.index_size = isz;
      ni.has_user_indices = true;
      ni.index.user = buf;
      ni.primitive_restart = false;
      nd.count = nr;
      ctx->converted = true;
      g9p_draw_vbo(&ctx->base, &ni, drawid_offset + d, NULL, &nd, 1);
      ctx->converted = false;
      free(buf);
   }
   return true;
}

#define G9P_MAX_SCISSORS 512

/* the draw's scissor rectangle in the frame's table (ISPCTL scenable, DBSC
 * scindex): its index, -1 with the scissor test off, -2 for an empty
 * rectangle (nothing to draw). A full table sends the render first. */
static int
scissor_index(struct g9p_context *ctx)
{
   struct g9p_frame *f = &ctx->frame;
   const struct pipe_scissor_state *sc = &ctx->scissor;

   if (!ctx->rast || !ctx->rast->scissor)
      return -1;
   const struct g9p_resource *rt = g9p_resource(f->target);
   const unsigned w = u_minify(rt->base.width0, f->level), h = u_minify(rt->base.height0, f->level);
   const unsigned x0 = MIN2(sc->minx, w), x1 = MIN2(sc->maxx, w);
   const unsigned y0 = MIN2(sc->miny, h), y1 = MIN2(sc->maxy, h);
   const uint32_t e[2] = { x0 << 16 | x1, y0 << 16 | y1 };

   if (x0 >= x1 || y0 >= y1)
      return -2;
   if (f->nsc && !memcmp(frame_ptr(ctx, f->sc_at + 8 * (f->nsc - 1)), e, sizeof(e)))
      return f->nsc - 1;
   if (f->nsc == G9P_MAX_SCISSORS)
      g9p_flush_frame(ctx);
   if (!f->sc_at && !(f->sc_at = frame_alloc(ctx, 8 * G9P_MAX_SCISSORS, 16)))
      return -2;
   memcpy(frame_ptr(ctx, f->sc_at + 8 * f->nsc), e, sizeof(e));
   return f->nsc++;
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
   if (draw_converted(ctx, info, drawid_offset, draws, num_draws))
      return;
   const int scindex = scissor_index(ctx);
   if (scindex == -2)
      return;
   /* a frame starting without a clear on a target with content: that
    * content first */
   if (!ctx->frame.ndraws && !ctx->frame.cleared && !ctx->frame.loading &&
       g9p_resource(ctx->frame.target)->seq)
      frame_load(ctx);
   if (topo == ~0u) {
      mesa_logw_once("g9p: a primitive type not drawn yet (adjacency, patches)");
      return;
   }
   /* the ISP object: triangles; lines (type 1) and point sprites (type 4,
    * UV 0..1) with their width / size in 4.4 fixed point less one (a
    * triangle's field 0xf: 1.0) */
   const enum mesa_prim reduced = u_reduced_prim(info->mode);
   uint32_t obj = 0xf00;
   if (reduced == MESA_PRIM_LINES) {
      const float lw = ctx->rast ? ctx->rast->line_width : 1;
      obj = 1u << 28 | CLAMP((int)lroundf(lw * 16) - 1, 0, 255) << 8;
   } else if (reduced == MESA_PRIM_POINTS) {
      const float ps = ctx->rast ? ctx->rast->point_size : 1;
      obj = 4u << 28 | CLAMP((int)lroundf(ps * 16) - 1, 0, 255) << 8;
   }
   /* culling (PPP_CTRL cullmode: 1 clockwise, 2 counter-clockwise, as the
    * hardware sees the triangle) */
   uint32_t cull = 0;
   if (ctx->rast && ctx->rast->cull_face && reduced == MESA_PRIM_TRIANGLES) {
      if (ctx->rast->cull_face == PIPE_FACE_FRONT_AND_BACK)
         return;
      /* the rasterizer's counter-clockwise is the hardware's (g9p-glcheck
       * cull); the state tracker already turns front_ccw round for
       * window-system buffers, whose viewport flips y (es2gears showed its
       * back faces while the driver turned it again) */
      const bool ccw_culled = (ctx->rast->cull_face == PIPE_FACE_FRONT) == ctx->rast->front_ccw;
      cull = ccw_culled ? 2 : 1;
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
   /* the vertex shader's common store: the attributes' addresses, then
    * the uniforms */
   unsigned vu_n, fu_n;
   const void *vu = cb_words(ctx, 0, &vu_n);
   const void *fu = cb_words(ctx, 1, &fu_n);
   const unsigned vubo = align(2 * ve->count, 4);
   const unsigned vcs = vubo + vu_n;
   const bool halfz = !(ctx->rast && ctx->rast->clip_halfz);
   struct {
      struct g9p_vertex_attrib a[G9P_MAX_ATTRIBS];
      unsigned ubo_dword, ubo_dwords;
      bool halfz;
   } vkey;
   memset(&vkey, 0, sizeof(vkey));
   memcpy(vkey.a, attribs, sizeof(attribs[0]) * ve->count);
   vkey.ubo_dword = vubo;
   vkey.ubo_dwords = vu_n;
   vkey.halfz = halfz;
   struct g9p_options vo = { .attribs = attribs, .num_attribs = ve->count, .ubo_dword = vubo, .ubo_dwords = vu_n,
                             .clip_halfz = halfz };
   struct g9p_variant *vs = variant(ctx, ctx->vs, &vkey, sizeof(vkey), &vo, NULL);
   if (!vs)
      return;

   /* the fragment shader for those varyings and this blend state */
   /* the fragment shader's common store: texture states (8 dwords each),
    * samplers (2 each), the uniforms */
   const unsigned ntex = util_last_bit(ctx->fs->nir->info.textures_used[0]);
   const unsigned nsamp = util_last_bit(ctx->fs->nir->info.samplers_used[0]);
   const unsigned fsamp = 8 * ntex, fubo = align(fsamp + 2 * nsamp, 4), fcs = fubo + fu_n;
   struct {
      struct g9p_varyings vary;
      struct g9p_blend blend;
      unsigned colormask;
      bool blending;
      unsigned ubo_dword, ubo_dwords, sampler_dword;
      bool pntc_flip;
   } fkey;
   memset(&fkey, 0, sizeof(fkey));
   fkey.vary = vs->vary;
   fkey.ubo_dword = fubo;
   fkey.ubo_dwords = fu_n;
   fkey.sampler_dword = fsamp;
   fkey.pntc_flip = ctx->rast && ctx->rast->sprite_coord_mode == PIPE_SPRITE_COORD_LOWER_LEFT &&
                    (ctx->fs->nir->info.inputs_read & VARYING_BIT_PNTC);
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
   struct g9p_options fo = { .blend = fkey.blending ? &fkey.blend : NULL, .colormask = fkey.colormask,
                             .ubo_dword = fubo, .ubo_dwords = fu_n, .sampler_dword = fsamp,
                             .pntc_flip = fkey.pntc_flip };
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
         frame_reads(ctx, info->index.resource);
      }
   }
   for (unsigned i = 0; i < ve->count; i++)
      frame_reads(ctx, ctx->vb[ve->e[i].vertex_buffer_index].buffer.resource);

   for (unsigned d = 0; d < num_draws; d++) {
      const struct pipe_draw_start_count_bias *dr = &draws[d];
      const int first = info->index_size ? dr->index_bias : (int)dr->start;

      if (!dr->count)
         continue;
      /* the attributes' addresses: common-store dwords 2i, 2i + 1; the
       * uniforms from vubo */
      uint32_t cs = frame_alloc(ctx, 4 * MAX2(vcs, 2), 16);
      uint64_t *addr = frame_ptr(ctx, cs);
      if (vu_n)
         memcpy((uint32_t *)addr + vubo, vu, 4 * vu_n);
      if (getenv("G9P_DEBUG") && atoi(getenv("G9P_DEBUG")) >= 3 && ctx->frame.ndraws < 3) {
         const float *uf = vu;
         for (unsigned k = 0; k < vu_n; k += 4)
            mesa_logi("g9p: draw %u vs uniform %u: %g %g %g %g", ctx->frame.ndraws, k / 4, uf[k], uf[k + 1],
                      uf[k + 2], uf[k + 3]);
      }
      for (unsigned i = 0; i < ve->count; i++) {
         const struct pipe_vertex_buffer *vb = &ctx->vb[ve->e[i].vertex_buffer_index];
         uint64_t base = vb->buffer.resource ? g9p_resource(vb->buffer.resource)->bo->va : 0;

         addr[i] = base + vb->buffer_offset + ve->e[i].src_offset + (int64_t)first * ve->e[i].src_stride;
         if (getenv("G9P_DEBUG") && atoi(getenv("G9P_DEBUG")) >= 2)
            mesa_logi("g9p: draw %u attr %u: vb %u (offset %u) +%u stride %u %s first %d count %u -> %#llx",
                      ctx->frame.ndraws, i, ve->e[i].vertex_buffer_index, vb->buffer_offset, ve->e[i].src_offset,
                      ve->e[i].src_stride, util_format_short_name(ve->e[i].src_format), first, dr->count,
                      (unsigned long long)addr[i]);
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
      k[1] = temps(vs_regs(vs->nregs));
      k[2] = 0x80000000;
      k[3] = 0;
      uint64_t cq[2] = { frame_va(ctx, cs), 0x90000000 | 0x10000000 | MAX2(vcs, 2) };
      memcpy(g9p_code_ptr(dev, csload), cq, 16);
      /* the fragment's common store, DOUTD'd by the "texunicode" program */
      uint32_t fq = 0;
      if (fcs) {
         uint32_t fimg = frame_alloc(ctx, 4 * fcs, 16);
         uint32_t *fw = frame_ptr(ctx, fimg);
         memset(fw, 0, 4 * fcs);
         for (unsigned t = 0; t < ntex; t++)
            texture_state(ctx, ctx->views[t], fw + 8 * t);
         for (unsigned s = 0; s < nsamp; s++)
            sampler_words(ctx->samplers[s], fw + fsamp + 2 * s);
         if (fu_n)
            memcpy(fw + fubo, fu, 4 * fu_n);
         fq = g9p_code_frame(dev, 16, 16);
         if (!fq)
            return;
         uint64_t q2[2] = { frame_va(ctx, fimg), 0x90000000 | 0x10000000 | fcs };
         memcpy(g9p_code_ptr(dev, fq), q2, 16);
      }
      k = g9p_code_ptr(dev, fkick);
      memset(k, 0, 0x40);
      k[0] = fs->code;
      k[1] = temps(fs->nregs);
      k[8] = 0xf4000002;
      k[9] = 0xd3000000;
      uint32_t vpds = 0, vpds_data = 0;
      if (vy->n) {
         /* {03000000, 0} (1 / w), then per varying its iterate; DOUTI each.
          * Bits 24-25 the shade model: 3 Gouraud, 0 flat from the
          * triangle's first vertex (draw_converted puts the provoking vertex
          * there). A flat varying keeps the perspective bits (16 here, 22
          * in the second word): without them its components came one
          * place on (R from G, G from B, B from A). */
         uint32_t data[4 + 2 * G9P_MAX_VARYINGS] = { 0x03000000, 0 };
         unsigned nd = 2;
         for (unsigned i = 0; i < vy->n; i++) {
            const unsigned c = vy->offset[i], n = vy->comps[i], L = 2 + 2 * c;
            /* the perspective bits for flat varyings (without them their
             * components came one place on) and the sprite coordinate
             * (bit 23: wrong values without them); the sprite's offset in
             * the vertex 0 (it has none there) */
            const bool pp = vy->persp[i] || vy->flat[i] || vy->sprite[i];
            const unsigned Ls = vy->sprite[i] ? 0 : L;
            data[nd++] = (vy->flat[i] ? 0u : 3u) << 24 | (vy->sprite[i] ? 1u << 23 : 0) |
                         ((n - 1) << 2 | pp) << 16 | Ls << 8 | Ls;
            data[nd++] = (pp ? 0x00400000 : 0) | (c / 2) << 23 | (i == vy->n - 1 ? 0x80000000 : 0);
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
      STREAM(ctx, 0x44002f80, G9P_PDS_VS_KICK, vs->noutputs, state5(vs->nregs, vcs));
      /* VARYING0: the components the vertex carries (gl_PointCoord's,
       * made by the varying program, not among them) */
      PPP(ctx, 0x001c0000, vs->vary.total, 0, 0);
      /* PDS_STATE0's common-store size: 20 dwords read crashed the
       * firmware with 3, ran with 4 (its unit not Rogue's 64 bytes: to
       * check); a unit of 16 bytes and one more */
      STREAM(ctx, 0x20000001 | MAX2(DIV_ROUND_UP(vcs, 4) + 1, 2) << 16, csload, G9P_PDS_DOUTD, 0x48002f80, vkick);
      const uint32_t sz1 = 0x00800000 | (vpds_data / 16) << 10 | DIV_ROUND_UP(vy->total, 2) << 4;
      if (fq)
         /* (SIZEINFO2 bit 14, pds_tri_merge_disable, with textures: a helper
          * pixel from the next triangle has its gradients) */
         PPP(ctx, 0x000002c0, fkick, G9P_PDS_DOUTD, sz1, (DIV_ROUND_UP(fcs, 4) + 1) << 23 | (ntex ? 1u << 14 : 0),
             vpds ? vpds : 0xc0, fq);
      else
         PPP(ctx, 0x000000c0, fkick, 0xc0, sz1, 0, vpds ? vpds : 0xc0);
      /* OUTPUT_SEL: the vertex size, rhw; bit 18 the point size (the
       * vertex's last dword) for point sprites */
      PPP(ctx, 0x00020000,
          vs->noutputs << 24 | 0x00010000 | (reduced == MESA_PRIM_POINTS && vs->psiz ? 1u << 18 : 0));
      /* ISP (Rogue's ISPCTL / ISPA / ISPB): the depth test and stencil of
       * each face; blending's passes translucent (the hidden-surface
       * removal would run only the top object's shader) */
      uint32_t ispa[2], ispb[2];
      /* the pass type: 1 translucent (blending), 4 punch-through for a
       * shader that discards (opaque, the hidden-surface removal took
       * the triangle as drawn and left uninitialised tile memory where
       * the shader dropped pixels; 2 drew the same as 4) */
      const unsigned passtype = fkey.blending ? 1 : ctx->fs->nir->info.fs.uses_discard ? 4 : 0;
      const bool two_sided = isp_state(ctx, passtype, obj, ispa, ispb);
      PPP(ctx, 0x0000001f, 0x03c40000 | (two_sided ? 1u << 19 : 0) | (scindex >= 0 ? 1u << 16 : 0), ispa[0],
          ispb[0], ispa[1], ispb[1]);
      const struct pipe_viewport_state *vp = &ctx->viewport;
      /* the hardware clips depth to 0..w: GL's -w..w was halved in the
       * vertex shader (clip_halfz), its viewport z follows */
      const float zt = halfz ? vp->translate[2] - vp->scale[2] : vp->translate[2];
      const float zs = halfz ? 2 * vp->scale[2] : vp->scale[2];
      PPP(ctx, 0x00000c00, 0x80000000 | (tx - 1), ty - 1, f32b(vp->translate[0]), f32b(vp->scale[0]),
          f32b(vp->translate[1]), f32b(vp->scale[1]), f32b(zt), f32b(zs));
      PPP(ctx, 0x00210000, 0x1e3ce508, 0x000004a0 | cull);
      PPP(ctx, 0x00000020, scindex >= 0 ? scindex : 0);        /* ISP DBSC: the scissor's index */
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

/* ---- the load ------------------------------------------------------------------ */

static void
load_shaders(struct g9p_context *ctx)
{
   const struct nir_shader_compiler_options *o = ctx->base.screen->nir_options[MESA_SHADER_VERTEX];
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_VERTEX, o, "g9p load vs");
   nir_def *zero = nir_imm_int(&b, 0);

   /* position (x, y) from attribute 0; uv = position / 2 + 1 / 2 */
   nir_def *pos = nir_load_input(&b, 2, 32, zero, .base = 0, .dest_type = nir_type_float32,
                                 .io_semantics.location = VERT_ATTRIB_GENERIC0, .io_semantics.num_slots = 1);
   nir_store_output(&b, nir_vec4(&b, nir_channel(&b, pos, 0), nir_channel(&b, pos, 1), nir_imm_float(&b, 0),
                                 nir_imm_float(&b, 1)),
                    zero, .base = 0, .write_mask = 0xf, .src_type = nir_type_float32,
                    .io_semantics.location = VARYING_SLOT_POS, .io_semantics.num_slots = 1);
   nir_store_output(&b, nir_fadd_imm(&b, nir_fmul_imm(&b, pos, 0.5), 0.5), zero, .base = 1, .write_mask = 0x3,
                    .src_type = nir_type_float32, .io_semantics.location = VARYING_SLOT_VAR0,
                    .io_semantics.num_slots = 1);
   b.shader->info.io_lowered = true;
   ctx->load_vs = CALLOC_STRUCT(g9p_cso_shader);
   ctx->load_vs->nir = b.shader;

   o = ctx->base.screen->nir_options[MESA_SHADER_FRAGMENT];
   b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, o, "g9p load fs");
   zero = nir_imm_int(&b, 0);
   nir_def *bary = nir_load_barycentric_pixel(&b, 32, .interp_mode = INTERP_MODE_SMOOTH);
   nir_def *uv = nir_load_interpolated_input(&b, 2, 32, bary, zero, .base = 0, .dest_type = nir_type_float32,
                                             .io_semantics.location = VARYING_SLOT_VAR0,
                                             .io_semantics.num_slots = 1);
   nir_tex_instr *tex = nir_tex_instr_create(b.shader, 1);
   tex->op = nir_texop_tex;
   tex->sampler_dim = GLSL_SAMPLER_DIM_2D;
   tex->dest_type = nir_type_float32;
   tex->coord_components = 2;
   tex->src[0] = nir_tex_src_for_ssa(nir_tex_src_coord, uv);
   nir_def_init(&tex->instr, &tex->def, 4, 32);
   nir_builder_instr_insert(&b, &tex->instr);
   nir_store_output(&b, &tex->def, zero, .base = 0, .write_mask = 0xf, .src_type = nir_type_float32,
                    .io_semantics.location = FRAG_RESULT_DATA0, .io_semantics.num_slots = 1);
   b.shader->info.io_lowered = true;
   b.shader->info.num_textures = 1;
   BITSET_SET(b.shader->info.textures_used, 0);
   BITSET_SET(b.shader->info.samplers_used, 0);
   ctx->load_fs = CALLOC_STRUCT(g9p_cso_shader);
   ctx->load_fs->nir = b.shader;

   /* one triangle over the whole target */
   static const float tri[6] = { -1, -1, 3, -1, -1, 3 };
   struct pipe_resource t = { .target = PIPE_BUFFER, .format = PIPE_FORMAT_R8_UNORM, .width0 = sizeof(tri),
                              .height0 = 1, .depth0 = 1, .array_size = 1, .bind = PIPE_BIND_VERTEX_BUFFER };
   ctx->load_vb = ctx->base.screen->resource_create(ctx->base.screen, &t);
   if (ctx->load_vb)
      memcpy(g9p_resource(ctx->load_vb)->bo->map, tri, sizeof(tri));
   ctx->load_velems.count = 1;
   ctx->load_velems.e[0] = (struct pipe_vertex_element){ .src_format = PIPE_FORMAT_R32G32_FLOAT, .src_stride = 8 };
   ctx->load_sampler = (struct pipe_sampler_state){ .min_img_filter = PIPE_TEX_FILTER_NEAREST,
                                                    .mag_img_filter = PIPE_TEX_FILTER_NEAREST,
                                                    .min_mip_filter = PIPE_TEX_MIPFILTER_NONE,
                                                    .wrap_s = PIPE_TEX_WRAP_CLAMP_TO_EDGE,
                                                    .wrap_t = PIPE_TEX_WRAP_CLAMP_TO_EDGE };
}

/* the target's content as the frame's first draw (depth and stencil are
 * not loaded yet) */
static void
frame_load(struct g9p_context *ctx)
{
   struct pipe_resource *rt = ctx->frame.target;

   if (ctx->frame.level || rt->last_level) {
      mesa_logw_once("g9p: a frame without a clear on a target of several levels: not loaded (to do)");
      return;
   }
   if (!ctx->load_vs)
      load_shaders(ctx);
   if (!ctx->load_vb)
      return;

   /* the context's state aside, the load's in */
   struct g9p_cso_shader *vs = ctx->vs, *fs = ctx->fs;
   struct g9p_velems *ve = ctx->velems;
   struct pipe_vertex_buffer vb0 = ctx->vb[0];
   unsigned nvb = ctx->nvb;
   struct pipe_blend_state *blend = ctx->blend;
   struct pipe_rasterizer_state *rast = ctx->rast;
   struct pipe_depth_stencil_alpha_state *dsa = ctx->dsa;
   struct pipe_viewport_state vp = ctx->viewport;
   struct pipe_sampler_view *view0 = ctx->views[0];
   struct pipe_sampler_state *samp0 = ctx->samplers[0];
   struct pipe_sampler_view tmpl = { .format = rt->format, .target = PIPE_TEXTURE_2D,
                                     .swizzle_r = PIPE_SWIZZLE_X, .swizzle_g = PIPE_SWIZZLE_Y,
                                     .swizzle_b = PIPE_SWIZZLE_Z, .swizzle_a = PIPE_SWIZZLE_W };
   struct pipe_sampler_view *view = ctx->base.create_sampler_view(&ctx->base, rt, &tmpl);

   ctx->vs = ctx->load_vs;
   ctx->fs = ctx->load_fs;
   ctx->velems = &ctx->load_velems;
   ctx->vb[0] = (struct pipe_vertex_buffer){ .buffer.resource = ctx->load_vb };
   ctx->nvb = 1;
   ctx->blend = NULL;
   ctx->rast = NULL;
   ctx->dsa = NULL;
   ctx->viewport = (struct pipe_viewport_state){ .scale = { rt->width0 / 2.0f, rt->height0 / 2.0f, 0.5f },
                                                 .translate = { rt->width0 / 2.0f, rt->height0 / 2.0f, 0.5f } };
   ctx->views[0] = view;
   ctx->samplers[0] = &ctx->load_sampler;

   const struct pipe_draw_info info = { .mode = MESA_PRIM_TRIANGLES, .instance_count = 1 };
   const struct pipe_draw_start_count_bias draw = { .start = 0, .count = 3 };
   ctx->frame.loading = true;
   g9p_draw_vbo(&ctx->base, &info, 0, NULL, &draw, 1);
   ctx->frame.loading = false;

   ctx->vs = vs;
   ctx->fs = fs;
   ctx->velems = ve;
   ctx->vb[0] = vb0;
   ctx->nvb = nvb;
   ctx->blend = blend;
   ctx->rast = rast;
   ctx->dsa = dsa;
   ctx->viewport = vp;
   ctx->views[0] = view0;
   ctx->samplers[0] = samp0;
   pipe_sampler_view_reference(&view, NULL);
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
   pipe_resource_reference(&ctx->load_vb, NULL);
   if (ctx->load_vs)
      g9p_delete_shader_state(pctx, ctx->load_vs);
   if (ctx->load_fs)
      g9p_delete_shader_state(pctx, ctx->load_fs);
   util_unreference_framebuffer_state(&ctx->fb);
   util_set_vertex_buffers_count(ctx->vb, &ctx->nvb, NULL, 0);
   for (unsigned i = 0; i < 2; i++)
      pipe_resource_reference(&ctx->cb[i].buffer, NULL);
   for (unsigned i = 0; i < G9P_MAX_TEXTURES; i++)
      pipe_sampler_view_reference(&ctx->views[i], NULL);
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
   u_index_init();
   p = &ctx->base;
   ctx->dev = &screen->dev;
   if (!(ctx->frame.bo = g9p_bo_create(ctx->dev, FRAME_BO_SIZE))) {
      FREE(ctx);
      return NULL;
   }
   ctx->frame.stream = (uint32_t *)ctx->frame.bo->map;
   frame_reset(ctx);
   ctx->frame.zclear = 1.0f;
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
   p->resource_release = u_default_resource_release;
   p->sampler_view_release = u_default_sampler_view_release;
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
