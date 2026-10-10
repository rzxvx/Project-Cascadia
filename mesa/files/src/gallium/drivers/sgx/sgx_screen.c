/*
 * Copyright 2026 Project Cascadia
 * SPDX-License-Identifier: MIT
 *
 * The screen: what the SGX543MP2 offers to the GL state tracker.  The caps
 * are a GLES 2.0 part's; nothing here draws yet (docs/research/p105-mesa.md,
 * M12-M14) -- clears do, through the template frame.
 */
#include "sgx_screen.h"

#include <stdlib.h>

#include "compiler/nir/nir.h"
#include "pipe/p_defines.h"
#include "util/format/u_format.h"
#include "util/log.h"
#include "util/os_time.h"
#include "util/u_debug.h"
#include "util/u_memory.h"
#include "util/u_screen.h"

#include "sgx_context.h"
#include "sgx_frame.h"
#include "sgx_resource.h"

static const struct nir_shader_compiler_options sgx_nir_options = {
   /* integers are floats (nir_lower_int_to_float): NIR's builtins (atan's
    * copysign) built without integer bit operations */
   .no_integers = true,
   .scalarize_ddx = true,
   .lower_fdiv = true,
   .lower_fmod = true,
   .lower_fpow = true,
   .lower_fsat = false,
   /* the fragment compiler has ffract and builds the rest on it (M13c);
    * ftrunc (int()) is sgx_compiler.c's lower_ftrunc's, after
    * nir_lower_int_to_float has made its own (M31) */
   .lower_ffloor = true,
   .lower_fceil = true,
   .lower_ftrunc = false,
   .lower_fround_even = true,
   .lower_fsign = true,
   .lower_sincos = true,
   .lower_flrp32 = true,
   .lower_ffma32 = true,
   .lower_bitops = true,
   .lower_insert_byte = true,
   .lower_insert_word = true,
   .lower_extract_byte = true,
   .lower_extract_word = true,
   .lower_uniforms_to_ubo = false,
   .force_indirect_unrolling = nir_var_all,
   .force_indirect_unrolling_sampler = true,
   .max_unroll_iterations = 32,
   .support_indirect_inputs = 0,
   .support_indirect_outputs = 0,
};

static void
sgx_screen_destroy(struct pipe_screen *pscreen)
{
   struct sgx_screen *screen = sgx_screen(pscreen);

   sgx_frame_destroy(screen->frame);
   sgx_device_fini(&screen->dev);
   simple_mtx_destroy(&screen->frame_lock);
   slab_destroy_parent(&screen->transfer_pool);
   FREE(screen);
}

static const char *
sgx_get_name(struct pipe_screen *pscreen)
{
   return "PowerVR SGX543MP2";
}

static const char *
sgx_get_vendor(struct pipe_screen *pscreen)
{
   return "Project Cascadia";
}

static const char *
sgx_get_device_vendor(struct pipe_screen *pscreen)
{
   return "Imagination Technologies";
}

static int
sgx_get_screen_fd(struct pipe_screen *pscreen)
{
   return sgx_screen(pscreen)->dev.fd;
}

static bool
sgx_is_format_supported(struct pipe_screen *pscreen, enum pipe_format format,
                        enum pipe_texture_target target, unsigned sample_count,
                        unsigned storage_sample_count, unsigned usage)
{
   const struct util_format_description *desc = util_format_description(format);

   if (MAX2(1, sample_count) != MAX2(1, storage_sample_count) || sample_count > 1)
      return false;
   switch (target) {
   case PIPE_BUFFER:
   case PIPE_TEXTURE_2D:
   case PIPE_TEXTURE_RECT:
   case PIPE_TEXTURE_CUBE:
      break;
   default:
      return false;
   }
   if (!desc || desc->layout != UTIL_FORMAT_LAYOUT_PLAIN)
      return false;

   /* what the pixel back end writes linear (the framebuffer's format) */
   if (usage & (PIPE_BIND_RENDER_TARGET | PIPE_BIND_DISPLAY_TARGET |
                PIPE_BIND_SCANOUT | PIPE_BIND_SHARED)) {
      if (format != PIPE_FORMAT_B8G8R8A8_UNORM && format != PIPE_FORMAT_B8G8R8X8_UNORM)
         return false;
   }
   if (usage & PIPE_BIND_DEPTH_STENCIL) {
      if (format != PIPE_FORMAT_Z24_UNORM_S8_UINT && format != PIPE_FORMAT_Z16_UNORM &&
          format != PIPE_FORMAT_Z24X8_UNORM)
         return false;
   }
   if (usage & PIPE_BIND_SAMPLER_VIEW) {
      switch (format) {
      case PIPE_FORMAT_R8G8B8A8_UNORM:
      case PIPE_FORMAT_B8G8R8A8_UNORM:
      case PIPE_FORMAT_B8G8R8X8_UNORM:
      case PIPE_FORMAT_R8G8B8X8_UNORM:
      case PIPE_FORMAT_B5G6R5_UNORM:
      case PIPE_FORMAT_B4G4R4A4_UNORM:
      case PIPE_FORMAT_B5G5R5A1_UNORM:
      case PIPE_FORMAT_L8_UNORM:
      case PIPE_FORMAT_A8_UNORM:
      case PIPE_FORMAT_L8A8_UNORM:
      /* depth (GLES 2 has OES_depth_texture whatever the driver says): its
       * copy grey, 8 bits of it (sgx_resource.c) */
      case PIPE_FORMAT_Z16_UNORM:
      case PIPE_FORMAT_Z24X8_UNORM:
      case PIPE_FORMAT_Z24_UNORM_S8_UINT:
         break;
      default:
         return false;
      }
   }
   if (usage & PIPE_BIND_VERTEX_BUFFER) {
      if (!desc->is_array && format != PIPE_FORMAT_R32_FLOAT &&
          format != PIPE_FORMAT_R32G32_FLOAT && format != PIPE_FORMAT_R32G32B32_FLOAT &&
          format != PIPE_FORMAT_R32G32B32A32_FLOAT)
         return false;
   }
   if (usage & PIPE_BIND_INDEX_BUFFER) {
      if (format != PIPE_FORMAT_R8_UINT && format != PIPE_FORMAT_R16_UINT &&
          format != PIPE_FORMAT_R32_UINT)
         return false;
   }
   return true;
}

static void
sgx_init_shader_caps(struct pipe_screen *pscreen)
{
   struct pipe_shader_caps *caps;

   caps = (struct pipe_shader_caps *)&pscreen->shader_caps[MESA_SHADER_VERTEX];
   caps->max_instructions = caps->max_alu_instructions = 16384;
   caps->max_control_flow_depth = 32;
   caps->max_inputs = 16;
   caps->max_outputs = 10;     /* position, point size, 8 varyings */
   /* GLES 2's 128 vec4s, and the 8 clip planes and point size the state
    * tracker keeps room for (it took them out of the 128: 119, M32) --
    * past sa's words they are in memory */
   caps->max_const_buffer0_size = (128 + 8 + 1) * 4 * sizeof(float);
   caps->max_const_buffers = 1;
   caps->max_temps = 64;

   caps = (struct pipe_shader_caps *)&pscreen->shader_caps[MESA_SHADER_FRAGMENT];
   caps->max_instructions = caps->max_alu_instructions = 16384;
   caps->max_tex_instructions = caps->max_tex_indirections = 16384;
   caps->max_control_flow_depth = 32;
   caps->max_inputs = 8;
   caps->max_outputs = 1;
   caps->max_const_buffer0_size = 64 * 4 * sizeof(float);
   caps->max_const_buffers = 1;
   caps->max_temps = 64;
   caps->max_texture_samplers = caps->max_sampler_views = 8;
}

static void
sgx_init_screen_caps(struct pipe_screen *pscreen)
{
   struct pipe_caps *caps = (struct pipe_caps *)&pscreen->caps;

   u_init_pipe_screen_caps(pscreen, 1);

   /* GLES 2.0: ARB_texture_non_power_of_two and EXT_blend_equation_separate
    * are what version.c asks for besides the shaders */
   caps->npot_textures = true;
   caps->fragment_shader_derivatives = true;   /* DSX, DSY (M33) */
   caps->generate_mipmap = true;     /* by the CPU: sgx_resource.c */
   /* a buffer's map is its CPU copy, which the GPU's is brought up to date
    * with at a draw, from what unmaps and flushes say was written (M26):
    * nothing tells of a write through a persistent map */
   caps->buffer_map_persistent_coherent = false;
   /* client arrays as they are: the CPU makes their vertices one stream
    * itself (u_vbuf would copy each array into a buffer of its own first,
    * and a draw's arrays would not follow the last one's) */
   caps->user_vertex_buffers = true;
   caps->native_fence_fd = true;
   caps->blend_equation_separate = true;
   caps->uma = true;
   caps->max_render_targets = 1;
   caps->max_texture_2d_size = 4096;      /* iOS's on the A5; SGX_RT_MAX_SIZE */
   caps->max_texture_cube_levels = 12;
   caps->max_texture_3d_levels = 0;
   caps->max_texture_array_layers = 0;
   caps->max_varyings = 8;
   caps->fs_coord_origin_upper_left = true;
   caps->fs_coord_pixel_center_half_integer = true;
   caps->fs_position_is_sysval = true;
   caps->fs_face_is_integer_sysval = true;
   caps->texture_transfer_modes = 0;
   caps->shareable_shaders = false;
   caps->alpha_test = false;
   caps->flatshade = false;
   caps->two_sided_color = false;
   caps->clip_planes = 0;
   caps->point_size_fixed = false;
   caps->vendor_id = 0x1010;   /* Imagination */
   caps->device_id = 0x0543;
   caps->video_memory = 0;
   caps->min_line_width = caps->min_line_width_aa = 1;
   caps->min_point_size = caps->min_point_size_aa = 1;
   caps->max_line_width = caps->max_line_width_aa = 16;
   caps->max_point_size = caps->max_point_size_aa = 512;
   caps->point_size_granularity = caps->line_width_granularity = 0.1f;
   caps->max_texture_lod_bias = 15.0f;
}

struct pipe_screen *
sgx_screen_create(int fd, const struct pipe_screen_config *config, struct renderonly *ro)
{
   struct sgx_screen *screen = CALLOC_STRUCT(sgx_screen);
   const char *pack;

   if (!screen)
      return NULL;
   if (!sgx_device_init(&screen->dev, fd)) {
      FREE(screen);
      return NULL;
   }

   screen->base.destroy = sgx_screen_destroy;
   screen->base.get_name = sgx_get_name;
   screen->base.get_vendor = sgx_get_vendor;
   screen->base.get_device_vendor = sgx_get_device_vendor;
   screen->base.get_screen_fd = sgx_get_screen_fd;
   screen->base.is_format_supported = sgx_is_format_supported;
   screen->base.context_create = sgx_context_create;
   for (unsigned i = 0; i <= MESA_SHADER_COMPUTE; i++)
      screen->base.nir_options[i] = &sgx_nir_options;
   sgx_resource_screen_init(screen);
   sgx_context_screen_init(screen);

   sgx_init_shader_caps(&screen->base);
   sgx_init_screen_caps(&screen->base);
   slab_create_parent(&screen->transfer_pool, sizeof(struct sgx_transfer), 16);

   simple_mtx_init(&screen->frame_lock, mtx_plain);
   pack = getenv("SGX_PACK");
   screen->frame = sgx_frame_create(&screen->dev, pack ? pack : "/usr/local/lib/sgx2d");

   mesa_logi("sgx: SGX543MP%u rev %u.%u.%u, %s", screen->dev.num_cores,
             (screen->dev.core_rev >> 16) & 0xff, (screen->dev.core_rev >> 8) & 0xff,
             screen->dev.core_rev & 0xff,
             screen->frame ? "clears on the GPU" : "clears on the CPU");
   return &screen->base;
}
