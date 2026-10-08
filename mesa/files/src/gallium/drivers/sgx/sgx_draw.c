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
 * are drawn into (the screen's size, B8G8R8A8); depth, stencil, blending,
 * scissors and colour masks are not applied yet.  Each draw is a render.
 */
#include "sgx_draw.h"

#include "compiler/glsl_types.h"
#include "compiler/nir/nir.h"
#include "draw/draw_context.h"
#include "draw/draw_vbuf.h"
#include "draw/draw_vertex.h"
#include "tgsi/tgsi_from_mesa.h"
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

   out[0] = in[0] * 2 / ctx->fb.width - 1;
   out[1] = in[1] * 2 / ctx->fb.height - 1;
   out[2] = 0;
   out[3] = 1;
   if (ctx->fs && ctx->fs->compiled) {
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

      if ((t[vf] - t[0]) * (t[2 * vf + 1] - t[1]) -
          (t[2 * vf] - t[0]) * (t[vf + 1] - t[1]) < 0) {
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
   if (ctx->fs && ctx->fs->compiled) {
      /* a compiled fragment shader: its inputs, in its order, all F32 */
      const struct sgx_fs *fs = ctx->fs->compiled;

      for (unsigned i = 0; i < fs->prog.ninputs; i++)
         emit_varying(ctx, vinfo, fs->input_slot[i]);
      draw_compute_vertex_size(vinfo);
      ctx->layout.nvaryings = fs->prog.ninputs;
      ctx->layout.f32 = (1u << fs->prog.ninputs) - 1;
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
   FREE(ctx->verts);
   ctx->verts = NULL;
   ctx->nverts = ctx->maxfloats = 0;
}

/* ---- draw_vbo ----------------------------------------------------------- */

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

/* the triangles gathered, into the bound colour buffer, a render each
 * sgx_frame_max_vertices() */
static void
submit(struct sgx_context *ctx)
{
   struct sgx_screen *screen = sgx_screen(ctx->base.screen);
   struct pipe_surface *surf = &ctx->fb.cbufs[0];
   struct sgx_resource *rt = surf->texture ? sgx_resource(surf->texture) : NULL;
   struct sgx_fs *fs = ctx->fs ? ctx->fs->compiled : NULL;
   uint32_t sa[128 + 4 * SGX_FS_MAX_SAMPLERS], handles[SGX_FS_MAX_SAMPLERS];
   unsigned max, done = 0, nhandles = 0;

   if (!ctx->nverts)
      return;
   if (ctx->debug_draw) {
      mesa_logi("sgx: %u triangles; the first:", ctx->nverts / 3);
      for (unsigned i = 0; i < MIN2(ctx->nverts, 3); i++) {
         const float *v = ctx->verts + i * sgx_frame_vertex_floats(&ctx->layout);
         const float *c = v + 4 + 4 * ctx->layout.colour;

         mesa_logi("sgx:   x %8.4f y %8.4f  colour %.3f %.3f %.3f %.3f",
                   v[0], v[1], c[0], c[1], c[2], c[3]);
      }
   }
   if (!screen->frame) {
      mesa_logw_once("sgx: no template frame (./cascadia gpu installs it): draws are dropped");
      ctx->nverts = 0;
      return;
   }
   if (!rt || ctx->fb.nr_cbufs < 1 || surf->level || surf->first_layer ||
       !sgx_frame_can_render(screen->frame, rt)) {
      mesa_logw_once("sgx: draws only go into the screen-sized B8G8R8A8 targets the "
                     "template frame can fill (M13a); others are dropped");
      ctx->nverts = 0;
      return;
   }
   /* a compiled fragment shader's secondary attributes: its uniforms,
    * then the state words of each texture it samples */
   if (fs) {
      unsigned n = MIN2(fs->nuniforms, ctx->fs_constants_size / 4);

      memset(sa, 0, sizeof(sa));
      if (ctx->fs_constants)
         memcpy(sa, ctx->fs_constants, n * sizeof(float));
      for (unsigned i = 0; i < fs->nsamplers; i++) {
         unsigned unit = fs->sampler_unit[i];
         struct pipe_sampler_view *view = unit < ARRAY_SIZE(ctx->fs_views) ?
                                          ctx->fs_views[unit] : NULL;
         struct sgx_resource *tex = view ? sgx_resource(view->texture) : NULL;

         if (!tex || !sgx_resource_texture(screen, tex, unit < PIPE_MAX_SAMPLERS ?
                                           ctx->fs_samplers[unit] : NULL,
                                           sa + fs->sampler_sa + 4 * i)) {
            mesa_logw_once("sgx: a draw samples a texture unit with nothing it can "
                           "sample bound: dropped");
            ctx->nverts = 0;
            return;
         }
         handles[nhandles++] = tex->tw->handle;
      }
   }
   max = sgx_frame_max_vertices(screen->frame, &ctx->layout);
   while (done < ctx->nverts) {
      unsigned n = MIN2(ctx->nverts - done, max);
      struct sgx_fence *fence = sgx_fence_create(&screen->dev, false);
      int ret;

      if (!fence)
         break;
      simple_mtx_lock(&screen->frame_lock);
      ret = sgx_frame_draw(screen->frame, rt, &ctx->layout,
                           ctx->verts + done * sgx_frame_vertex_floats(&ctx->layout), n,
                           fs ? &fs->prog : NULL, sa, handles, nhandles, fence);
      if (!ret)
         rt->seq++;
      simple_mtx_unlock(&screen->frame_lock);
      if (!ret)
         sgx_fence_reference(&ctx->last, fence);
      else
         mesa_logw("sgx: a draw of %u vertices failed (%d)", n, ret);
      sgx_fence_reference(&fence, NULL);
      done += n;
   }
   ctx->nverts = 0;
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
