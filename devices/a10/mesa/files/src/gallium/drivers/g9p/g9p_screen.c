/* SPDX-License-Identifier: MIT */
/*
 * The screen: what the iPad 7's GPU offers the state tracker so far
 * (devices/a10/docs/gpu.md "G9"): OpenGL ES 2.0 / OpenGL 2.1 shaders through
 * Cascadia's NIR -> G9P compiler, BGRA8 render targets, renders waited for.
 */
#include "g9p_screen.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>

#include "compiler/nir/nir.h"
#include "pipe/p_defines.h"
#include "util/format/u_format.h"
#include "util/log.h"
#include "util/os_time.h"
#include "util/u_memory.h"
#include "util/u_screen.h"

#include "g9p_context.h"
#include "g9p_public.h"
#include "g9p_resource.h"

static const struct nir_shader_compiler_options g9p_nir_options = {
   /* what the backend (src/g9p/g9p_compile.c) does not take: built from
    * what it does */
   .lower_fdiv = false,
   .lower_fpow = false,
   .lower_fmod = true,
   .lower_fsign = true,
   .lower_fceil = true,
   .lower_ftrunc = true,
   .lower_fround_even = true,
   .lower_sincos = true,
   .lower_isign = true,
   .lower_iabs = true,
   .lower_bitops = true,
   .lower_bitfield_extract = true,
   .lower_bitfield_insert = true,
   .lower_bitfield_reverse = true,
   .lower_bit_count = true,
   .lower_ifind_msb = true,
   .lower_ufind_msb = true,
   .lower_find_lsb = true,
   .lower_uadd_carry = true,
   .lower_usub_borrow = true,
   .lower_mul_high = true,
   .lower_hadd = true,
   .lower_uadd_sat = true,
   .lower_iadd_sat = true,
   .lower_scmp = true,
   .lower_extract_byte = true,
   .lower_extract_word = true,
   .lower_insert_byte = true,
   .lower_insert_word = true,
   .lower_pack_half_2x16 = true,
   .lower_pack_unorm_2x16 = true,
   .lower_pack_snorm_2x16 = true,
   .lower_pack_unorm_4x8 = true,
   .lower_pack_snorm_4x8 = true,
   .lower_unpack_half_2x16 = true,
   .lower_unpack_unorm_2x16 = true,
   .lower_unpack_snorm_2x16 = true,
   .lower_unpack_unorm_4x8 = true,
   .lower_unpack_snorm_4x8 = true,
   .lower_flrp32 = false,
   .max_unroll_iterations = 32,
   .force_indirect_unrolling = nir_var_all,
   .force_indirect_unrolling_sampler = true,
   /* inputs and outputs as intrinsics by location (g9p_compile reads
    * io semantics) */
   .io_options = nir_io_has_intrinsics,
   /* uniforms: constant buffer 0, its words in the common store */
   .lower_uniforms_to_ubo = true,
};

static void
g9p_screen_destroy(struct pipe_screen *pscreen)
{
   struct g9p_screen *screen = g9p_screen(pscreen);

   g9p_device_fini(&screen->dev);
   slab_destroy_parent(&screen->transfer_pool);
   FREE(screen);
}

static const char *g9p_get_name(struct pipe_screen *p) { return "Apple A10 GPU (G9P)"; }
static const char *g9p_get_vendor(struct pipe_screen *p) { return "Project Cascadia"; }
static const char *g9p_get_device_vendor(struct pipe_screen *p) { return "Apple"; }
static int g9p_get_screen_fd(struct pipe_screen *p) { return g9p_screen(p)->dev.fd; }
static uint64_t g9p_get_timestamp(struct pipe_screen *p) { return os_time_get_nano(); }

bool
g9p_rt_format(enum pipe_format format)
{
   return format == PIPE_FORMAT_B8G8R8A8_UNORM || format == PIPE_FORMAT_B8G8R8X8_UNORM;
}

static bool
g9p_is_format_supported(struct pipe_screen *pscreen, enum pipe_format format,
                        enum pipe_texture_target target, unsigned sample_count,
                        unsigned storage_sample_count, unsigned usage)
{
   const struct util_format_description *desc = util_format_description(format);

   if (MAX2(1, sample_count) != MAX2(1, storage_sample_count) || sample_count > 1)
      return false;
   if (target != PIPE_BUFFER && target != PIPE_TEXTURE_2D)
      return false;
   if (!desc || desc->layout != UTIL_FORMAT_LAYOUT_PLAIN)
      return false;
   if ((usage & (PIPE_BIND_RENDER_TARGET | PIPE_BIND_DISPLAY_TARGET | PIPE_BIND_SCANOUT |
                 PIPE_BIND_SHARED)) && !g9p_rt_format(format))
      return false;
   /* depth and stencil live in the tiles for one render (to do: their
    * load / store between renders); the buffer itself is not written */
   if ((usage & PIPE_BIND_DEPTH_STENCIL) && format != PIPE_FORMAT_Z16_UNORM &&
       format != PIPE_FORMAT_Z24X8_UNORM && format != PIPE_FORMAT_Z24_UNORM_S8_UINT &&
       format != PIPE_FORMAT_S8_UINT && format != PIPE_FORMAT_Z32_FLOAT &&
       format != PIPE_FORMAT_Z32_FLOAT_S8X24_UINT)
      return false;
   if (util_format_is_depth_or_stencil(format) && (usage & ~PIPE_BIND_DEPTH_STENCIL & ~PIPE_BIND_SHARED))
      return false;
   if ((usage & PIPE_BIND_SAMPLER_VIEW) && format != PIPE_FORMAT_R8G8B8A8_UNORM &&
       format != PIPE_FORMAT_B8G8R8A8_UNORM && format != PIPE_FORMAT_B8G8R8X8_UNORM &&
       format != PIPE_FORMAT_R8G8B8X8_UNORM)
      return false;
   if ((usage & PIPE_BIND_VERTEX_BUFFER) && format != PIPE_FORMAT_R32_FLOAT &&
       format != PIPE_FORMAT_R32G32_FLOAT && format != PIPE_FORMAT_R32G32B32_FLOAT &&
       format != PIPE_FORMAT_R32G32B32A32_FLOAT)
      return false;
   if ((usage & PIPE_BIND_INDEX_BUFFER) && format != PIPE_FORMAT_R8_UINT &&
       format != PIPE_FORMAT_R16_UINT && format != PIPE_FORMAT_R32_UINT)
      return false;
   return true;
}

static void
g9p_init_shader_caps(struct pipe_screen *pscreen)
{
   for (unsigned s = MESA_SHADER_VERTEX; s <= MESA_SHADER_FRAGMENT; s += MESA_SHADER_FRAGMENT) {
      struct pipe_shader_caps *caps = (struct pipe_shader_caps *)&pscreen->shader_caps[s];

      caps->max_instructions = caps->max_alu_instructions = 16384;
      caps->max_control_flow_depth = 32;
      caps->max_inputs = 16;
      caps->max_outputs = s == MESA_SHADER_VERTEX ? 16 : 1;
      /* in the common store: 128 vec4s for vertex shaders, 64 for
       * fragment shaders (GLES 2's minimums: 128 / 16) */
      caps->max_const_buffer0_size = (s == MESA_SHADER_VERTEX ? 128 : 64) * 4 * sizeof(float);
      caps->max_const_buffers = 1;
      caps->max_temps = 64;
      caps->integers = true;
      if (s == MESA_SHADER_FRAGMENT) {
         caps->max_tex_instructions = caps->max_tex_indirections = 16384;
         caps->max_texture_samplers = caps->max_sampler_views = 8;
      }
   }
}

static void
g9p_init_screen_caps(struct pipe_screen *pscreen)
{
   struct pipe_caps *caps = (struct pipe_caps *)&pscreen->caps;

   u_init_pipe_screen_caps(pscreen, 1);
   caps->npot_textures = true;
   caps->generate_mipmap = true;      /* by the CPU: g9p_resource.c */
   caps->blend_equation_separate = true;
   caps->uma = true;
   caps->max_render_targets = 1;
   caps->max_texture_2d_size = 4096;
   caps->max_texture_cube_levels = 0;
   caps->max_texture_3d_levels = 0;
   caps->max_texture_array_layers = 0;
   caps->max_varyings = 16;
   caps->fs_coord_origin_upper_left = true;
   caps->fs_coord_pixel_center_half_integer = true;
   caps->texture_transfer_modes = 0;
   caps->shareable_shaders = false;
   caps->alpha_test = false;
   caps->flatshade = false;
   caps->two_sided_color = false;
   caps->clip_planes = 0;
   caps->point_size_fixed = false;
   caps->user_vertex_buffers = false;
   /* (attributes loaded as dwords: u_vbuf copies the others) */
   caps->vertex_input_alignment = PIPE_VERTEX_INPUT_ALIGNMENT_4BYTE;
   caps->vendor_id = 0x106b;   /* Apple */
   caps->device_id = 0x8010;   /* T8010 */
   caps->video_memory = 0;
   caps->min_line_width = caps->min_line_width_aa = 1;
   caps->min_point_size = caps->min_point_size_aa = 1;
   /* ISPA's point / line field: 4.4 fixed point less one, up to 255 */
   caps->max_line_width = caps->max_line_width_aa = 16;
   caps->max_point_size = caps->max_point_size_aa = 64;
   caps->point_size_granularity = caps->line_width_granularity = 0.1f;
   caps->max_texture_lod_bias = 15.0f;
   caps->max_constant_buffer_size = 65536;
   caps->constant_buffer_offset_alignment = 16;
   caps->glsl_feature_level = caps->glsl_feature_level_compatibility = 120;
}

struct pipe_screen *
g9p_create_sw_screen(struct sw_winsys *ws)
{
   /* the first render node of the powervr driver */
   for (unsigned i = 128; i < 136; i++) {
      char path[32];
      int fd;

      snprintf(path, sizeof(path), "/dev/dri/renderD%u", i);
      if ((fd = open(path, O_RDWR | O_CLOEXEC)) < 0)
         continue;
      drmVersionPtr v = drmGetVersion(fd);
      const bool ours = v && !strcmp(v->name, "powervr");
      drmFreeVersion(v);
      if (ours) {
         struct pipe_screen *s = g9p_screen_create(fd, NULL, NULL);
         if (s) {
            g9p_screen(s)->sw = ws;
            return s;
         }
      }
      close(fd);
   }
   return NULL;
}

struct pipe_screen *
g9p_screen_create(int fd, const struct pipe_screen_config *config, struct renderonly *ro)
{
   struct g9p_screen *screen = CALLOC_STRUCT(g9p_screen);

   if (!screen)
      return NULL;
   if (!g9p_device_init(&screen->dev, fd)) {
      mesa_loge("g9p: the GPU's render node did not take our setup");
      FREE(screen);
      return NULL;
   }
   screen->base.destroy = g9p_screen_destroy;
   screen->base.get_name = g9p_get_name;
   screen->base.get_vendor = g9p_get_vendor;
   screen->base.get_device_vendor = g9p_get_device_vendor;
   screen->base.get_screen_fd = g9p_get_screen_fd;
   screen->base.get_timestamp = g9p_get_timestamp;
   screen->base.is_format_supported = g9p_is_format_supported;
   screen->base.context_create = g9p_context_create;
   for (unsigned i = 0; i <= MESA_SHADER_COMPUTE; i++)
      screen->base.nir_options[i] = &g9p_nir_options;
   g9p_resource_screen_init(&screen->base);
   g9p_context_screen_init(&screen->base);
   g9p_init_shader_caps(&screen->base);
   g9p_init_screen_caps(&screen->base);
   slab_create_parent(&screen->transfer_pool, sizeof(struct pipe_transfer), 16);
   mesa_logi("g9p: Apple A10 GPU, Cascadia's driver");
   return &screen->base;
}
