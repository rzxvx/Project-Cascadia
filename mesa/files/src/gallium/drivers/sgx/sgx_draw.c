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
#include <time.h>

#include "compiler/glsl_types.h"
#include "compiler/nir/nir.h"
#include "draw/draw_context.h"
#include "draw/draw_vbuf.h"
#include "draw/draw_vertex.h"
#include "tgsi/tgsi_from_mesa.h"
#include "util/format/u_format.h"
#include "util/log.h"
#include "util/os_time.h"
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
#include "sgx_usse.h"

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
   /* the triangles as the draw module wound them: the TA culls nothing
    * (state word 18, M18), and gl_FrontFacing reads the winding */
   emit_vertex(sr, (const float *)(sr->vertices + index * sr->vertex_size),
               ctx->verts + ctx->nverts++ * vf);
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
   /* polygon offset's unit: a 24-bit depth buffer's (times four, the
    * rasterizer state's copy) -- the tiles hold depth as F32 whatever the
    * depth buffer's format (M23), and the draw module's unit for that is
    * a bit of it, too fine to survive */
   draw_set_zs_format(ctx->draw, PIPE_FORMAT_Z24X8_UNORM);
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
   if (ctx->clear_prog.code_va || ctx->colour_prog.code_va) {
      struct sgx_screen *screen = sgx_screen(ctx->base.screen);

      simple_mtx_lock(&screen->frame_lock);
      sgx_frame_retire(screen->frame, &ctx->clear_prog);
      sgx_frame_retire(screen->frame, &ctx->colour_prog);
      simple_mtx_unlock(&screen->frame_lock);
   }
   free(ctx->batch.dead_vs);
   free(ctx->batch.dead_fs);
   free(ctx->batch.verts);
   free(ctx->batch.vdata);
   free(ctx->batch.sa);
   free(ctx->batch.idx);
   ctx->batch.verts = NULL;
   ctx->batch.vdata = NULL;
   ctx->batch.sa = NULL;
   ctx->batch.idx = NULL;
   FREE(ctx->verts);
   free(ctx->vdata);
   free(ctx->indices);
   ctx->verts = NULL;
   ctx->vdata = NULL;
   ctx->indices = NULL;
   ctx->nverts = ctx->maxfloats = ctx->maxvdata = ctx->maxindices = 0;
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
   /* gl_FrontFacing: which winding is the front */
   if (ctx->rast && ctx->rast->front_ccw &&
       BITSET_TEST(sh->nir->info.system_values_read, SYSTEM_VALUE_FRONT_FACE))
      key.front_ccw = 1;
   if (!key.enable && key.colormask == 0xf && !key.tex_swap && !key.tex_x8 && !key.front_ccw)
      return sh->compiled;
   for (unsigned i = 0; i < sh->nvariants; i++)
      if (sh->variant[i]->blend.enable == key.enable &&
          sh->variant[i]->blend.rgb_func == key.rgb_func &&
          sh->variant[i]->blend.rgb_src == key.rgb_src &&
          sh->variant[i]->blend.rgb_dst == key.rgb_dst &&
          sh->variant[i]->blend.alpha_func == key.alpha_func &&
          sh->variant[i]->blend.alpha_src == key.alpha_src &&
          sh->variant[i]->blend.alpha_dst == key.alpha_dst &&
          sh->variant[i]->blend.colormask == key.colormask &&
          sh->variant[i]->blend.tex_swap == key.tex_swap &&
          sh->variant[i]->blend.tex_x8 == key.tex_x8 &&
          sh->variant[i]->blend.front_ccw == key.front_ccw)
         return sh->variant[i];
   if (!(fs = sgx_compile_fs(sh->nir, &key, why, sizeof(why)))) {
      mesa_logw_once("sgx: a variant of a fragment shader (blending, texture orders) "
                     "not compiled (%s): drawn without it", why);
      return sh->compiled;
   }
   if (debug_get_bool_option("SGX_DEBUG_SHADER", false)) {
      mesa_logi("sgx: a fragment shader's variant (blend %u %u/%u/%u %u/%u/%u, mask %x): "
                "%u instructions, %u temps", key.enable, key.rgb_func, key.rgb_src, key.rgb_dst,
                key.alpha_func, key.alpha_src, key.alpha_dst, key.colormask, fs->prog.ncode,
                fs->prog.ntemps);
      for (unsigned i = 0; i < fs->prog.ncode; i++)
         mesa_logi("sgx:   %016llx", (unsigned long long)fs->prog.code[i]);
   }
   if (sh->nvariants == ARRAY_SIZE(sh->variant)) {
      sgx_batch_bury_fs(ctx, sh->variant[0]);
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

   if (!res || !(map = res->data ? res->data : res->bo ? sgx_bo_map(res->bo) : NULL))
      return NULL;
   /* (a buffer's CPU copy: no render writes it, M26) */
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

static bool zs_quad(struct sgx_context *ctx, const struct pipe_scissor_state *sc, float d,
                    const struct sgx_frame_state *st, bool flush);
static bool quad(struct sgx_context *ctx, const struct pipe_scissor_state *sc, float d,
                 const struct sgx_frame_state *st, struct sgx_pixel_program *prog,
                 const uint32_t *sa, unsigned nsa, bool flush);

/* the buried programs: retired from the frame (their places free once the
 * renders submitted so far are done), then freed */
static void
bury(struct sgx_context *ctx)
{
   struct sgx_screen *screen = sgx_screen(ctx->base.screen);
   struct sgx_batch *b = &ctx->batch;

   if (!b->ndead_vs && !b->ndead_fs)
      return;
   simple_mtx_lock(&screen->frame_lock);
   for (unsigned i = 0; screen->frame && i < b->ndead_vs; i++)
      sgx_frame_retire_vs(screen->frame, b->dead_vs[i]);
   for (unsigned i = 0; screen->frame && i < b->ndead_fs; i++)
      sgx_frame_retire(screen->frame, &b->dead_fs[i]->prog);
   simple_mtx_unlock(&screen->frame_lock);
   for (unsigned i = 0; i < b->ndead_vs; i++)
      sgx_vs_destroy(b->dead_vs[i]);
   for (unsigned i = 0; i < b->ndead_fs; i++)
      sgx_fs_destroy(b->dead_fs[i]);
   b->ndead_vs = b->ndead_fs = 0;
}

void
sgx_batch_bury_fs(struct sgx_context *ctx, struct sgx_fs *fs)
{
   struct sgx_batch *b = &ctx->batch;
   struct sgx_fs **d;

   if (!fs)
      return;
   if (!(d = realloc(b->dead_fs, (b->ndead_fs + 1) * sizeof(*d)))) {
      sgx_batch_flush(ctx);
      sgx_fs_destroy(fs);       /* its places stay taken */
      return;
   }
   b->dead_fs = d;
   d[b->ndead_fs++] = fs;
   if (!b->ndraws)
      bury(ctx);
}

void
sgx_batch_bury_vs(struct sgx_context *ctx, struct sgx_vs *vs)
{
   struct sgx_batch *b = &ctx->batch;
   struct sgx_vs **d;

   if (!vs)
      return;
   if (!(d = realloc(b->dead_vs, (b->ndead_vs + 1) * sizeof(*d)))) {
      sgx_batch_flush(ctx);
      sgx_vs_destroy(vs);       /* its places stay taken */
      return;
   }
   b->dead_vs = d;
   d[b->ndead_vs++] = vs;
   if (!b->ndraws)
      bury(ctx);
}

static struct { const char *file; int line; unsigned n; } why[16];

/* SGX_TRACE=1: a thread waits for each render and says how long it took
 * from its kick, and how long since the last one ended (debugging speed) */
#include <pthread.h>
static struct { struct sgx_fence *f; int64_t kick; } trace_q[64];
static unsigned trace_head, trace_tail;
static pthread_mutex_t trace_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t trace_cond = PTHREAD_COND_INITIALIZER;

static void *
trace_thread(void *arg)
{
   int64_t last_done = 0;

   for (;;) {
      struct sgx_fence *f;
      int64_t kick, done;

      pthread_mutex_lock(&trace_lock);
      while (trace_head == trace_tail)
         pthread_cond_wait(&trace_cond, &trace_lock);
      f = trace_q[trace_tail % 64].f;
      kick = trace_q[trace_tail % 64].kick;
      trace_tail++;
      pthread_mutex_unlock(&trace_lock);
      sgx_fence_wait(f, OS_TIMEOUT_INFINITE);
      done = os_time_get_nano();
      sgx_trace_mark("(render done)");
      mesa_logi("sgx: trace: render done %.1f ms after its kick, %.1f ms after the last; "
                "kicked %.1f ms after the last ended", (done - kick) / 1e6,
                last_done ? (done - last_done) / 1e6 : 0, last_done ? (kick - last_done) / 1e6 : 0);
      last_done = done;
      sgx_fence_reference(&f, NULL);
   }
   return NULL;
}

static void
trace_kick(struct sgx_fence *fence)
{
   static int on = -1;
   static pthread_t thread;

   if (on < 0) {
      on = getenv("SGX_TRACE") && atoi(getenv("SGX_TRACE"));
      if (on)
         pthread_create(&thread, NULL, trace_thread, NULL);
   }
   if (!on)
      return;
   pthread_mutex_lock(&trace_lock);
   if (trace_head - trace_tail < 64) {
      trace_q[trace_head % 64].f = NULL;
      sgx_fence_reference(&trace_q[trace_head % 64].f, fence);
      trace_q[trace_head % 64].kick = os_time_get_nano();
      trace_head++;
      pthread_cond_signal(&trace_cond);
   }
   pthread_mutex_unlock(&trace_lock);
}

/* the renders since the last call, by where they were flushed from */
void
sgx_batch_why(struct sgx_context *ctx)
{
   char s[512];
   unsigned n = 0;

   s[0] = 0;
   for (unsigned i = 0; i < ARRAY_SIZE(why) && why[i].file; i++) {
      n += snprintf(s + n, sizeof(s) - MIN2(n, sizeof(s)), " %s:%d %u",
                    strrchr(why[i].file, '/') ? strrchr(why[i].file, '/') + 1 : why[i].file,
                    why[i].line, why[i].n);
      why[i].n = 0;
   }
   mesa_logi("sgx:  renders from%s", s);
}

void
sgx_batch_flush_at(struct sgx_context *ctx, const char *file, int line)
{
   struct sgx_screen *screen = sgx_screen(ctx->base.screen);
   struct sgx_batch *b = &ctx->batch;
   struct sgx_frame_draw *d = b->fdraw;
   struct sgx_resource *zs = b->zs ? sgx_resource(b->zs) : NULL;
   struct sgx_frame_zls zls = { 0 };
   struct sgx_fence *fence;
   int ret = -ENOMEM;

   if (!b->ndraws) {
      bury(ctx);
      return;
   }
   /* the depth buffer's tiles in memory (M24): stored at the render's end
    * when a draw (or a clear's quad) writes depth, loaded at its start when
    * there is something there, no clear came first and a draw reads or
    * writes depth.  SGX_ZLS=0: never (depth lives in the tiles only) */
   if (zs && b->zs->width0 && debug_get_bool_option("SGX_ZLS", true)) {
      bool store, load;

      /* a clear no draw used is only noted: the next render starts from
       * it instead of loading */
      if (zs->zls_cleared && !b->zs_cleared) {
         b->zs_cleared = true;
         b->depth_clear = zs->zls_clear;
      }
      zs->zls_cleared = false;
      store = b->zs_written;
      load = zs->zls_valid && !b->zs_cleared && (store || b->zs_read);
      if (b->zs_cleared && !store) {
         zs->zls_cleared = true;
         zs->zls_clear = b->depth_clear;
      }

      if ((load || store) && !zs->zls) {
         zs->zls = sgx_bo_create(&screen->dev, sgx_frame_zls_size(b->zs->width0,
                                                                  b->zs->height0), 0, 0);
         zs->zls_valid = false;
         load = false;
      }
      if ((load || store) && zs->zls) {
         zls.bo = zs->zls;
         zls.w = b->zs->width0;
         zls.load = load;
         zls.store = store;
         zls.stencil = util_format_has_stencil(util_format_description(b->zs->format));
      }
   }
   /* a render that starts from a clear stores only the tiles it draws in:
    * a quad that draws nothing (depth NEVER) over the target puts every
    * tile in it, all stored at the clear's depth */
   if (zls.store && !zls.load) {
      const struct sgx_frame_state never = { .depth_func = PIPE_FUNC_NEVER };

      zs_quad(ctx, NULL, 0.5f, &never, false);
   }
   for (unsigned i = 0; i < b->ndraws; i++) {
      const struct sgx_batch_draw *bd = &b->draw[i];

      d[i] = (struct sgx_frame_draw){
         .l = bd->l, .nverts = bd->nverts,
         .prog = bd->compiled ? &bd->prog : NULL, .sa = b->sa + bd->sa, .st = bd->st,
         .vs = bd->vs, .vs_sa = b->sa + bd->vs_sa,
         .base = bd->base, .stride = bd->stride, .count = bd->count,
         .indices = bd->nidx ? b->idx + bd->idx : NULL,
      };
      if (!bd->vs)
         d[i].verts = b->verts + bd->first;
      else if (bd->repacked)
         d[i].vdata = b->vdata + bd->first;
   }
   /* the buffers to list: the textures', then the buffers' */
   for (unsigned i = 0; i < b->nbos; i++)
      b->handles[b->ntex + i] = b->bos[i]->handle;
   if ((fence = sgx_fence_create(&screen->dev, false))) {
      simple_mtx_lock(&screen->frame_lock);
      const struct sgx_frame_target t = { sgx_resource(b->rt), b->rt_level, b->rt_layer };

      ret = sgx_frame_render(screen->frame, &t, d, b->ndraws, b->handles,
                             b->ntex + b->nbos, b->depth_clear, zls.bo ? &zls : NULL, fence);
      simple_mtx_unlock(&screen->frame_lock);
   }
   if (!ret && ctx->debug_fps) {
      unsigned i;

      for (i = 0; i < ARRAY_SIZE(why) - 1 && why[i].file &&
                  (why[i].line != line || strcmp(why[i].file, file)); i++)
         ;
      why[i].file = file;
      why[i].line = line;
      why[i].n++;
   }
   /* SGX_DEBUG=fps,sync: each render waited for, its time on the GPU said */
   if (!ret && ctx->debug_sync) {
      int64_t t0 = os_time_get_nano();

      sgx_fence_wait(fence, OS_TIMEOUT_INFINITE);
      ctx->stat_gpu += os_time_get_nano() - t0;
      if (!ctx->debug_fps)
         mesa_logi("sgx: a render of %u draws: %.2f ms on the GPU", b->ndraws,
                   (os_time_get_nano() - t0) / 1e6);
   }
   if (!ret) {
      sgx_trace_mark("kicked");
      trace_kick(fence);
   }
   if (!ret) {
      ctx->stat_renders++;
      sgx_fence_reference(&ctx->last, fence);
      sgx_resource(b->rt)->seq++;
      sgx_resource(b->rt)->gpu_written = true;
      if (zls.store)
         zs->zls_valid = true;
   } else {
      mesa_logw("sgx: a render of %u draws failed (%d)", b->ndraws, ret);
   }
   sgx_fence_reference(&fence, NULL);
   pipe_resource_reference(&b->rt, NULL);
   pipe_resource_reference(&b->zs, NULL);
   b->zs_cleared = b->zs_read = b->zs_written = false;
   for (unsigned i = 0; i < b->ntex; i++)
      pipe_resource_reference(&b->tex[i], NULL);
   /* (the kernel keeps the buffers for the render) */
   for (unsigned i = 0; i < b->nbos; i++) {
      if (!ret)
         sgx_bo_set_busy(b->bos[i], ctx->last);
      sgx_bo_destroy(b->bos[i]);
   }
   b->ndraws = b->ntex = b->nbos = b->nfloats = b->nbytes = b->nsa = b->nidx = 0;
   b->seq++;
   bury(ctx);
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

/* Where a draw's pixels may go: its viewport (a vertex shader's draw; the
 * draw module clips to it itself), the scissor, the target.  The TA clips
 * to a guard band 1.5 times its viewport -- exactly, found drawing past
 * the edges of viewports of all sizes -- so it gets that rectangle shrunk
 * 1.5 times, and the vertices x' = a x + b w (ab: a, b for x, then y) to
 * land where they did through the draw's viewport (t, s).  false when
 * nothing is left. */
static bool
clip_to(struct sgx_context *ctx, bool vs, struct sgx_frame_state *st, float ab[4])
{
   float lo[2] = { 0, 0 }, hi[2] = { ctx->fb.width, ctx->fb.height }, t[2], s[2];

   for (unsigned i = 0; i < 2; i++) {
      if (vs) {
         t[i] = ctx->viewport.translate[i];
         s[i] = ctx->viewport.scale[i];
         lo[i] = MAX2(lo[i], t[i] - fabsf(s[i]));
         hi[i] = MIN2(hi[i], t[i] + fabsf(s[i]));
      } else {
         /* the draw module's vertices: over the target's whole viewport */
         t[i] = s[i] = hi[i] / 2;
      }
   }
   if (ctx->rast && ctx->rast->scissor) {
      lo[0] = MAX2(lo[0], ctx->scissor.minx);
      hi[0] = MIN2(hi[0], ctx->scissor.maxx);
      lo[1] = MAX2(lo[1], ctx->scissor.miny);
      hi[1] = MIN2(hi[1], ctx->scissor.maxy);
   }
   if (!(lo[0] < hi[0] && lo[1] < hi[1]))
      return false;
   for (unsigned i = 0; i < 2; i++) {
      float c = (lo[i] + hi[i]) / 2, h = (hi[i] - lo[i]) / 3;

      st->translate[i] = c;
      st->scale[i] = h;
      ab[2 * i] = s[i] / h;
      ab[2 * i + 1] = (t[i] - c) / h;
   }
   /* depth as the viewport has it; the draw module's z through 0.5 z + 0.5 */
   st->translate[2] = vs ? ctx->viewport.translate[2] : 0.5f;
   st->scale[2] = vs ? ctx->viewport.scale[2] : 0.5f;
   st->viewport = true;
   return true;
}

/* the framebuffer's colour target as the frame takes it: its level and
 * face (M29) */
static bool
fb_target(struct sgx_context *ctx, struct sgx_frame_target *t)
{
   struct pipe_surface *surf = &ctx->fb.cbufs[0];

   if (ctx->fb.nr_cbufs < 1 || !surf->texture)
      return false;
   *t = (struct sgx_frame_target){ sgx_resource(surf->texture), surf->level, surf->first_layer };
   return true;
}

/* whether the gathered draws go elsewhere than t */
static bool
batch_elsewhere(const struct sgx_batch *b, const struct sgx_frame_target *t)
{
   return b->ndraws && (b->rt != &t->res->base || b->rt_level != t->level ||
                        b->rt_layer != t->layer);
}

/* the gathered render's target and depth buffer, at its first draw */
static void
batch_start(struct sgx_context *ctx, const struct sgx_frame_target *t)
{
   struct sgx_batch *b = &ctx->batch;

   pipe_resource_reference(&b->rt, &t->res->base);
   b->rt_level = t->level;
   b->rt_layer = t->layer;
   pipe_resource_reference(&b->zs, ctx->fb.zsbuf.texture);
}

/* the vertices and indices gathered, in bytes (SGX_FRAME_MAX_BYTES) */
static unsigned
batch_bytes(const struct sgx_batch *b)
{
   return b->nfloats * sizeof(float) + b->nbytes + 2 * b->nidx;
}

/* how many more buffers the render would list for the kernel with these
 * textures and buffers' GPU copies (M26) besides its own */
static unsigned
more_handles(struct sgx_context *ctx, struct pipe_resource **tex, unsigned ntex,
             struct sgx_bo **bos, unsigned nbos)
{
   struct sgx_batch *b = &ctx->batch;
   unsigned n = 0;

   for (unsigned i = 0; i < ntex; i++) {
      unsigned k;

      for (k = 0; k < b->ntex && b->tex[k] != tex[i]; k++)
         ;
      n += k == b->ntex;
   }
   for (unsigned i = 0; i < nbos; i++)
      n += bos[i]->batch_ctx != ctx || bos[i]->batch_seq != b->seq;
   return n;
}

/* the textures and buffers' GPU copies listed for the render, referenced
 * (more_handles() said there is room) */
static void
add_handles(struct sgx_context *ctx, struct pipe_resource **tex, unsigned ntex,
            struct sgx_bo **bos, unsigned nbos)
{
   struct sgx_batch *b = &ctx->batch;

   for (unsigned i = 0; i < ntex; i++) {
      unsigned k;

      for (k = 0; k < b->ntex && b->tex[k] != tex[i]; k++)
         ;
      if (k == b->ntex) {
         pipe_resource_reference(&b->tex[b->ntex], tex[i]);
         b->handles[b->ntex++] = sgx_resource(tex[i])->sampled->handle;
      }
   }
   for (unsigned i = 0; i < nbos; i++) {
      if (bos[i]->batch_ctx == ctx && bos[i]->batch_seq == b->seq)
         continue;
      sgx_bo_ref(bos[i]);
      bos[i]->batch_ctx = ctx;
      bos[i]->batch_seq = b->seq;
      b->bos[b->nbos++] = bos[i];
   }
}

/* what a draw does to the depth buffer's tiles, noted for the render */
static void
note_zs(struct sgx_context *ctx, const struct sgx_frame_state *st)
{
   struct sgx_batch *b = &ctx->batch;

   if (!ctx->fb.zsbuf.texture)
      return;
   b->zs_written |= st->depth_write || (st->stencil_on && (st->stencil & 0xff));
   b->zs_read |= (st->depth_func != PIPE_FUNC_ALWAYS && st->depth_func != PIPE_FUNC_NEVER) ||
                 st->stencil_on;
}

/* A vertex shader's draw (ctx->vtx) added to the last one gathered when
 * that has the same state exactly -- the same programs, uniforms (its
 * constant attributes among them), textures, layout and ISP state: SDL
 * draws a tile a draw, a frame's thousand of them alike (M25) -- and its
 * vertices follow on from the last one's: true if it was. */
static bool
merge_vs_draw(struct sgx_context *ctx, struct sgx_vs *vs, const struct sgx_fs *fs,
              const uint32_t *sa, unsigned nsa, const struct sgx_frame_state *st)
{
   struct sgx_screen *screen = sgx_screen(ctx->base.screen);
   struct sgx_batch *b = &ctx->batch;
   struct sgx_vtx *v = &ctx->vtx;
   unsigned nvsa = vs->nuniforms + vs->nubuf, at = 0;
   unsigned nbytes = v->repacked ? v->nverts * v->stride : 0, nidx = v->indexed ? v->count : 0;
   struct sgx_batch_draw *p;
   bool found = false;

   if (!b->ndraws)
      return false;
   p = &b->draw[b->ndraws - 1];
   if (p->vs != vs || p->compiled != (fs != NULL) || p->repacked != v->repacked ||
       p->stride != v->stride ||
       (fs && (p->prog.code_va != fs->prog.code_va || p->prog.pds_va != fs->prog.pds_va ||
               !sgx_words_equal(b->sa + p->sa, sa, nsa))) ||
       p->l.nvaryings != ctx->layout.nvaryings || p->l.f32 != ctx->layout.f32 ||
       p->l.colour != ctx->layout.colour || !sgx_frame_state_equal(&p->st, st) ||
       p->vs_sa + nvsa > b->nsa || !sgx_words_equal(b->sa + p->vs_sa, ctx->vs_sa, nvsa))
      return false;

   /* where its vertex 0 is among the last draw's vertices: right after
    * them (the CPU's stream), or where its attributes' addresses say --
    * the same number of vertices on for each */
   if (v->repacked) {
      if (p->first + p->nverts * p->stride != b->nbytes)
         return false;
      at = p->nverts;
   } else {
      for (unsigned a = 0; a < vs->nattrs; a++) {
         uint32_t d = v->base[a] - p->base[a];

         if (SGX_ATTR_KIND(vs->attr[a]) == SGX_ATTR_CONST)
            continue;
         if (!found) {
            if (!v->stride || d % v->stride)
               return false;
            at = d / v->stride;
            found = true;
         } else if (d != at * v->stride) {
            return false;
         }
      }
   }
   /* and its triangles: a list on from the last draw's, or indices after
    * its indices */
   if (!v->indexed && !p->nidx) {
      if (at != p->count || p->count + v->count > sgx_frame_list_max(screen->frame))
         return false;
   } else if (v->indexed && p->nidx) {
      if (p->idx + p->nidx != b->nidx || at + v->max_index > 0xffff)
         return false;
   } else {
      return false;
   }
   if (batch_bytes(b) + nbytes + 2 * nidx > SGX_FRAME_MAX_BYTES ||
       b->ntex + b->nbos + more_handles(ctx, NULL, 0, v->bos, v->nbos) > SGX_FRAME_MAX_HANDLES ||
       !grow((void **)&b->vdata, &b->maxbytes, b->nbytes + nbytes, 1) ||
       !grow((void **)&b->idx, &b->maxidx, b->nidx + nidx, sizeof(uint16_t)))
      return false;

   add_handles(ctx, NULL, 0, v->bos, v->nbos);
   for (unsigned i = 0; i < nidx; i++)
      b->idx[b->nidx + i] = ctx->indices[i] + at;
   p->nidx += nidx;
   b->nidx += nidx;
   p->max_index = MAX2(p->max_index, at + v->max_index);
   memcpy(b->vdata + b->nbytes, ctx->vdata, nbytes);
   p->nverts += v->repacked ? v->nverts : 0;
   b->nbytes += nbytes;
   p->count += v->count;
   return true;
}

/* A vertex shader's draw (ctx->vtx) into the gathered render: merged into
 * the last draw, or a draw of its own */
static void
add_vs_draw(struct sgx_context *ctx, struct sgx_vs *vs, const struct sgx_fs *fs,
            const uint32_t *sa, unsigned nsa, struct pipe_resource **tex,
            const struct sgx_frame_target *t, const struct sgx_frame_state *st)
{
   struct sgx_screen *screen = sgx_screen(ctx->base.screen);
   struct sgx_batch *b = &ctx->batch;
   struct sgx_vtx *v = &ctx->vtx;
   unsigned nvsa = vs->nuniforms + vs->nubuf, nidx = v->indexed ? v->count : 0;
   unsigned nbytes = v->repacked ? v->nverts * v->stride : 0, ntex = fs ? fs->nsamplers : 0;
   struct sgx_batch_draw *bd;

   if (merge_vs_draw(ctx, vs, fs, sa, nsa, st)) {
      sgx_stat_merged++;
      return;
   }
   /* room in this render: a draw, its vertices, indices, textures and
    * buffers */
   if (b->ndraws == sgx_frame_max_draws(screen->frame) ||
       b->ntex + b->nbos + more_handles(ctx, tex, ntex, v->bos, v->nbos) > SGX_FRAME_MAX_HANDLES ||
       batch_bytes(b) + nbytes + 2 * nidx > SGX_FRAME_MAX_BYTES) {
      sgx_batch_flush(ctx);
      if (nbytes + 2 * nidx > SGX_FRAME_MAX_BYTES ||
          more_handles(ctx, tex, ntex, v->bos, v->nbos) > SGX_FRAME_MAX_HANDLES) {
         mesa_logw_once("sgx: a draw larger than a render takes: dropped");
         return;
      }
   }
   if (!grow((void **)&b->vdata, &b->maxbytes, b->nbytes + nbytes, 1) ||
       !grow((void **)&b->sa, &b->maxsa, b->nsa + nsa + nvsa, sizeof(uint32_t)) ||
       !grow((void **)&b->idx, &b->maxidx, b->nidx + nidx, sizeof(uint16_t)))
      return;
   if (!b->ndraws)
      batch_start(ctx, t);
   /* from now on sampled linear: decided before any draw samples it */
   t->res->gpu_written = true;
   add_handles(ctx, tex, ntex, v->bos, v->nbos);
   note_zs(ctx, st);

   bd = &b->draw[b->ndraws++];
   bd->l = ctx->layout;
   bd->first = b->nbytes;
   bd->nverts = v->repacked ? v->nverts : 0;
   bd->compiled = fs != NULL;
   if (fs)
      bd->prog = fs->prog;
   bd->sa = b->nsa;
   bd->st = *st;
   bd->vs = vs;
   bd->vs_sa = b->nsa + nsa;
   bd->idx = b->nidx;
   bd->nidx = nidx;
   memcpy(bd->base, v->base, sizeof(bd->base));
   bd->stride = v->stride;
   bd->count = v->count;
   bd->max_index = v->max_index;
   bd->repacked = v->repacked;
   memcpy(b->vdata + b->nbytes, ctx->vdata, nbytes);
   memcpy(b->sa + b->nsa, sa, nsa * sizeof(uint32_t));
   memcpy(b->sa + b->nsa + nsa, ctx->vs_sa, nvsa * sizeof(uint32_t));
   memcpy(b->idx + b->nidx, ctx->indices, nidx * sizeof(uint16_t));
   b->nbytes += nbytes;
   b->nsa += nsa + nvsa;
   b->nidx += nidx;
}

/* One draw added to the gathered render: the draw module's triangles
 * (ctx->verts, ctx->nverts), in pieces when they are more than one draw of
 * a render takes -- or, with ctx->gpu_vs, a vertex shader's (ctx->vtx) */
static void
submit(struct sgx_context *ctx)
{
   struct sgx_screen *screen = sgx_screen(ctx->base.screen);
   struct sgx_batch *b = &ctx->batch;
   struct pipe_surface *surf = &ctx->fb.cbufs[0];
   struct sgx_resource *rt = surf->texture ? sgx_resource(surf->texture) : NULL;
   struct sgx_fs *fs = ctx->draw_fs;
   /* (sa's 128 words, then those of the uniforms in memory, M32) */
   uint32_t sa[128 + SGX_UBUF_MAX];
   struct pipe_resource *tex[SGX_FS_MAX_SAMPLERS];
   struct sgx_vs *vs = ctx->gpu_vs;
   unsigned vf = sgx_frame_vertex_floats(&ctx->layout);
   unsigned nsa = fs ? fs->prog.nsa + fs->prog.nubuf : 0;
   unsigned max, done = 0, npass = 1;
   uint32_t face_stencil[2];
   bool two_sided = false;
   struct sgx_frame_state pass[2];
   struct sgx_frame_target target;
   float ab[4];
   struct sgx_frame_state st = {
      .depth_func = ctx->dsa && ctx->dsa->depth_enabled ? ctx->dsa->depth_func : PIPE_FUNC_ALWAYS,
      .depth_write = ctx->dsa && ctx->dsa->depth_enabled && ctx->dsa->depth_writemask,
   };

   /* the stencil test (M23): ISP state B -- compare in 27:25 (as the depth
    * compare, gallium's order), the ops on stencil fail, depth fail and
    * pass in 24:22, 21:19, 18:16, the compare mask in 15:8 and the write
    * mask in 7:0 -- the reference in ISP state A's low byte */
   if (ctx->dsa && ctx->dsa->stencil[0].enabled && ctx->fb.zsbuf.texture &&
       util_format_has_stencil(util_format_description(ctx->fb.zsbuf.texture->format))) {
      /* gallium's ops in the order of the hardware's: keep, zero, replace,
       * incr, decr, invert, incr wrap, decr wrap */
      static const uint8_t op[8] = {
         [PIPE_STENCIL_OP_KEEP] = 0, [PIPE_STENCIL_OP_ZERO] = 1,
         [PIPE_STENCIL_OP_REPLACE] = 2, [PIPE_STENCIL_OP_INCR] = 3,
         [PIPE_STENCIL_OP_DECR] = 4, [PIPE_STENCIL_OP_INVERT] = 5,
         [PIPE_STENCIL_OP_INCR_WRAP] = 6, [PIPE_STENCIL_OP_DECR_WRAP] = 7,
      };

      for (unsigned f = 0; f < 2; f++) {
         const struct pipe_stencil_state *s = &ctx->dsa->stencil[f && ctx->dsa->stencil[1].enabled];

         face_stencil[f] = (uint32_t)(s->func & 7) << 25 | (uint32_t)op[s->fail_op & 7] << 22 |
                           (uint32_t)op[s->zfail_op & 7] << 19 |
                           (uint32_t)op[s->zpass_op & 7] << 16 | (uint32_t)s->valuemask << 8 |
                           s->writemask;
      }
      st.stencil_on = true;
      st.stencil = face_stencil[0];
      st.stencil_ref = ctx->stencil_ref.ref_value[0];
      two_sided = face_stencil[1] != face_stencil[0] ||
                  (ctx->dsa->stencil[1].enabled &&
                   ctx->stencil_ref.ref_value[1] != ctx->stencil_ref.ref_value[0]);
   }

   if (vs ? !ctx->vtx.count : !ctx->nverts)
      return;
   if (!clip_to(ctx, vs, &st, ab)) {
      ctx->nverts = 0;
      return;
   }
   if (vs) {
      const struct pipe_rasterizer_state *r = ctx->rast;

      memcpy(ctx->vs_sa + vs->clip_sa, ab, sizeof(ab));
      /* gallium's anticlockwise is the target's, row 0 at the top: the
       * TA's; the back faces are what the front ones are not */
      if (r->cull_face == PIPE_FACE_BACK)
         st.cull = r->front_ccw ? SGX_CULL_CW : SGX_CULL_CCW;
      else if (r->cull_face == PIPE_FACE_FRONT)
         st.cull = r->front_ccw ? SGX_CULL_CCW : SGX_CULL_CW;
   }
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
   if (!fb_target(ctx, &target) || !sgx_frame_can_render(screen->frame, &target)) {
      mesa_logw_once("sgx: draws only go into the B8G8R8A8 targets the template frame "
                     "can fill (up to 4096 x 4096); others are dropped");
      ctx->nverts = 0;
      return;
   }
   if (batch_elsewhere(b, &target))
      sgx_batch_flush(ctx);

   /* SGX_FRAME=packvertex, packpixel: the pack's sides, a render a draw,
    * M13a's colour (debugging only) */
   if (!vs && (sgx_frame_options() & (SGX_FRAME_PACKVTX | SGX_FRAME_PACKPIX)) &&
       !surf->level && !surf->first_layer) {
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

   /* the draw module's vertices through the clip rectangle's x' = a x + b w */
   if (!vs)
      for (unsigned i = 0; i < ctx->nverts; i++) {
         float *v = ctx->verts + i * vf;

         v[0] = ab[0] * v[0] + ab[1] * v[3];
         v[1] = ab[2] * v[1] + ab[3] * v[3];
      }

   /* a compiled fragment shader's secondary attributes: its uniforms,
    * then the state words of each texture it samples, the blend colour */
   if (fs) {
      unsigned n = MIN2(fs->nuniforms, ctx->fs_constants_size / 4);

      memset(sa, 0, nsa * sizeof(uint32_t));
      if (ctx->fs_constants)
         memcpy(sa, ctx->fs_constants, n * sizeof(float));
      /* the uniforms past sa's, all of them in memory after it (M32) */
      if (fs->prog.nubuf && ctx->fs_constants)
         memcpy(sa + fs->prog.nsa, ctx->fs_constants,
                MIN2(fs->prog.nubuf, ctx->fs_constants_size / 4) * sizeof(float));
      for (unsigned i = 0; i < fs->nsamplers; i++) {
         unsigned unit = fs->sampler_unit[i];
         struct pipe_sampler_view *view = unit < ARRAY_SIZE(ctx->fs_views) ?
                                          ctx->fs_views[unit] : NULL;
         struct sgx_resource *t = view ? sgx_resource(view->texture) : NULL;
         bool swap, x8;

         /* its copy is about to be made again (or it is the target):
          * what was gathered reads the old one, so render that first --
          * one sampled as it is has no copy */
         if (t && sgx_batch_uses(ctx, &t->base) &&
             (&t->base == b->rt ||
              (!sgx_resource_linear(t, &swap, &x8) && (!t->tw || t->tw_seq != t->seq))))
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

   /* two-sided stencil: the front faces with theirs, then the back ones
    * with theirs -- a pass each, the TA culling the other faces (the
    * draw module has culled what the rasterizer state does) */
   pass[0] = st;
   if (two_sided) {
      const struct pipe_rasterizer_state *r = ctx->rast;
      uint8_t back = r->front_ccw ? SGX_CULL_CW : SGX_CULL_CCW;
      uint8_t front = r->front_ccw ? SGX_CULL_CCW : SGX_CULL_CW;

      npass = 0;
      if (r->cull_face != PIPE_FACE_FRONT) {
         pass[npass] = st;
         pass[npass++].cull = back;
      }
      if (r->cull_face != PIPE_FACE_BACK) {
         pass[npass] = st;
         pass[npass].cull = front;
         pass[npass].stencil = face_stencil[1];
         pass[npass++].stencil_ref = ctx->stencil_ref.ref_value[1];
      }
   }

   for (unsigned k = 0; vs && k < npass; k++)
      add_vs_draw(ctx, vs, fs, sa, nsa, tex, &target, &pass[k]);

   max = sgx_frame_max_vertices(screen->frame, &ctx->layout);
   for (unsigned k = 0; !vs && k < npass; k++)
   for (done = 0, st = pass[k]; done < ctx->nverts;) {
      unsigned n = MIN2(ctx->nverts - done, max), ntex = fs ? fs->nsamplers : 0;
      struct sgx_batch_draw *bd;

      /* room in this render: a draw, its vertices, its textures */
      if (b->ndraws == sgx_frame_max_draws(screen->frame) ||
          b->ntex + b->nbos + more_handles(ctx, tex, ntex, NULL, 0) > SGX_FRAME_MAX_HANDLES ||
          batch_bytes(b) + n * vf * sizeof(float) > SGX_FRAME_MAX_BYTES)
         sgx_batch_flush(ctx);
      if (!grow((void **)&b->verts, &b->maxfloats, b->nfloats + n * vf, sizeof(float)) ||
          !grow((void **)&b->sa, &b->maxsa, b->nsa + nsa, sizeof(uint32_t)))
         break;
      if (!b->ndraws)
         batch_start(ctx, &target);
      /* from now on sampled linear: decided before any draw samples it */
      rt->gpu_written = true;
      add_handles(ctx, tex, ntex, NULL, 0);
      note_zs(ctx, &st);
      bd = &b->draw[b->ndraws++];
      memset(bd, 0, sizeof(*bd));
      bd->l = ctx->layout;
      bd->first = b->nfloats;
      bd->nverts = n;
      bd->compiled = fs != NULL;
      if (fs)
         bd->prog = fs->prog;
      bd->sa = b->nsa;
      bd->st = st;
      bd->vs_sa = b->nsa + nsa;
      bd->idx = b->nidx;
      memcpy(b->verts + b->nfloats, ctx->verts + done * vf, n * vf * sizeof(float));
      memcpy(b->sa + b->nsa, sa, nsa * sizeof(uint32_t));
      b->nfloats += n * vf;
      b->nsa += nsa;
      done += n;
   }
   ctx->nverts = 0;
}

/* A quad over the rectangle sc (all of the target: NULL) at the depth d,
 * its pixel program prog with sa[0..nsa) in its secondary attributes, the
 * ISP doing what st says; false when there is no room in the render (and
 * room is not to be made: flush false) */
static bool
quad(struct sgx_context *ctx, const struct pipe_scissor_state *sc, float d,
     const struct sgx_frame_state *st, struct sgx_pixel_program *prog, const uint32_t *sa,
     unsigned nsa, bool flush)
{
   static const struct sgx_frame_layout l = { .nvaryings = 1, .f32 = 1, .colour = 0 };
   struct sgx_screen *screen = sgx_screen(ctx->base.screen);
   struct sgx_batch *b = &ctx->batch;
   struct pipe_surface *surf = &ctx->fb.cbufs[0];
   struct sgx_resource *rt = surf->texture ? sgx_resource(surf->texture) : NULL;
   unsigned vf = sgx_frame_vertex_floats(&l);
   /* the rectangle in clip space, through the target's whole viewport (rows
    * from the first: gallium's framebuffer space), at the depth d */
   float x0 = sc ? 2.0f * sc->minx / ctx->fb.width - 1 : -1;
   float x1 = sc ? 2.0f * sc->maxx / ctx->fb.width - 1 : 1;
   float y0 = sc ? 2.0f * sc->miny / ctx->fb.height - 1 : -1;
   float y1 = sc ? 2.0f * sc->maxy / ctx->fb.height - 1 : 1;
   float z = 2 * d - 1;
   /* x y z w, then the varying nobody reads */
   const float quad[6][8] = {
      { x0, y0, z, 1 }, { x1, y0, z, 1 }, { x1, y1, z, 1 },
      { x0, y0, z, 1 }, { x1, y1, z, 1 }, { x0, y1, z, 1 },
   };
   struct sgx_frame_target target;
   struct sgx_batch_draw *bd;
   int ret;

   if (!rt || !ctx->fb.width || !ctx->fb.height || !fb_target(ctx, &target) ||
       !sgx_frame_can_render(screen->frame, &target))
      return false;
   if (batch_elsewhere(b, &target)) {
      if (!flush)
         return false;
      sgx_batch_flush(ctx);
   }
   simple_mtx_lock(&screen->frame_lock);
   ret = sgx_frame_upload(screen->frame, prog);
   simple_mtx_unlock(&screen->frame_lock);
   if (ret)
      return false;
   if (b->ndraws == sgx_frame_max_draws(screen->frame) ||
       batch_bytes(b) + sizeof(quad) > SGX_FRAME_MAX_BYTES) {
      if (!flush)
         return false;
      sgx_batch_flush(ctx);
   }
   if (!grow((void **)&b->verts, &b->maxfloats, b->nfloats + 6 * vf, sizeof(float)) ||
       !grow((void **)&b->sa, &b->maxsa, b->nsa + nsa, sizeof(uint32_t)))
      return false;
   if (!b->ndraws)
      batch_start(ctx, &target);
   rt->gpu_written = true;
   bd = &b->draw[b->ndraws++];
   memset(bd, 0, sizeof(*bd));
   bd->l = l;
   bd->first = b->nfloats;
   bd->nverts = 6;
   bd->compiled = true;
   bd->prog = *prog;
   bd->sa = b->nsa;
   memcpy(b->sa + b->nsa, sa, nsa * sizeof(uint32_t));
   b->nsa += nsa;
   bd->vs_sa = b->nsa;
   bd->idx = b->nidx;
   bd->st = *st;
   b->zs_written |= st->depth_write || (st->stencil_on && (st->stencil & 0xff));
   memcpy(b->verts + b->nfloats, quad, sizeof(quad));
   b->nfloats += 6 * vf;
   return true;
}

/* a quad whose pixel program writes nothing (o0 keeps the tile's colour) */
static bool
zs_quad(struct sgx_context *ctx, const struct pipe_scissor_state *sc, float d,
        const struct sgx_frame_state *st, bool flush)
{
   static const uint64_t nothing[2] = { 0xfa44070000000000ull, 0xf804014000000000ull };

   if (!ctx->clear_prog.code) {
      ctx->clear_prog.code = (uint64_t *)nothing;
      ctx->clear_prog.ncode = 2;
      ctx->clear_prog.ninputs = 1;
      ctx->clear_prog.nvaryings = 1;
   }
   return quad(ctx, sc, d, st, &ctx->clear_prog, NULL, 0, flush);
}

/* A colour clear (all four channels) in the render, a quad over the
 * rectangle whose program puts the colour, packed, in o0 (`or o0, sa0,
 * #0`) -- not a render of its own, which waited for the last one to be
 * done and ended the gathered draws (M25) */
bool
sgx_colour_clear(struct sgx_context *ctx, const union pipe_color_union *color,
                 const struct pipe_scissor_state *sc)
{
   const struct sgx_frame_state st = { .depth_func = PIPE_FUNC_ALWAYS };
   uint32_t packed = 0;

   if (!ctx->colour_prog.code) {
      ctx->colour_code[0] = USSE_PHAS;
      ctx->colour_code[1] = usse_vbw_or(usse_reg(USSE_OUTPUT, 0), usse_reg(USSE_SA, 0), 0) |
                            USSE_END;
      ctx->colour_prog.code = ctx->colour_code;
      ctx->colour_prog.ncode = 2;
      ctx->colour_prog.ninputs = 1;
      ctx->colour_prog.nvaryings = 1;
      ctx->colour_prog.nsa = 1;
   }
   for (unsigned i = 0; i < 4; i++)
      packed |= (uint32_t)float_to_ubyte(color->f[i]) << 8 * i;
   return quad(ctx, sc, 0.5f, &st, &ctx->colour_prog, &packed, 1, true);
}

/* A render starts at the last depth clear's value and stencil 0 (or from
 * memory, M24) -- no register for another stencil was found.  So a clear
 * inside a render, a scissored one, or one to another stencil, is a quad
 * over the cleared rectangle at the cleared depth: depth ALWAYS (written if
 * cleared), stencil ALWAYS, REPLACE by value through mask (if cleared). */
void
sgx_zs_clear(struct sgx_context *ctx, bool depth, float d, bool stencil, unsigned value,
             unsigned mask, const struct pipe_scissor_state *sc)
{
   const struct sgx_frame_state st = {
      .depth_func = PIPE_FUNC_ALWAYS,
      .depth_write = depth,
      .stencil_on = stencil,
      .stencil = 7u << 25 | 2u << 22 | 2u << 19 | 2u << 16 | 0xffu << 8 | (mask & 0xff),
      .stencil_ref = value,
   };

   zs_quad(ctx, sc, depth ? d : 0.5f, &st, true);
}

/* ---- the vertex shader on the GPU (M18) --------------------------------- */

uint8_t
sgx_attr_of_format(enum pipe_format format)
{
   switch (format) {
   case PIPE_FORMAT_R32_FLOAT:          return SGX_ATTR(SGX_ATTR_F32, 1);
   case PIPE_FORMAT_R32G32_FLOAT:       return SGX_ATTR(SGX_ATTR_F32, 2);
   case PIPE_FORMAT_R32G32B32_FLOAT:    return SGX_ATTR(SGX_ATTR_F32, 3);
   case PIPE_FORMAT_R32G32B32A32_FLOAT: return SGX_ATTR(SGX_ATTR_F32, 4);
   case PIPE_FORMAT_R8_UNORM:           return SGX_ATTR(SGX_ATTR_U8N, 1);
   case PIPE_FORMAT_R8G8_UNORM:         return SGX_ATTR(SGX_ATTR_U8N, 2);
   case PIPE_FORMAT_R8G8B8_UNORM:       return SGX_ATTR(SGX_ATTR_U8N, 3);
   case PIPE_FORMAT_R8G8B8A8_UNORM:     return SGX_ATTR(SGX_ATTR_U8N, 4);
   default:                             return 0;
   }
}

/* The vertex shader for the bound fragment shader's varyings and the
 * attributes' formats (attr): compiled once for each; NULL when it cannot
 * be (the draw module's way then) */
static struct sgx_vs *
vs_for_draw(struct sgx_context *ctx, const struct sgx_fs *fs, const uint8_t *attr)
{
   struct sgx_shader *sh = ctx->vs;
   unsigned slot[SGX_FRAME_MAX_VARYINGS], n = 0;
   union {
      uint8_t b[SGX_VS_MAX_ATTRIBS];
      uint32_t w[SGX_VS_MAX_ATTRIBS / 4];
   } key;
   struct sgx_vs *vs;
   char why[128];

   if (!sh || !sh->nir || sh->vs_failed)
      return NULL;
   for (unsigned i = 0; i < fs->prog.ninputs; i++)
      if (fs->input_slot[i] != VARYING_SLOT_POS)
         slot[n++] = fs->input_slot[i];
   /* (as sgx_compile_vs fills it in) */
   for (unsigned a = 0; a < SGX_VS_MAX_ATTRIBS; a++)
      key.b[a] = a < sh->vs_nattrs ? attr[a] : SGX_ATTR(SGX_ATTR_CONST, 4);
   for (unsigned i = 0; i < sh->nvs; i++)
      if (sh->vs_variant[i]->nvaryings == n &&
          sgx_words_equal(sh->vs_variant[i]->varying_slot, slot, n) &&
          sgx_words_equal(sh->vs_variant[i]->attr_words, key.w, ARRAY_SIZE(key.w)))
         return sh->vs_variant[i];
   if (!(vs = sgx_compile_vs(sh->nir, slot, n, attr, why, sizeof(why)))) {
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
      sgx_batch_bury_vs(ctx, sh->vs_variant[0]);
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
   static int off = -1;

   if (off < 0)
      off = debug_get_bool_option("SGX_CPU_VS", false);
   if (off || !ctx->draw_fs || !r || !ctx->velems || !ctx->vs || !ctx->vs->nir)
      return false;
   if (info->mode != MESA_PRIM_TRIANGLES && info->mode != MESA_PRIM_TRIANGLE_STRIP &&
       info->mode != MESA_PRIM_TRIANGLE_FAN)
      return false;
   if (info->primitive_restart || info->instance_count != 1 ||
       r->cull_face == PIPE_FACE_FRONT_AND_BACK ||
       r->fill_front != PIPE_POLYGON_MODE_FILL || r->fill_back != PIPE_POLYGON_MODE_FILL ||
       r->clip_plane_enable || r->flatshade ||
       /* polygon offset: the draw module's (the TA's not found yet) */
       ((r->offset_tri || r->offset_point || r->offset_line) &&
        (r->offset_units != 0.0f || r->offset_scale != 0.0f)))
      return false;
   return true;
}

static bool
grow_bytes(uint8_t **p, unsigned *max, unsigned want)
{
   uint8_t *q;

   if (want <= *max)
      return true;
   want = MAX2(want, 2 * *max);
   if (!(q = realloc(*p, want)))
      return false;
   *p = q;
   *max = want;
   return true;
}

/* One draw with the vertex shader on the GPU (M18, M26).  Its vertices
 * are fetched from the application's buffers as they are when every
 * attribute the shader reads from memory is in a format the fetch takes
 * (sgx_attr_of_format), in a buffer, 4-byte aligned, and all have one
 * stride (the PDS fetch has one for all, M26) -- else the CPU makes them
 * one stream, in the formats the fetch takes or F32.  Attributes with a
 * stride of 0 are constants, the shader's sa words.  The triangles go as
 * a list of vertices, or indices (strips and fans made lists).  False:
 * not done, the caller's way. */
static bool
gpu_vs_draw(struct sgx_context *ctx, const struct pipe_draw_info *info,
            const struct pipe_draw_start_count_bias *draw)
{
   const struct sgx_vertex_elements *ve = ctx->velems;
   struct sgx_shader *sh = ctx->vs;
   struct sgx_vtx *v = &ctx->vtx;
   struct sgx_screen *screen = sgx_screen(ctx->base.screen);
   const uint8_t *src[SGX_VS_MAX_ATTRIBS];
   struct sgx_resource *res[SGX_VS_MAX_ATTRIBS];
   uint32_t off[SGX_VS_MAX_ATTRIBS];
   uint8_t attr[SGX_VS_MAX_ATTRIBS];
   const void *indices = NULL;
   struct sgx_vs *vs;
   unsigned nattrs = sh->vs_nattrs, stride = 0, ntri, lo = ~0u, hi = 0, vtx0;
   int bias = info->index_size ? draw->index_bias : 0;
   static int repack = -1;
   bool direct;

   /* SGX_REPACK=1: every draw's vertices made one stream by the CPU */
   if (repack < 0)
      repack = debug_get_bool_option("SGX_REPACK", false);
   direct = !repack;

   ntri = info->mode == MESA_PRIM_TRIANGLES ? draw->count / 3 :
          draw->count >= 3 ? draw->count - 2 : 0;
   if (!ntri)
      return true;

   /* each attribute: where vertex 0 of the buffer has it, how it comes */
   for (unsigned a = nattrs; a < SGX_VS_MAX_ATTRIBS; a++) {
      src[a] = NULL;
      attr[a] = SGX_ATTR(SGX_ATTR_CONST, 4);
   }
   for (unsigned a = 0; a < nattrs; a++) {
      const struct pipe_vertex_element *e = a < ve->count ? &ve->e[a] : NULL;
      const struct pipe_vertex_buffer *vb =
         e && (ctx->vb_mask & 1u << e->vertex_buffer_index) ? &ctx->vb[e->vertex_buffer_index] :
                                                               NULL;

      src[a] = NULL;
      res[a] = NULL;
      if (vb && vb->is_user_buffer) {
         src[a] = vb->buffer.user;
      } else if (vb && vb->buffer.resource) {
         res[a] = sgx_resource(vb->buffer.resource);
         src[a] = res[a]->data;
      }
      if (src[a]) {
         off[a] = vb->buffer_offset + e->src_offset;
         src[a] += off[a];
      }
      /* (no instancing here: gpu_vs_can_draw) */
      if (!src[a] || !e->src_stride || e->instance_divisor) {
         attr[a] = SGX_ATTR(SGX_ATTR_CONST, 4);
         continue;
      }
      attr[a] = ve->attr[a];
      if (!attr[a] || !res[a] || !res[a]->data || (off[a] & 3) || (e->src_stride & 3) ||
          (stride && e->src_stride != stride))
         direct = false;
      stride = e->src_stride;
   }

   /* the vertices the draw reads */
   if (info->index_size) {
      size_t size;

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
   /* indices go to the GPU as they are, or less the first vertex the
    * draw reads: 16 bits (a draw of more vertices, or more than the
    * render's room for a stream of them, is the draw module's, which
    * draws it in pieces) */
   if (direct && info->index_size && hi > 0xffff)
      direct = false;
   if (hi - lo > 0xffff)
      return false;
   if (!direct)
      for (unsigned a = 0; a < nattrs; a++)
         if (!attr[a])
            attr[a] = SGX_ATTR(SGX_ATTR_F32, 4);
   if (!(vs = vs_for_draw(ctx, ctx->draw_fs, attr)))
      return false;

   /* vertex 0: the draw's first (the list's start, or index 0 as it is),
    * or the first it reads (the CPU's stream) */
   vtx0 = direct ? (info->index_size ? 0 : draw->start) : lo;

   /* the triangles */
   v->count = 3 * ntri;
   v->max_index = hi - vtx0;
   v->indexed = info->index_size || info->mode != MESA_PRIM_TRIANGLES ||
                v->count > sgx_frame_list_max(screen->frame);
   if (v->indexed) {
      if (v->count > ctx->maxindices) {
         uint16_t *p = realloc(ctx->indices, v->count * sizeof(uint16_t));

         if (!p)
            return false;
         ctx->indices = p;
         ctx->maxindices = v->count;
      }
      for (unsigned t = 0; t < ntri; t++) {
         unsigned k[3], x = 0;

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
               index_at(indices, info->index_size, k[i], &x);
            else
               x = draw->start + k[i];
            ctx->indices[3 * t + i] = x - vtx0;
         }
      }
   }

   /* the attributes: where the fetch finds them */
   v->nbos = 0;
   if (direct) {
      v->repacked = false;
      v->stride = stride;
      for (unsigned a = 0; a < vs->nattrs; a++) {
         struct sgx_bo *bo;
         unsigned i;

         if (SGX_ATTR_KIND(vs->attr[a]) == SGX_ATTR_CONST)
            continue;
         /* (the bytes it reads: vertices lo..hi) */
         if (!(bo = sgx_buffer_bo(ctx, res[a], off[a] + (lo + bias) * stride,
                                  off[a] + (hi + bias) * stride + 4 * sgx_attr_words(vs->attr[a]))))
            return false;
         v->base[a] = bo->va + off[a] + (vtx0 + bias) * stride;
         for (i = 0; i < v->nbos && v->bos[i] != bo; i++)
            ;
         if (i == v->nbos)
            v->bos[v->nbos++] = bo;
      }
   } else {
      unsigned n = hi - lo + 1, at = 0;

      v->repacked = true;
      v->nverts = n;
      for (unsigned a = 0; a < vs->nattrs; a++)
         if (SGX_ATTR_KIND(vs->attr[a]) != SGX_ATTR_CONST) {
            v->base[a] = at;
            at += 4 * sgx_attr_words(vs->attr[a]);
         }
      v->stride = MAX2(at, 4);
      if (n * v->stride > SGX_FRAME_MAX_BYTES ||
          !grow_bytes(&ctx->vdata, &ctx->maxvdata, n * v->stride))
         return false;
      /* the attributes one array of the application's laid out as the
       * stream is -- SDL's x y, r g b a, u v at stride 32 (M34): the
       * vertices in one copy */
      {
         const uint8_t *first = NULL;
         bool whole = true;

         for (unsigned a = 0; a < vs->nattrs && whole; a++) {
            const struct pipe_vertex_element *e = &ve->e[a];

            if (SGX_ATTR_KIND(vs->attr[a]) == SGX_ATTR_CONST)
               continue;
            if (!first)
               first = src[a] - v->base[a];
            whole = vs->attr[a] == ve->attr[a] && e->src_stride == v->stride &&
                    src[a] == first + v->base[a] &&
                    util_format_get_blocksize(e->src_format) == 4 * sgx_attr_words(vs->attr[a]);
         }
         if (whole && first) {
            memcpy(ctx->vdata, first + (lo + bias) * v->stride, n * v->stride);
            goto copied;
         }
      }
      for (unsigned a = 0; a < vs->nattrs; a++) {
         const struct pipe_vertex_element *e = &ve->e[a];
         const uint8_t *in;
         uint8_t *out = ctx->vdata + v->base[a];
         unsigned size;

         if (SGX_ATTR_KIND(vs->attr[a]) == SGX_ATTR_CONST)
            continue;
         in = src[a] + (lo + bias) * e->src_stride;
         if (vs->attr[a] == ve->attr[a]) {
            /* as it is: its bytes (a word for RGB8, the fourth zero) */
            size = util_format_get_blocksize(e->src_format);
            for (unsigned i = 0; i < n; i++, in += e->src_stride, out += v->stride) {
               memset(out, 0, 4 * sgx_attr_words(vs->attr[a]));
               memcpy(out, in, size);
            }
         } else {
            for (unsigned i = 0; i < n; i++, in += e->src_stride, out += v->stride)
               util_format_unpack_rgba(e->src_format, (float *)out, in, 1);
         }
      }
   copied:;
   }

   /* its uniforms: constant buffer 0's words, then the constant
    * attributes' */
   if (vs->nuniforms) {
      const struct pipe_constant_buffer *cb = &ctx->cb[0];
      const uint8_t *p = cb->user_buffer;
      size_t size;

      if (!p && cb->buffer && (p = map_buffer(cb->buffer, &size)))
         p += cb->buffer_offset;
      unsigned n = p ? MIN2(vs->nuniforms * 4, cb->buffer_size) : 0;

      if (p)
         memcpy(ctx->vs_sa, p, n);
      memset((uint8_t *)ctx->vs_sa + n, 0, vs->nuniforms * 4 - n);
      /* those past sa's, all of them in memory after it (M32) */
      if (vs->nubuf) {
         unsigned m = p ? MIN2(vs->nubuf * 4, cb->buffer_size) : 0;

         if (p)
            memcpy(ctx->vs_sa + vs->nuniforms, p, m);
         memset((uint8_t *)(ctx->vs_sa + vs->nuniforms) + m, 0, vs->nubuf * 4 - m);
      }
   }
   for (unsigned a = 0; a < vs->nattrs; a++) {
      float *c = (float *)ctx->vs_sa + vs->attr_sa[a];

      if (SGX_ATTR_KIND(vs->attr[a]) != SGX_ATTR_CONST || vs->attr_sa[a] == 0xff)
         continue;
      if (src[a]) {
         util_format_unpack_rgba(ve->e[a].src_format, c, src[a], 1);
      } else {
         c[0] = c[1] = c[2] = 0;
         c[3] = 1;
      }
   }

   if (ctx->debug_draw) {
      mesa_logi("sgx: a vertex shader's draw: %u vertices%s, %s, stride %u, %u buffers",
                v->count, v->indexed ? " indexed" : "", v->repacked ? "repacked" : "direct",
                v->stride, v->nbos);
      for (unsigned a = 0; a < vs->nattrs; a++)
         mesa_logi("sgx:   attribute %u: %02x at 0x%08x", a, vs->attr[a], v->base[a]);
   }
   ctx->layout.nvaryings = vs->nvaryings;
   ctx->layout.f32 = (1u << vs->nvaryings) - 1;
   ctx->layout.colour = 0;
   ctx->gpu_vs = vs;
   submit(ctx);
   ctx->gpu_vs = NULL;
   v->count = 0;
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
   ctx->stat_draws += num_draws;
   if (ctx->stat_swap) {
      struct timespec ts;

      sgx_trace_mark("first draw");
      clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
      ctx->stat_between += os_time_get_nano() - ctx->stat_swap;
      ctx->stat_between_cpu += ts.tv_sec * 1000000000ll + ts.tv_nsec - ctx->stat_swap_cpu;
      ctx->stat_swap = 0;
   }
   ctx->draw_fs = fs_for_draw(ctx);
   if (ctx->fs && !ctx->fs->compiled && !ctx->fs->colour.ok && !ctx->warned_fs) {
      mesa_logw("sgx: a fragment shader whose colour is not a varying, a constant or a "
                "uniform per channel: drawn grey (M13a)");
      ctx->warned_fs = true;
   }

   /* the fragment shader's constants (the vertex shader's: gpu_vs_draw's,
    * or the draw module's below) */
   {
      const struct pipe_constant_buffer *cb = &ctx->cb[1];
      const uint8_t *p = cb->user_buffer;

      if (!p && cb->buffer) {
         size_t whole;

         p = map_buffer(cb->buffer, &whole);
         if (p)
            p += cb->buffer_offset;
      }
      ctx->fs_constants = (const float *)p;
      ctx->fs_constants_size = p ? cb->buffer_size : 0;
   }

   /* the vertex shader on the GPU, where it can be (M18) */
   if (gpu_vs_can_draw(ctx, info)) {
      for (i = 0; i < num_draws; i++)
         if (!gpu_vs_draw(ctx, info, &draws[i]))
            break;
      if (i == num_draws)
         return;
      draws += i;
      num_draws -= i;
   }

   /* the draw module's way: its buffers mapped for it */
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
   {
      const struct pipe_constant_buffer *cb = &ctx->cb[0];
      const uint8_t *p = cb->user_buffer;

      if (!p && cb->buffer) {
         size_t whole;

         p = map_buffer(cb->buffer, &whole);
         if (p)
            p += cb->buffer_offset;
      }
      draw_set_mapped_constant_buffer(draw, MESA_SHADER_VERTEX, 0, p, p ? cb->buffer_size : 0);
   }

   update_vertex_info(ctx);
   draw_vbo(draw, info, drawid_offset, NULL, draws, num_draws, 0);
   draw_flush(draw);

   for (i = 0; i < PIPE_MAX_ATTRIBS; i++)
      if (ctx->vb_mask & 1u << i)
         draw_set_mapped_vertex_buffer(draw, i, NULL, 0);
   if (indices)
      draw_set_indexes(draw, NULL, 0, 0);

   submit(ctx);
}
