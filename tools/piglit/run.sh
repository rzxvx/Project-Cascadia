#!/bin/sh
# tools/piglit/run.sh NAME [ARGS...] -- on the iPad: piglit's desktop GL
# profile (quick_gl) through the driver, into /root/piglit-results/NAME
# (tools/piglit/build.sh puts piglit in /root/piglit).  What an OpenGL 2.1
# part can never run is left out: GLSL 1.30 and later, OpenGL 3 and 4, and
# the extensions that come with them; the rest runs, and a test the driver
# does not have the extensions for says skip.
#
#   WRAP=/root/sgx-dev run.sh r1            another build (SGX_MESA=dir)
#   run.sh r1 -t 'spec@!opengl 1.1'         only some (piglit's -t, -x)
#   piglit summary console -d /root/piglit-results/r1 ...  (or html)
set -eu
NAME=$1; shift
export LD_LIBRARY_PATH=/root/piglit/lib/piglit/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
# GBM: the surfaceless platform aborts in waffle
export PIGLIT_PLATFORM=gbm
# (a test past 15 minutes is stopped: texture uploads are the CPU's.
# streaming-texture-leak, 5000 textures of 4 MiB, takes 25 minutes alone:
# run it by itself)
exec ${WRAP:-} python3 /root/piglit/bin/piglit run -p gbm --timeout 900 quick_gl \
    "/root/piglit-results/$NAME" \
    -x 'glsl-1\.[3-5]0' -x 'glsl-[34]\.' -x 'glsl-es-3' -x '!opengl [34]\.' \
    -x 'arb_gpu_shader' -x 'arb_tessellation' -x 'arb_compute' -x 'arb_shader_image' \
    -x 'arb_vertex_attrib_64bit' -x 'arb_gl_spirv' -x 'arb_shader_storage' \
    -x 'transform_feedback' -x 'arb_geometry' -x 'arb_bindless' \
    -x 'arb_sparse' -x 'arb_direct_state' -x 'arb_shader_atomic' -x 'arb_indirect' \
    -x 'arb_multi_draw_indirect' -x 'arb_texture_cube_map_array' -x 'arb_texture_buffer' \
    -x 'arb_sample_shading' -x 'arb_texture_multisample' -x 'arb_viewport_array' \
    -x 'arb_shader_subroutine' -x 'arb_shader_precision' -x 'arb_enhanced_layouts' \
    -x 'arb_cull_distance' -x 'arb_separate_shader' -x 'nv_' -x 'amd_' -x 'intel_' \
    -x 'ext_gpu_shader4' -x 'arb_shading_language_packing' -x 'arb_gpu_shader_int64' \
    -x 'arb_program_interface_query' -x 'khr_' -x 'ovr_' -x 'ext_memory_object' \
    -x 'ext_semaphore' -x 'arb_post_depth' -x 'arb_query_buffer' -x 'arb_clip_control' \
    -x 'streaming-texture-leak' \
    "$@"
