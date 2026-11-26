# SPDX-License-Identifier: MIT
# The iPad 7's (Apple A10, applegpu_g9p) Gallium driver, Cascadia's own
# (devices/a10/docs/gpu.md "G9"); its shaders by src/g9p's compiler.

files_g9p = files(
  'g9p_context.c',
  'g9p_context.h',
  'g9p_device.c',
  'g9p_device.h',
  'g9p_public.h',
  'g9p_resource.c',
  'g9p_resource.h',
  'g9p_screen.c',
  'g9p_screen.h',
)

libg9p = static_library(
  'g9p',
  files_g9p,
  include_directories : [
    inc_src, inc_include, inc_gallium, inc_gallium_aux, inc_gallium_drivers,
  ],
  dependencies : [dep_libdrm, idep_nir_headers, idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)

driver_g9p = declare_dependency(
  compile_args : '-DGALLIUM_G9P',
  link_with : [libg9p, libg9pwinsys, libg9p_compiler],
  dependencies : idep_nir,
)
