# Copyright 2026 Project Cascadia
# SPDX-License-Identifier: MIT

files_sgx = files(
  'sgx_context.c',
  'sgx_context.h',
  'sgx_device.c',
  'sgx_device.h',
  'sgx_draw.c',
  'sgx_draw.h',
  'sgx_frame.c',
  'sgx_frame.h',
  'sgx_resource.c',
  'sgx_resource.h',
  'sgx_screen.c',
  'sgx_screen.h',
)

libsgx = static_library(
  'sgx',
  files_sgx,
  include_directories : [
    inc_src, inc_include, inc_gallium, inc_gallium_aux, inc_gallium_drivers,
  ],
  dependencies : [dep_libdrm, idep_nir_headers, idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)

driver_sgx = declare_dependency(
  compile_args : '-DGALLIUM_SGX',
  link_with : [libsgx, libsgxwinsys],
  dependencies : idep_nir,
)

if with_tools.contains('drm-shim')
  subdir('drm-shim')
endif
