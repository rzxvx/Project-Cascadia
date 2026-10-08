/*
 * Copyright 2026 Project Cascadia
 * SPDX-License-Identifier: MIT
 *
 * Draws, for now (docs/research/p105-mesa.md, M13a).  The vertex shader
 * runs on the CPU, in Gallium's draw module (tgsi_exec, as i915 has it):
 * it also clips, culls and turns lines and points into triangles.  The
 * triangles are drawn on the GPU through the template frame, whose programs
 * give each pixel the colour interpolated between its triangle's vertices.
 * So the fragment shader is not run but read: one whose colour is, channel
 * by channel, a constant or a component of a varying or of a uniform is
 * drawn right, the colour worked out per vertex here; any other is drawn
 * in grey, with a warning once.  The vertices go to the GPU as the frame's
 * vertex side takes them (M13b): the position, then the colour as a
 * varying.  Only render targets the frame can fill
 * are drawn into (B8G8R8A8, up to 4096 x 4096: M10); depth, stencil, blending,
 * scissors and colour masks are not applied yet.  Each draw is a render.
 */
#include "sgx_draw.h"

#include <errno.h>
#include <stdlib.h>

#include "compiler/glsl_types.h"
#include "compiler/nir/nir.h"
#include "draw/draw_context.h"
#include "draw/draw_vbuf.h"
#include "draw/draw_vertex.h"
#include "tgsi/tgsi_from_mesa.h"
#include "util/format/u_format.h"
#include "util/log.h"
#include "util/u_debug.h"
#include "util/u_inlines.h"
#include "util/u_math.h"
#include "util/u_memory.h"
#include "util/u_prim.h"

#include "sgx_compiler.h"
#include "sgx_context.h"
#include "sgx_device.h"
#include "sgx_frame.h"
#include "sgx_resource.h"
#include "sgx_screen.h"

/* ---- the fragment shader's colour --------------------------------------- */

static int
type_size_vec4(const struct glsl_type *type, bool bindless)
{
   return glsl_count_attribute_slots(type, false);
}

/* where one channel of the colour comes from; false if from anything else */
static bool
colour_source(nir_scalar s, struct sgx_fs_colour *c, unsigned i)
{
   nir_intrinsic_instr *in;
   nir_src *offset;

   s = nir_scalar_chase_movs(s);
   if (nir_scalar_is_const(s)) {
      c->ch[i].src = SGX_SRC_CONST;
      c->ch[i].value = s.def->bit_size == 32 ? nir_scalar_as_float(s) : 0;
      return s.def->bit_size == 32;
   }
   if (!(in = nir_scalar_as_intrinsic(s)))
      return false;
   switch (in->intrinsic) {
   case nir_intrinsic_load_input:
   case nir_intrinsic_load_interpolated_input:
      offset = nir_get_io_offset_src(in);
      if (!offset || !nir_src_is_const(*offset))
         return false;
      c->ch[i].src = SGX_SRC_VARYING;
      c->ch[i].slot = nir_intrinsic_io_semantics(in).location + nir_src_as_uint(*offset);
      c->ch[i].comp = nir_intrinsic_component(in) + s.comp;
      return true;
   case nir_intrinsic_load_uniform:
      /* vec4 slots (the screen does not pack uniforms) */
      if (!nir_src_is_const(in->src[0]))
         return false;
      c->ch[i].src = SGX_SRC_UNIFORM;
      c->ch[i].slot = (nir_intrinsic_base(in) + nir_src_as_uint(in->src[0])) * 4 + s.comp;
      return true;
   case nir_intrinsic_load_ubo:
      /* bytes, buffer 0 */
      if (!nir_src_is_const(in->src[0]) || nir_src_as_uint(in->src[0]) ||
          !nir_src_is_const(in->src[1]))
         return false;
      c->ch[i].src = SGX_SRC_UNIFORM;
      c->ch[i].slot = nir_src_as_uint(in->src[1]) / 4 + s.comp;
      return true;
   default:
      return false;
   }
}

void
sgx_fs_colour_analyse(const struct nir_shader *fs, struct sgx_fs_colour *out)
{
   nir_shader *nir = nir_shader_clone(NULL, fs);
   bool have[4] = { false };

   memset(out, 0, sizeof(*out));
   NIR_PASS(_, nir, nir_lower_io, nir_var_shader_in | nir_var_shader_out | nir_var_uniform,
            type_size_vec4, 0);
   NIR_PASS(_, nir, nir_opt_copy_prop);
   NIR_PASS(_, nir, nir_opt_constant_folding);
   NIR_PASS(_, nir, nir_opt_dce);

   out->ok = true;
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            nir_intrinsic_instr *in;
            unsigned loc, mask, comp;

            if (instr->type != nir_instr_type_intrinsic)
               continue;
            in = nir_instr_as_intrinsic(instr);
            if (in->intrinsic == nir_intrinsic_terminate ||
                in->intrinsic == nir_intrinsic_terminate_if ||
                in->intrinsic == nir_intrinsic_demote ||
                in->intrinsic == nir_intrinsic_demote_if) {
               out->ok = false;         /* discard */
               continue;
            }
            if (in->intrinsic != nir_intrinsic_store_output)
               continue;
            loc = nir_intrinsic_io_semantics(in).location;
            if (loc != FRAG_RESULT_COLOR && loc != FRAG_RESULT_DATA0)
               continue;
            if (block->cf_node.parent->type != nir_cf_node_function)
               out->ok = false;         /* inside control flow: not one value */
            mask = nir_intrinsic_write_mask(in);
            comp = nir_intrinsic_component(in);
            for (unsigned i = 0; i < in->src[0].ssa->num_components; i++) {
               if (!(mask & 1 << i) || comp + i >= 4)
                  continue;
               out->ok &= colour_source(nir_get_scalar(in->src[0].ssa, i), out, comp + i);
               have[comp + i] = true;
            }
         }
      }
   }
   for (unsigned i = 0; i < 4; i++) {
      if (!have[i]) {
         out->ch[i].src = SGX_SRC_CONST;
         out->ch[i].value = i == 3 ? 1.0f : 0.0f;
      }
   }
   ralloc_free(nir);
}

/* ---- the draw module's back end ----------------------------------------- */

/* what the draw module hands over for each vertex: the position (window
 * coordinates), then each varying the colour reads, four floats each */
struct sgx_render {
   struct vbuf_render base;
   struct sgx_context *ctx;
   struct vertex_info vinfo;
   unsigned varying[4];         /* the varyings, by gl_varying_slot */
   unsigned nvarying;
   enum mesa_prim prim;
   uint8_t *vertices;           /* the draw module's, vertex_size each */
   unsigned vertex_size, capacity;
};

static struct sgx_render *
sgx_render(struct vbuf_render *r)
{
   return (struct sgx_render *)r;
}

static const struct vertex_info *
render_vertex_info(struct vbuf_render *r)
{
   return &sgx_render(r)->vinfo;
}

static bool
render_allocate(struct vbuf_render *r, uint16_t vertex_size, uint16_t n)
{
   struct sgx_render *sr = sgx_render(r);
   unsigned need = (unsigned)vertex_size * n;

   if (need > sr->capacity) {
      uint8_t *p = REALLOC(sr->vertices, sr->capacity, need);

      if (!p)
         return false;
      sr->vertices = p;
      sr->capacity = need;
   }
   sr->vertex_size = vertex_size;
   return true;
}

static void *
render_map(struct vbuf_render *r)
{
   return sgx_render(r)->vertices;
}

static void
render_unmap(struct vbuf_render *r, uint16_t min_index, uint16_t max_index)
{
}

static void
render_set_primitive(struct vbuf_render *r, enum mesa_prim prim)
{
   sgx_render(r)->prim = prim;
}

static void
render_set_view_index(struct vbuf_render *r, unsigned view_index)
{
}

/* one vertex, as the frame's vertex side takes it: x y over the target, z
 * 0, w 1, then the varyings -- the colour, and with SGX_DRAW_LAYOUT the
 * fillers around it */
static void
emit_vertex(struct sgx_render *sr, const float *in, float *out)
{
   struct sgx_context *ctx = sr->ctx;
   const struct sgx_fs_colour *c = ctx->fs ? &ctx->fs->colour : NULL;
   const float *cb = ctx->fs_constants;
   const struct sgx_frame_layout *l = &ctx->layout;

   /* clip coordinates, as a vertex shader would hand them to the TA (it
    * divides by w, and interpolates the varyings with it): the draw
    * module's window coordinates back to -1..1 -- z too, the frame's
    * viewport takes it to depth as z / 2 + 1 / 2 -- times w, which the
    * draw module keeps as 1 / w */
   float w = in[3] > 0 ? 1 / in[3] : 1;

   out[0] = (in[0] * 2 / ctx->fb.width - 1) * w;
   out[1] = (in[1] * 2 / ctx->fb.height - 1) * w;
   out[2] = (in[2] * 2 - 1) * w;
   out[3] = w;
   if (ctx->draw_fs) {
      /* the program's inputs, as the draw module has them */
      memcpy(out + 4, in + 4, 4 * l->nvaryings * sizeof(float));
      return;
   }
   for (unsigned k = 0; k < l->nvaryings; k++) {
      /* a filler: a colour the checks would notice */
      out[4 + 4 * k + 0] = 1;
      out[4 + 4 * k + 1] = 0;
      out[4 + 4 * k + 2] = 1;
      out[4 + 4 * k + 3] = (k + 1) / 16.0f;
   }
   out += 4 + 4 * l->colour;
   for (unsigned i = 0; i < 4; i++) {
      float v = 0.5f;

      if (c && c->ok) {
         switch (c->ch[i].src) {
         case SGX_SRC_CONST:
            v = c->ch[i].value;
            break;
         case SGX_SRC_UNIFORM:
            v = cb && c->ch[i].slot < ctx->fs_constants_size / 4 ? cb[c->ch[i].slot] : 0;
            break;
         case SGX_SRC_VARYING:
            v = 0;
            for (unsigned k = 0; k < sr->nvarying; k++)
               if (sr->varying[k] == c->ch[i].slot)
                  v = in[4 + 4 * k + c->ch[i].comp];
            break;
         }
      }
      out[i] = CLAMP(v, 0.0f, 1.0f);
   }
}

static void
render_index(struct sgx_render *sr, unsigned index)
{
   struct sgx_context *ctx = sr->ctx;
   unsigned vf = sgx_frame_vertex_floats(&ctx->layout);

   if ((ctx->nverts + 1) * vf > ctx->maxfloats) {
      unsigned n = MAX2(ctx->maxfloats * 2, 3 * 256 * vf);
      float *p = REALLOC(ctx->verts, ctx->maxfloats * sizeof(float), n * sizeof(float));

      if (!p)
         return;
      ctx->verts = p;
      ctx->maxfloats = n;
   }
   emit_vertex(sr, (const float *)(sr->vertices + index * sr->vertex_size),
               ctx->verts + ctx->nverts++ * vf);

   /* every triangle anticlockwise, as the clear's quad is: the draw module
    * has culled what GL culls, and the pack's state may cull the rest */
   if (ctx->nverts % 3 == 0) {
      float *t = ctx->verts + (ctx->nverts - 3) * vf;
      float tmp[4 * (1 + SGX_FRAME_MAX_VARYINGS)];

      /* (on the screen: x / w, y / w) */
      float x0 = t[0] / t[3], y0 = t[1] / t[3], x1 = t[vf] / t[vf + 3];
      float y1 = t[vf + 1] / t[vf + 3], x2 = t[2 * vf] / t[2 * vf + 3];
      float y2 = t[2 * vf + 1] / t[2 * vf + 3];

      if ((x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0) < 0) {
         memcpy(tmp, t + vf, vf * sizeof(float));
         memcpy(t + vf, t + 2 * vf, vf * sizeof(float));
         memcpy(t + 2 * vf, tmp, vf * sizeof(float));
      }
   }
}

static void
render_draw_elements(struct vbuf_render *r, const uint16_t *indices, unsigned n)
{
   struct sgx_render *sr = sgx_render(r);

   if (sr->prim != MESA_PRIM_TRIANGLES) {
      mesa_logw_once("sgx: %s left over by the draw module: dropped",
                     u_prim_name(sr->prim));
      return;
   }
   for (unsigned i = 0; i < n - n % 3; i++)
      render_index(sr, indices[i]);
}

static void
render_draw_arrays(struct vbuf_render *r, unsigned start, unsigned n)
{
   struct sgx_render *sr = sgx_render(r);

   if (sr->prim != MESA_PRIM_TRIANGLES) {
      mesa_logw_once("sgx: %s left over by the draw module: dropped",
                     u_prim_name(sr->prim));
      return;
   }
   for (unsigned i = 0; i < n - n % 3; i++)
      render_index(sr, start + i);
}

static void
render_release(struct vbuf_render *r)
{
}

static void
render_destroy(struct vbuf_render *r)
{
   FREE(sgx_render(r)->vertices);
   FREE(r);
}

static bool
render_need_pipeline(const struct vbuf_render *r, const struct pipe_rasterizer_state *rast,
                     unsigned prim)
{
   return true;         /* everything as triangles, one by one */
}

/* One varying of the draw module's output vertex, four floats, named as
 * nir_to_tgsi names the vertex shader's outputs: VARn is GENERIC n (no
 * shift here), the rest as TGSI has them */
static void
emit_varying(struct sgx_context *ctx, struct vertex_info *vinfo, unsigned slot)
{
   unsigned name, index;
   int out;

   if (slot >= VARYING_SLOT_VAR0 && slot < VARYING_SLOT_PATCH0) {
      name = TGSI_SEMANTIC_GENERIC;
      index = slot - VARYING_SLOT_VAR0;
   } else {
      tgsi_get_gl_varying_semantic(slot, true, &name, &index);
   }
   out = draw_find_shader_output(ctx->draw, name, index);
   draw_emit_vertex_attr(vinfo, EMIT_4F, out);
   if (ctx->debug_draw && out < 0)
      mesa_logi("sgx: the fragment shader reads varying slot %u (TGSI %u %u), which the "
                "vertex shader does not write", slot, name, index);
}

/* What goes to the GPU: the position and the colour, as a varying.
 * SGX_DRAW_LAYOUT=N[,K][,f32] (a test of the frame's vertex side) makes
 * that N varyings, the colour the K-th (the last by default), F32 or
 * not, the rest fillers. */
static struct sgx_frame_layout
draw_layout(void)
{
   static struct sgx_frame_layout l;
   static bool parsed;

   if (!parsed) {
      const char *env = getenv("SGX_DRAW_LAYOUT");
      char *e;

      l.nvaryings = 1;
      if (env && *env) {
         l.nvaryings = CLAMP(strtoul(env, &e, 0), 1, SGX_FRAME_MAX_VARYINGS);
         l.colour = l.nvaryings - 1;
         if (*e == ',' && e[1] >= '0' && e[1] <= '9')
            l.colour = MIN2(strtoul(e + 1, &e, 0), l.nvaryings - 1);
         if (strstr(env, "f32"))
            l.f32 = 1u << l.colour;
         mesa_logi("sgx: SGX_DRAW_LAYOUT: %u varyings, the colour the %u-th%s",
                   l.nvaryings, l.colour, l.f32 ? ", F32" : "");
      }
      parsed = true;
   }
   return l;
}

/* The vertex layout for the bound shaders: the position, then the varyings
 * the fragment colour reads (as the vertex shader's outputs are named) */
static void
update_vertex_info(struct sgx_context *ctx)
{
   struct sgx_render *sr = sgx_render(ctx->render);
   const struct sgx_fs_colour *c = ctx->fs ? &ctx->fs->colour : NULL;
   struct vertex_info *vinfo = &sr->vinfo;

   memset(vinfo, 0, sizeof(*vinfo));
   sr->nvarying = 0;
   draw_emit_vertex_attr(vinfo, EMIT_4F,
                         draw_find_shader_output(ctx->draw, TGSI_SEMANTIC_POSITION, 0));
   if (ctx->draw_fs) {
      /* a compiled fragment shader: its inputs, in its order, all F32 */
      const struct sgx_fs *fs = ctx->draw_fs;

      for (unsigned i = 0; i < fs->prog.ninputs; i++)
         if (fs->input_slot[i] != VARYING_SLOT_POS)     /* gl_FragCoord: iterated, not sent */
            emit_varying(ctx, vinfo, fs->input_slot[i]);
      draw_compute_vertex_size(vinfo);
      ctx->layout.nvaryings = fs->prog.nvaryings;
      ctx->layout.f32 = (1u << fs->prog.nvaryings) - 1;
      ctx->layout.colour = 0;
      return;
   }
   for (unsigned i = 0; c && c->ok && i < 4; i++) {
      unsigned k;

      if (c->ch[i].src != SGX_SRC_VARYING)
         continue;
      for (k = 0; k < sr->nvarying && sr->varying[k] != c->ch[i].slot; k++)
         ;
      if (k < sr->nvarying)
         continue;
      sr->varying[sr->nvarying++] = c->ch[i].slot;
      emit_varying(ctx, vinfo, c->ch[i].slot);
   }
   draw_compute_vertex_size(vinfo);

   ctx->layout = draw_layout();
}

bool
sgx_draw_init(struct sgx_context *ctx)
{
   struct sgx_render *sr = CALLOC_STRUCT(sgx_render);

   if (!sr)
      return false;
   sr->ctx = ctx;
   sr->base.max_indices = 16 * 1024;
   sr->base.max_vertex_buffer_bytes = 1024 * 1024;
   sr->base.need_pipeline = render_need_pipeline;
   sr->base.get_vertex_info = render_vertex_info;
   sr->base.allocate_vertices = render_allocate;
   sr->base.map_vertices = render_map;
   sr->base.unmap_vertices = render_unmap;
   sr->base.set_primitive = render_set_primitive;
   sr->base.set_view_index = render_set_view_index;
   sr->base.draw_elements = render_draw_elements;
   sr->base.draw_arrays = render_draw_arrays;
   sr->base.release_vertices = render_release;
   sr->base.destroy = render_destroy;
   ctx->render = &sr->base;

   if (!(ctx->draw = draw_create_no_llvm(&ctx->base))) {
      render_destroy(&sr->base);
      ctx->render = NULL;
      return false;
   }
   ctx->vbuf = draw_vbuf_stage(ctx->draw, ctx->render);
   if (!ctx->vbuf)
      return false;
   draw_set_rasterize_stage(ctx->draw, ctx->vbuf);
   draw_set_render(ctx->draw, ctx->render);
   /* lines and points as triangles, whatever their width */
   draw_wide_line_threshold(ctx->draw, 0.0f);
   draw_wide_point_threshold(ctx->draw, 0.0f);
   draw_enable_line_stipple(ctx->draw, false);
   draw_enable_point_sprites(ctx->draw, false);
   return true;
}

void
sgx_draw_fini(struct sgx_context *ctx)
{
   if (ctx->draw)
      draw_destroy(ctx->draw);
   else if (ctx->render)
      render_destroy(ctx->render);
   ctx->draw = NULL;
   ctx->render = NULL;
   sgx_batch_flush(ctx);
   free(ctx->batch.verts);
   free(ctx->batch.sa);
   free(ctx->batch.idx);
   ctx->batch.verts = NULL;
   ctx->batch.sa = NULL;
   ctx->batch.idx = NULL;
   FREE(ctx->verts);
   free(ctx->indices);
   ctx->verts = NULL;
   ctx->indices = NULL;
   ctx->nverts = ctx->maxfloats = ctx->nindices = ctx->maxindices = 0;
}

/* ---- draw_vbo ----------------------------------------------------------- */

/* The fragment shader as this draw runs it: compiled with the bound blend
 * state folded in (a variant made at the first draw that needs it), or
 * NULL for M13a's way. */
static struct sgx_fs *
fs_for_draw(struct sgx_context *ctx)
{
   struct sgx_shader *sh = ctx->fs;
   const struct pipe_rt_blend_state *rt = ctx->blend ? &ctx->blend->rt[0] : NULL;
   struct sgx_blend_key key = { .colormask = 0xf };
   struct sgx_fs *fs;
   char why[128];

   if (!sh || !sh->compiled)
      return NULL;
   if (rt) {
      key.colormask = rt->colormask;
      if (rt->blend_enable) {
         key.enable = 1;
         key.rgb_func = rt->rgb_func;
         key.rgb_src = rt->rgb_src_factor;
         key.rgb_dst = rt->rgb_dst_factor;
         key.alpha_func = rt->alpha_func;
         key.alpha_src = rt->alpha_src_factor;
         key.alpha_dst = rt->alpha_dst_factor;
      }
   }
   /* textures sampled linear in other orders than B G R A (M16) */
   for (unsigned i = 0; i < sh->compiled->nsamplers; i++) {
      unsigned unit = sh->compiled->sampler_unit[i];
      struct pipe_sampler_view *view = unit < ARRAY_SIZE(ctx->fs_views) ?
                                       ctx->fs_views[unit] : NULL;
      bool swap, x8;

      if (unit < 8 && view && sgx_resource_linear(sgx_resource(view->texture), &swap, &x8)) {
         key.tex_swap |= swap << unit;
         key.tex_x8 |= x8 << unit;
      }
   }
   if (!key.enable && key.colormask == 0xf && !key.tex_swap && !key.tex_x8)
      return sh->compiled;
   for (unsigned i = 0; i < sh->nvariants; i++)
      if (!memcmp(&sh->variant[i]->blend, &key, sizeof(key)))
         return sh->variant[i];
   if (!(fs = sgx_compile_fs(sh->nir, &key, why, sizeof(why)))) {
      mesa_logw_once("sgx: a variant of a fragment shader (blending, texture orders) "
                     "not compiled (%s): drawn without it", why);
      return sh->compiled;
   }
   if (sh->nvariants == ARRAY_SIZE(sh->variant)) {
      /* its code stays where the frame put it: that place is not reused */
      sgx_fs_destroy(sh->variant[0]);
      memmove(sh->variant, sh->variant + 1, (ARRAY_SIZE(sh->variant) - 1) * sizeof(fs));
      sh->nvariants--;
   }
   sh->variant[sh->nvariants++] = fs;
   return fs;
}

static const void *
map_buffer(struct pipe_resource *prsc, size_t *size)
{
   struct sgx_resource *res = sgx_resource(prsc);
   uint8_t *map;

   if (!res || !res->bo || !(map = sgx_bo_map(res->bo)))
      return NULL;
   /* the CPU reads what a render may still write: wait for it */
   sgx_bo_wait(res->bo, -1);
   *size = prsc->width0;
   return map;
}

/* ---- gathering draws into renders (M14) -------------------------------- */

bool
sgx_batch_uses(struct sgx_context *ctx, struct pipe_resource *p)
{
   struct sgx_batch *b = &ctx->batch;

   if (!b->ndraws || !p)
      return false;
   if (b->rt == p)
      return true;
   for (unsigned i = 0; i < b->ntex; i++)
      if (b->tex[i] == p)
         return true;
   return false;
}

void
sgx_batch_flush(struct sgx_context *ctx)
{
   struct sgx_screen *screen = sgx_screen(ctx->base.screen);
   struct sgx_batch *b = &ctx->batch;
   struct sgx_frame_draw d[SGX_FRAME_MAX_DRAWS];
   struct sgx_fence *fence;
   int ret = -ENOMEM;

   if (!b->ndraws)
      return;
   for (unsigned i = 0; i < b->ndraws; i++) {
      const struct sgx_batch_draw *bd = &b->draw[i];

      d[i] = (struct sgx_frame_draw){
         .l = bd->l, .verts = b->verts + bd->first, .nverts = bd->nverts,
         .prog = bd->compiled ? &bd->prog : NULL, .sa = b->sa + bd->sa, .st = bd->st,
         .vs = bd->vs, .vs_sa = b->sa + bd->vs_sa,
         .indices = bd->vs ? b->idx + bd->idx : NULL, .nindices = bd->nidx,
      };
   }
   if ((fence = sgx_fence_create(&screen->dev, false))) {
      simple_mtx_lock(&screen->frame_lock);
      ret = sgx_frame_render(screen->frame, sgx_resource(b->rt), d, b->ndraws, b->handles,
                             b->ntex, b->depth_clear, fence);
      simple_mtx_unlock(&screen->frame_lock);
   }
   if (!ret) {
      sgx_fence_reference(&ctx->last, fence);
      sgx_resource(b->rt)->seq++;
      sgx_resource(b->rt)->gpu_written = true;
   } else {
      mesa_logw("sgx: a render of %u draws failed (%d)", b->ndraws, ret);
   }
   sgx_fence_reference(&fence, NULL);
   pipe_resource_reference(&b->rt, NULL);
   for (unsigned i = 0; i < b->ntex; i++)
      pipe_resource_reference(&b->tex[i], NULL);
   b->ndraws = b->ntex = b->nfloats = b->nsa = b->nidx = b->cursor = 0;
}

static bool
grow(void **p, unsigned *max, unsigned want, unsigned size)
{
   unsigned n;
   void *q;

   if (want <= *max)
      return true;
   n = MAX2(want, *max * 2);
   if (!(q = realloc(*p, (size_t)n * size)))
      return false;
   *p = q;
   *max = n;
   return true;
}

/* One draw's triangles (ctx->verts, ctx->nverts) added to the gathered
 * render, in pieces when they are more than one draw of a render takes */
static void
submit(struct sgx_context *ctx)
{
   struct sgx_screen *screen = sgx_screen(ctx->base.screen);
   struct sgx_batch *b = &ctx->batch;
   struct pipe_surface *surf = &ctx->fb.cbufs[0];
   struct sgx_resource *rt = surf->texture ? sgx_resource(surf->texture) : NULL;
   struct sgx_fs *fs = ctx->draw_fs;
   uint32_t sa[128 + 4 * SGX_FS_MAX_SAMPLERS + 4];
   struct pipe_resource *tex[SGX_FS_MAX_SAMPLERS];
   struct sgx_vs *vs = ctx->gpu_vs;
   unsigned vf = vs ? 4 * vs->nattrs : sgx_frame_vertex_floats(&ctx->layout);
   unsigned nsa = fs ? fs->prog.nsa : 0, nvsa = vs ? vs->nuniforms : 0;
   unsigned max, done = 0;
   struct sgx_frame_state st = {
      .depth_func = ctx->dsa && ctx->dsa->depth_enabled ? ctx->dsa->depth_func : PIPE_FUNC_ALWAYS,
      .depth_write = ctx->dsa && ctx->dsa->depth_enabled && ctx->dsa->depth_writemask,
   };

   if (!ctx->nverts)
      return;
   if (ctx->debug_draw && !vs) {
      mesa_logi("sgx: %u triangles; the first:", ctx->nverts / 3);
      for (unsigned i = 0; i < MIN2(ctx->nverts, 3); i++) {
         const float *v = ctx->verts + i * vf;
         const float *c = v + 4 + 4 * ctx->layout.colour;

         mesa_logi("sgx:   x %8.4f y %8.4f z %8.4f  colour %.3f %.3f %.3f %.3f",
                   v[0], v[1], v[2], c[0], c[1], c[2], c[3]);
      }
   }
   if (!screen->frame) {
      mesa_logw_once("sgx: no template frame (./cascadia gpu installs it): draws are dropped");
      ctx->nverts = 0;
      return;
   }
   if (!rt || ctx->fb.nr_cbufs < 1 || surf->level || surf->first_layer ||
       !sgx_frame_can_render(screen->frame, rt)) {
      mesa_logw_once("sgx: draws only go into the B8G8R8A8 targets the template frame "
                     "can fill (level 0, up to 4096 x 4096); others are dropped");
      ctx->nverts = 0;
      return;
   }
   if (b->ndraws && b->rt != &rt->base)
      sgx_batch_flush(ctx);

   /* SGX_FRAME=packvertex, packpixel: the pack's sides, a render a draw,
    * M13a's colour (debugging only) */
   if (!vs && (sgx_frame_options() & (SGX_FRAME_PACKVTX | SGX_FRAME_PACKPIX))) {
      sgx_batch_flush(ctx);
      max = sgx_frame_max_vertices(screen->frame, &ctx->layout);
      for (done = 0; done < ctx->nverts; done += max) {
         unsigned n = MIN2(ctx->nverts - done, max);
         struct sgx_fence *fence = sgx_fence_create(&screen->dev, false);

         if (!fence)
            break;
         simple_mtx_lock(&screen->frame_lock);
         if (!sgx_frame_draw(screen->frame, rt, &ctx->layout, ctx->verts + done * vf, n,
                             NULL, NULL, NULL, 0, &st, fence)) {
            sgx_fence_reference(&ctx->last, fence);
            rt->seq++;
            rt->gpu_written = true;
         }
         simple_mtx_unlock(&screen->frame_lock);
         sgx_fence_reference(&fence, NULL);
      }
      ctx->nverts = 0;
      return;
   }

   /* a compiled fragment shader's secondary attributes: its uniforms,
    * then the state words of each texture it samples, the blend colour */
   if (fs) {
      unsigned n = MIN2(fs->nuniforms, ctx->fs_constants_size / 4);

      memset(sa, 0, sizeof(sa));
      if (ctx->fs_constants)
         memcpy(sa, ctx->fs_constants, n * sizeof(float));
      for (unsigned i = 0; i < fs->nsamplers; i++) {
         unsigned unit = fs->sampler_unit[i];
         struct pipe_sampler_view *view = unit < ARRAY_SIZE(ctx->fs_views) ?
                                          ctx->fs_views[unit] : NULL;
         struct sgx_resource *t = view ? sgx_resource(view->texture) : NULL;

         /* its copy is about to be made again (or it is the target):
          * what was gathered reads the old one, so render that first */
         if (t && sgx_batch_uses(ctx, &t->base) &&
             (&t->base == b->rt || !t->tw || t->tw_seq != t->seq))
            sgx_batch_flush(ctx);
         if (!t || !sgx_resource_texture(screen, t, unit < PIPE_MAX_SAMPLERS ?
                                         ctx->fs_samplers[unit] : NULL,
                                         sa + fs->sampler_sa + 4 * i)) {
            mesa_logw_once("sgx: a draw samples a texture unit with nothing it can "
                           "sample bound: dropped");
            ctx->nverts = 0;
            return;
         }
         tex[i] = &t->base;
      }
      if (fs->blend_sa >= 0)
         memcpy(sa + fs->blend_sa, ctx->blend_color.color, 4 * sizeof(float));
      simple_mtx_lock(&screen->frame_lock);
      if (sgx_frame_upload(screen->frame, &fs->prog) ||
          (vs && sgx_frame_upload_vs(screen->frame, vs))) {
         simple_mtx_unlock(&screen->frame_lock);
         ctx->nverts = 0;
         return;
      }
      simple_mtx_unlock(&screen->frame_lock);
   }

   max = vs ? ctx->nverts : sgx_frame_max_vertices(screen->frame, &ctx->layout);
   while (done < ctx->nverts) {
      unsigned n = MIN2(ctx->nverts - done, max), first, ntex = b->ntex;
      unsigned cursor = b->cursor, nidx = vs ? ctx->nindices : 0;
      struct sgx_batch_draw *bd;

      /* room in this render: a draw, its vertices, its textures */
      for (unsigned i = 0; fs && i < fs->nsamplers; i++) {
         unsigned k;

         for (k = 0; k < b->ntex && b->tex[k] != tex[i]; k++)
            ;
         ntex += k == b->ntex;
      }
      if (b->ndraws == SGX_FRAME_MAX_DRAWS || ntex > SGX_FRAME_MAX_HANDLES ||
          b->nidx + nidx + 8 * (b->ndraws + 1) > SGX_FRAME_MAX_INDICES ||
          !(vs ? sgx_frame_place_indexed(screen->frame, &cursor, vf * 4, n, &first) :
                 sgx_frame_place(screen->frame, &cursor, &ctx->layout, n, &first))) {
         sgx_batch_flush(ctx);
         cursor = 0;
         if (nidx + 8 > SGX_FRAME_MAX_INDICES ||
             !(vs ? sgx_frame_place_indexed(screen->frame, &cursor, vf * 4, n, &first) :
                    sgx_frame_place(screen->frame, &cursor, &ctx->layout, n, &first))) {
            mesa_logw_once("sgx: a draw larger than a render takes: dropped");
            break;
         }
      }
      if (!grow((void **)&b->verts, &b->maxfloats, b->nfloats + n * vf, sizeof(float)) ||
          !grow((void **)&b->sa, &b->maxsa, b->nsa + nsa + nvsa, sizeof(uint32_t)) ||
          !grow((void **)&b->idx, &b->maxidx, b->nidx + nidx, sizeof(uint16_t)))
         break;
      if (!b->ndraws)
         pipe_resource_reference(&b->rt, &rt->base);
      /* from now on sampled linear: decided before any draw samples it */
      rt->gpu_written = true;
      for (unsigned i = 0; fs && i < fs->nsamplers; i++) {
         unsigned k;

         for (k = 0; k < b->ntex && b->tex[k] != tex[i]; k++)
            ;
         if (k == b->ntex) {
            pipe_resource_reference(&b->tex[b->ntex], tex[i]);
            b->handles[b->ntex++] = sgx_resource(tex[i])->sampled->handle;
         }
      }
      bd = &b->draw[b->ndraws++];
      bd->l = ctx->layout;
      bd->first = b->nfloats;
      bd->nverts = n;
      bd->compiled = fs != NULL;
      if (fs)
         bd->prog = fs->prog;
      bd->sa = b->nsa;
      bd->st = st;
      bd->vs = vs;
      bd->vs_sa = b->nsa + nsa;
      bd->idx = b->nidx;
      bd->nidx = nidx;
      memcpy(b->verts + b->nfloats, ctx->verts + done * vf, n * vf * sizeof(float));
      memcpy(b->sa + b->nsa, sa, nsa * sizeof(uint32_t));
      memcpy(b->sa + b->nsa + nsa, ctx->vs_sa, nvsa * sizeof(uint32_t));
      memcpy(b->idx + b->nidx, ctx->indices, nidx * sizeof(uint16_t));
      b->nfloats += n * vf;
      b->nsa += nsa + nvsa;
      b->nidx += nidx;
      b->cursor = cursor;
      done += n;
   }
   ctx->nverts = 0;
}

/* ---- the vertex shader on the GPU (M18) --------------------------------- */

/* The vertex shader for the bound fragment shader's varyings: compiled
 * once for each list of them; NULL when it cannot be (the draw module's
 * way then) */
static struct sgx_vs *
vs_for_draw(struct sgx_context *ctx, const struct sgx_fs *fs)
{
   struct sgx_shader *sh = ctx->vs;
   unsigned slot[SGX_FRAME_MAX_VARYINGS], n = 0;
   struct sgx_vs *vs;
   char why[128];

   if (!sh || !sh->nir || sh->vs_failed)
      return NULL;
   for (unsigned i = 0; i < fs->prog.ninputs; i++)
      if (fs->input_slot[i] != VARYING_SLOT_POS)
         slot[n++] = fs->input_slot[i];
   for (unsigned i = 0; i < sh->nvs; i++)
      if (sh->vs_variant[i]->nvaryings == n &&
          !memcmp(sh->vs_variant[i]->varying_slot, slot, n * sizeof(*slot)))
         return sh->vs_variant[i];
   if (!(vs = sgx_compile_vs(sh->nir, slot, n, why, sizeof(why)))) {
      mesa_logw("sgx: a vertex shader not compiled (%s): run on the CPU", why);
      sh->vs_failed = true;
      return NULL;
   }
   if (debug_get_bool_option("SGX_DEBUG_SHADER", false)) {
      mesa_logi("sgx: a vertex shader: %u instructions, %u temps, %u attributes, %u "
                "uniform words", vs->ncode, vs->ntemps, vs->nattrs, vs->nuniforms);
      for (unsigned i = 0; i < vs->ncode; i++)
         mesa_logi("sgx:   %016llx", (unsigned long long)vs->code[i]);
   }
   if (sh->nvs == ARRAY_SIZE(sh->vs_variant)) {
      sgx_batch_flush(ctx);
      sgx_vs_destroy(sh->vs_variant[0]);
      memmove(sh->vs_variant, sh->vs_variant + 1, (ARRAY_SIZE(sh->vs_variant) - 1) * sizeof(vs));
      sh->nvs--;
   }
   sh->vs_variant[sh->nvs++] = vs;
   return vs;
}

static bool
index_at(const void *indices, unsigned size, unsigned i, unsigned *out)
{
   switch (size) {
   case 1: *out = ((const uint8_t *)indices)[i]; return true;
   case 2: *out = ((const uint16_t *)indices)[i]; return true;
   case 4: *out = ((const uint32_t *)indices)[i]; return true;
   default: return false;
   }
}

/* What the GPU's vertex side does not do yet: then the draw module's way */
static bool
gpu_vs_can_draw(struct sgx_context *ctx, const struct pipe_draw_info *info)
{
   const struct pipe_rasterizer_state *r = ctx->rast;
   const struct pipe_viewport_state *vp = &ctx->viewport;
   static int off = -1;

   if (off < 0)
      off = debug_get_bool_option("SGX_CPU_VS", false);
   if (off || !ctx->draw_fs || !r || !ctx->velems)
      return false;
   if (info->mode != MESA_PRIM_TRIANGLES && info->mode != MESA_PRIM_TRIANGLE_STRIP &&
       info->mode != MESA_PRIM_TRIANGLE_FAN)
      return false;
   if (info->primitive_restart || r->cull_face != PIPE_FACE_NONE ||
       r->fill_front != PIPE_POLYGON_MODE_FILL || r->fill_back != PIPE_POLYGON_MODE_FILL ||
       r->clip_plane_enable || r->flatshade)
      return false;
   /* the TA's viewport is the target's for now: GL's whole framebuffer,
    * the right way up, depth 0..1 */
   return vp->scale[0] == ctx->fb.width / 2.0f && vp->translate[0] == ctx->fb.width / 2.0f &&
          vp->scale[1] == ctx->fb.height / 2.0f && vp->translate[1] == ctx->fb.height / 2.0f &&
          vp->scale[2] == 0.5f && vp->translate[2] == 0.5f;
}

/* One draw with the vertex shader on the GPU: its vertices' attributes
 * unpacked to F32 vec4s, its triangles as indices into them (strips and
 * fans made lists), its uniforms; false: not done, the caller's way. */
static bool
gpu_vs_draw(struct sgx_context *ctx, const struct pipe_draw_info *info,
            const struct pipe_draw_start_count_bias *draw)
{
   const struct sgx_vertex_elements *ve = ctx->velems;
   const void *indices = NULL;
   struct sgx_vs *vs;
   unsigned lo = ~0u, hi = 0, ntri, vf;
   int bias = info->index_size ? draw->index_bias : 0;

   if (draw->count < 3 || !(vs = vs_for_draw(ctx, ctx->draw_fs)))
      return false;
   ntri = info->mode == MESA_PRIM_TRIANGLES ? draw->count / 3 : draw->count - 2;

   /* the vertices the draw reads */
   if (info->index_size) {
      size_t size = ~(size_t)0;

      indices = info->has_user_indices ? info->index.user :
                map_buffer(info->index.resource, &size);
      if (!indices)
         return false;
      indices = (const uint8_t *)indices + draw->start * info->index_size;
      for (unsigned i = 0; i < draw->count; i++) {
         unsigned x;

         if (!index_at(indices, info->index_size, i, &x))
            return false;
         lo = MIN2(lo, x);
         hi = MAX2(hi, x);
      }
   } else {
      lo = draw->start;
      hi = draw->start + draw->count - 1;
   }
   if (hi - lo + 1 > 65536 || (int)lo + bias < 0)
      return false;

   /* their attributes, four floats each, attribute n from vertex element n */
   vf = 4 * vs->nattrs;
   ctx->nverts = 0;
   if ((hi - lo + 1) * vf > ctx->maxfloats) {
      unsigned n = (hi - lo + 1) * vf;
      float *p = REALLOC(ctx->verts, ctx->maxfloats * sizeof(float), n * sizeof(float));

      if (!p)
         return false;
      ctx->verts = p;
      ctx->maxfloats = n;
   }
   for (unsigned a = 0; a < vs->nattrs; a++) {
      const struct pipe_vertex_element *e = a < ve->count ? &ve->e[a] : NULL;
      const struct pipe_vertex_buffer *vb = e ? &ctx->vb[e->vertex_buffer_index] : NULL;
      const uint8_t *base = NULL;

      if (vb && (ctx->vb_mask & 1u << e->vertex_buffer_index)) {
         if (vb->is_user_buffer) {
            base = vb->buffer.user;
         } else if (vb->buffer.resource) {
            size_t size;

            base = map_buffer(vb->buffer.resource, &size);
         }
         if (base)
            base += vb->buffer_offset + e->src_offset;
      }
      for (unsigned v = 0; v <= hi - lo; v++) {
         float *out = ctx->verts + v * vf + 4 * a;
         unsigned at = e && e->instance_divisor ? info->start_instance / e->instance_divisor :
                       lo + bias + v;

         if (!base) {
            out[0] = out[1] = out[2] = 0;
            out[3] = 1;
            continue;
         }
         util_format_unpack_rgba(e->src_format, out, base + at * e->src_stride, 1);
      }
   }
   ctx->nverts = hi - lo + 1;

   /* the triangles, as a list */
   if (3 * ntri > ctx->maxindices) {
      uint16_t *p = realloc(ctx->indices, 3 * ntri * sizeof(uint16_t));

      if (!p)
         return false;
      ctx->indices = p;
      ctx->maxindices = 3 * ntri;
   }
   for (unsigned t = 0; t < ntri; t++) {
      unsigned k[3], x[3];

      if (info->mode == MESA_PRIM_TRIANGLES) {
         k[0] = 3 * t, k[1] = 3 * t + 1, k[2] = 3 * t + 2;
      } else if (info->mode == MESA_PRIM_TRIANGLE_STRIP) {
         /* every other triangle the other way round, as GL turns them */
         k[0] = t + (t & 1), k[1] = t + 1 - (t & 1), k[2] = t + 2;
      } else {
         k[0] = 0, k[1] = t + 1, k[2] = t + 2;
      }
      for (unsigned i = 0; i < 3; i++) {
         if (indices)
            index_at(indices, info->index_size, k[i], &x[i]);
         else
            x[i] = draw->start + k[i];
         ctx->indices[3 * t + i] = x[i] - lo;
      }
   }
   ctx->nindices = 3 * ntri;

   /* its uniforms: constant buffer 0's words */
   if (vs->nuniforms) {
      const struct pipe_constant_buffer *cb = &ctx->cb[0];
      const uint8_t *p = cb->user_buffer;
      size_t size;

      if (!p && cb->buffer && (p = map_buffer(cb->buffer, &size)))
         p += cb->buffer_offset;
      memset(ctx->vs_sa, 0, sizeof(ctx->vs_sa));
      if (p)
         memcpy(ctx->vs_sa, p, MIN2(vs->nuniforms * 4, cb->buffer_size));
   }

   ctx->layout.nvaryings = vs->nvaryings;
   ctx->layout.f32 = (1u << vs->nvaryings) - 1;
   ctx->layout.colour = 0;
   ctx->gpu_vs = vs;
   submit(ctx);
   ctx->gpu_vs = NULL;
   ctx->nverts = ctx->nindices = 0;
   return true;
}

void
sgx_draw_vbo(struct pipe_context *pctx, const struct pipe_draw_info *info,
             unsigned drawid_offset, const struct pipe_draw_indirect_info *indirect,
             const struct pipe_draw_start_count_bias *draws, unsigned num_draws)
{
   struct sgx_context *ctx = sgx_context(pctx);
   struct draw_context *draw = ctx->draw;
   const void *indices = NULL;
   unsigned i;

   if (indirect || !draw || !ctx->vs || !ctx->vs->draw)
      return;
   ctx->draw_fs = fs_for_draw(ctx);
   if (ctx->fs && !ctx->fs->compiled && !ctx->fs->colour.ok && !ctx->warned_fs) {
      mesa_logw("sgx: a fragment shader whose colour is not a varying, a constant or a "
                "uniform per channel: drawn grey (M13a)");
      ctx->warned_fs = true;
   }

   for (i = 0; i < PIPE_MAX_ATTRIBS; i++) {
      const struct pipe_vertex_buffer *vb = &ctx->vb[i];
      const void *p = NULL;
      size_t size = ~(size_t)0;

      if (!(ctx->vb_mask & 1u << i))
         continue;
      if (vb->is_user_buffer)
         p = vb->buffer.user;
      else if (vb->buffer.resource)
         p = map_buffer(vb->buffer.resource, &size);
      draw_set_mapped_vertex_buffer(draw, i, p, size);
   }
   if (info->index_size) {
      size_t size = ~(size_t)0;

      indices = info->has_user_indices ? info->index.user :
                map_buffer(info->index.resource, &size);
      draw_set_indexes(draw, indices, info->index_size, size);
   }
   for (i = 0; i < 2; i++) {
      const struct pipe_constant_buffer *cb = &ctx->cb[i];
      const uint8_t *p = cb->user_buffer;
      size_t size = cb->buffer_size;

      if (!p && cb->buffer) {
         size_t whole;

         p = map_buffer(cb->buffer, &whole);
         if (p)
            p += cb->buffer_offset;
      }
      if (i == 0)
         draw_set_mapped_constant_buffer(draw, MESA_SHADER_VERTEX, 0, p, p ? size : 0);
      else {
         ctx->fs_constants = (const float *)p;
         ctx->fs_constants_size = p ? size : 0;
      }
   }

   /* the vertex shader on the GPU, where it can be (M18) */
   if (gpu_vs_can_draw(ctx, info)) {
      for (i = 0; i < num_draws; i++)
         if (!gpu_vs_draw(ctx, info, &draws[i]))
            break;
      if (i == num_draws)
         goto done;
      draws += i;
      num_draws -= i;
   }

   update_vertex_info(ctx);
   draw_vbo(draw, info, drawid_offset, NULL, draws, num_draws, 0);
   draw_flush(draw);

done:
   for (i = 0; i < PIPE_MAX_ATTRIBS; i++)
      if (ctx->vb_mask & 1u << i)
         draw_set_mapped_vertex_buffer(draw, i, NULL, 0);
   if (indices)
      draw_set_indexes(draw, NULL, 0, 0);

   submit(ctx);
}
