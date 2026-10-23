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
   /* rectangle textures' coordinates scaled by load_texture_scale (M35) */
   .has_texture_scaling = true,
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
   case PIPE_TEXTURE_1D:      /* a 2D texture one texel high (lower_1d) */
   case PIPE_TEXTURE_2D:
   case PIPE_TEXTURE_RECT:
   case PIPE_TEXTURE_CUBE:
      break;
   default:
      return false;
   }
   /* (the packed float and shared exponent formats: sampled, as half
    * floats, M43) */
   if (!desc || (desc->layout != UTIL_FORMAT_LAYOUT_PLAIN &&
                 !((usage & ~PIPE_BIND_SAMPLER_VIEW) == 0 &&
                   (format == PIPE_FORMAT_R11G11B10_FLOAT ||
                    format == PIPE_FORMAT_R9G9B9E5_FLOAT)) &&
                 !((usage & ~(PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_RENDER_TARGET)) == 0 &&
                   format == PIPE_FORMAT_R11G11B10_FLOAT)))
      return false;

   /* what the pixel back end writes linear (the framebuffer's format) */
   if (usage & (PIPE_BIND_RENDER_TARGET | PIPE_BIND_DISPLAY_TARGET |
                PIPE_BIND_SCANOUT | PIPE_BIND_SHARED)) {
      /* (M43: R and RG of bytes too -- sgx_frame.c's rt_format; not for
       * the screen) */
      if (format != PIPE_FORMAT_B8G8R8A8_UNORM && format != PIPE_FORMAT_B8G8R8X8_UNORM &&
          ((usage & (PIPE_BIND_DISPLAY_TARGET | PIPE_BIND_SCANOUT | PIPE_BIND_SHARED)) ||
           (format != PIPE_FORMAT_R8_UNORM && format != PIPE_FORMAT_R8G8_UNORM &&
            format != PIPE_FORMAT_I8_UNORM && format != PIPE_FORMAT_L8_UNORM &&
            format != PIPE_FORMAT_L8A8_UNORM && format != PIPE_FORMAT_A8_UNORM &&
            format != PIPE_FORMAT_R16G16B16A16_FLOAT && format != PIPE_FORMAT_R16G16_FLOAT &&
            format != PIPE_FORMAT_R16G16B16X16_FLOAT &&
            format != PIPE_FORMAT_B8G8R8A8_SRGB && format != PIPE_FORMAT_B8G8R8X8_SRGB &&
            format != PIPE_FORMAT_R8G8B8A8_UNORM && format != PIPE_FORMAT_R8G8B8A8_SRGB &&
            format != PIPE_FORMAT_R16_FLOAT && format != PIPE_FORMAT_R32_FLOAT &&
            format != PIPE_FORMAT_R32G32_FLOAT && format != PIPE_FORMAT_R11G11B10_FLOAT)))
         return false;
   }
   if (usage & PIPE_BIND_DEPTH_STENCIL) {
      /* (the tiles hold F32 depth whatever the format: M43's Z32F with or
       * without stencil as the ISP stores them, ARB_depth_buffer_float) */
      if (format != PIPE_FORMAT_Z24_UNORM_S8_UINT && format != PIPE_FORMAT_Z16_UNORM &&
          format != PIPE_FORMAT_Z24X8_UNORM && format != PIPE_FORMAT_Z32_FLOAT &&
          format != PIPE_FORMAT_Z32_FLOAT_S8X24_UINT)
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
      case PIPE_FORMAT_I8_UNORM:      /* (M43: U8, RRRR) */
      /* sRGB (EXT_texture_sRGB, GL 2.1): the bytes as they are, decoded
       * by the program (sgx_blend_key's tex_srgb) */
      case PIPE_FORMAT_R8G8B8A8_SRGB:
      case PIPE_FORMAT_B8G8R8A8_SRGB:
      case PIPE_FORMAT_R8G8B8X8_SRGB:
      case PIPE_FORMAT_B8G8R8X8_SRGB:
      case PIPE_FORMAT_L8_SRGB:
      case PIPE_FORMAT_L8A8_SRGB:
      /* (M43) copied as half floats (sgx_resource_copy_f16): float,
       * signed, 16-bit and the packed formats -- ARB_texture_float,
       * EXT_texture_snorm, EXT_packed_float, EXT_texture_shared_exponent */
      case PIPE_FORMAT_R16_FLOAT: case PIPE_FORMAT_R16G16_FLOAT:
      case PIPE_FORMAT_R16G16B16_FLOAT: case PIPE_FORMAT_R16G16B16A16_FLOAT:
      case PIPE_FORMAT_R16G16B16X16_FLOAT:
      case PIPE_FORMAT_A16_FLOAT: case PIPE_FORMAT_L16_FLOAT: case PIPE_FORMAT_L16A16_FLOAT:
      case PIPE_FORMAT_I16_FLOAT:
      case PIPE_FORMAT_R32_FLOAT: case PIPE_FORMAT_R32G32_FLOAT:
      case PIPE_FORMAT_R32G32B32_FLOAT: case PIPE_FORMAT_R32G32B32A32_FLOAT:
      case PIPE_FORMAT_R32G32B32X32_FLOAT:
      case PIPE_FORMAT_A32_FLOAT: case PIPE_FORMAT_L32_FLOAT: case PIPE_FORMAT_L32A32_FLOAT:
      case PIPE_FORMAT_I32_FLOAT:
      case PIPE_FORMAT_R8_SNORM: case PIPE_FORMAT_R8G8_SNORM: case PIPE_FORMAT_R8G8B8A8_SNORM:
      case PIPE_FORMAT_R8G8B8X8_SNORM:
      case PIPE_FORMAT_A8_SNORM: case PIPE_FORMAT_L8_SNORM: case PIPE_FORMAT_L8A8_SNORM:
      case PIPE_FORMAT_I8_SNORM:
      case PIPE_FORMAT_R16_SNORM: case PIPE_FORMAT_R16G16_SNORM:
      case PIPE_FORMAT_R16G16B16A16_SNORM: case PIPE_FORMAT_R16G16B16X16_SNORM:
      case PIPE_FORMAT_A16_SNORM: case PIPE_FORMAT_L16_SNORM: case PIPE_FORMAT_L16A16_SNORM:
      case PIPE_FORMAT_I16_SNORM:
      case PIPE_FORMAT_R16_UNORM: case PIPE_FORMAT_R16G16_UNORM:
      case PIPE_FORMAT_R16G16B16A16_UNORM: case PIPE_FORMAT_R16G16B16X16_UNORM:
      case PIPE_FORMAT_A16_UNORM: case PIPE_FORMAT_L16_UNORM: case PIPE_FORMAT_L16A16_UNORM:
      case PIPE_FORMAT_I16_UNORM:
      case PIPE_FORMAT_R10G10B10A2_UNORM: case PIPE_FORMAT_B10G10R10A2_UNORM:
      case PIPE_FORMAT_R10G10B10X2_UNORM: case PIPE_FORMAT_B10G10R10X2_UNORM:
      case PIPE_FORMAT_R11G11B10_FLOAT: case PIPE_FORMAT_R9G9B9E5_FLOAT:
      /* (and R and RG of bytes, RGBA8's copy: ARB_texture_rg's sampling) */
      case PIPE_FORMAT_R8_UNORM: case PIPE_FORMAT_R8G8_UNORM:
      /* depth (GLES 2 has OES_depth_texture whatever the driver says): its
       * copy F32 (M37, sgx_resource.c) */
      case PIPE_FORMAT_Z16_UNORM:
      case PIPE_FORMAT_Z24X8_UNORM:
      case PIPE_FORMAT_Z24_UNORM_S8_UINT:
      case PIPE_FORMAT_Z32_FLOAT:
      case PIPE_FORMAT_Z32_FLOAT_S8X24_UINT:
         break;
      default:
         return false;
      }
   }
   if (usage & PIPE_BIND_VERTEX_BUFFER) {
      /* (M43: the packed 10-10-10-2 ones too -- ARB_vertex_type_2_10_10_10_rev
       * -- which the fetch has not: the draw module's translate reads them,
       * the vertex shader on the CPU, gpu_vs_can_draw) */
      if (!desc->is_array && format != PIPE_FORMAT_R32_FLOAT &&
          format != PIPE_FORMAT_R32G32_FLOAT && format != PIPE_FORMAT_R32G32B32_FLOAT &&
          format != PIPE_FORMAT_R32G32B32A32_FLOAT &&
          !(desc->nr_channels == 4 && desc->channel[0].size == 10 &&
            desc->channel[3].size == 2))
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
   /* OpenGL 3's 256 vec4s (1024 components, M43; GLES 2's 128 before),
    * and the 8 clip planes and point size the state tracker keeps room for
    * (it takes them out: M32) -- past sa's words they are in memory */
   caps->max_const_buffer0_size = (256 + 8 + 1) * 4 * sizeof(float);
   /* (M43) uniform buffers 1..15, read from memory (VLDST), through an
    * index too: ARB_uniform_buffer_object */
   caps->max_const_buffers = SGX_FRAME_MAX_UBOS;
   caps->indirect_const_addr = true;
   caps->max_temps = 64;

   caps = (struct pipe_shader_caps *)&pscreen->shader_caps[MESA_SHADER_FRAGMENT];
   caps->max_instructions = caps->max_alu_instructions = 16384;
   caps->max_tex_instructions = caps->max_tex_indirections = 16384;
   caps->max_control_flow_depth = 32;
   caps->max_inputs = 8;
   caps->max_outputs = 1;
   /* 256 vec4s as the vertex shader's (64 before M43), and the alpha
    * test's reference the state tracker keeps room for -- in memory past
    * sa's words, as there */
   caps->max_const_buffer0_size = (256 + 1) * 4 * sizeof(float);
   caps->max_const_buffers = SGX_FRAME_MAX_UBOS;
   caps->indirect_const_addr = true;
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
   /* rectangle textures as 2D ones, their coordinates scaled by the
    * program (sgx_compiler.c, scale_sa) */
   caps->texrect = false;
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
   /* glthread (M34): maps of buffers in the application's thread, kept
    * while draws read them (sgx_resource.c) */
   caps->map_unsynchronized_thread_safe = true;
   caps->allow_mapped_buffers_during_execution = true;
   caps->native_fence_fd = true;
   caps->blend_equation_separate = true;
   caps->uma = true;
   caps->max_render_targets = 1;
   /* attachments of different sizes: a render over their common area, the
    * depth buffer's tiles by its own size (M43: ARB_framebuffer_object) */
   caps->mixed_framebuffer_sizes = true;
   /* a view's swizzle: the program's, after the lookup (M43:
    * EXT_texture_swizzle; sgx_blend_key's tex_swizzle) */
   caps->texture_swizzle = true;
   /* vertex colours as the program wrote them: the varyings go unclamped
    * (F32 or F16), the state tracker clamps where GL says (M43:
    * ARB_color_buffer_float) */
   caps->vertex_color_unclamped = true;
   /* (M43) instanced draws, instanced attributes and primitive restart:
    * the draw module's, the vertex shader on the CPU (gpu_vs_can_draw);
    * conditional rendering: the query waited for (sgx_render_condition_ok) */
   caps->vs_instanceid = true;
   caps->vertex_element_instance_divisor = true;
   caps->primitive_restart = true;
   caps->primitive_restart_fixed_index = true;
   caps->conditional_render = true;
   /* (M43) ARB_depth_clamp: the draw module's, near and far clipping off
    * and the triangles cut at the depth range's ends (render_triangle) */
   caps->depth_clip_disable = true;
   /* (M43) EXT_framebuffer_sRGB: a surface's format says whether the
    * program encodes (sgx_blend_key's rt_srgb) */
   caps->dest_surface_srgb_control = true;
   /* (M43) ARB_blend_func_extended: blending is the program's, the second
    * colour another output nir_lower_blend takes in */
   caps->max_dual_source_render_targets = 1;
   /* (M43) a uniform buffer's words read by VLDST: 64 KiB of them, from any
    * multiple of 16 bytes */
   caps->max_constant_buffer_size = 65536;
   caps->constant_buffer_offset_alignment = 16;
   /* (M43) ARB_timer_query: the GPU's times are its fences' signal times
    * (CLOCK_MONOTONIC, the kernel's), screen->get_timestamp the same clock */
   caps->query_timestamp = true;
   caps->query_time_elapsed = true;
   caps->timer_resolution = 1;
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
   caps->occlusion_query = true;     /* M37: ISP state A bit 14 (sgx_context.c) */
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

static uint64_t
sgx_get_timestamp(struct pipe_screen *pscreen)
{
   return os_time_get_nano();
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
   screen->base.get_timestamp = sgx_get_timestamp;
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
