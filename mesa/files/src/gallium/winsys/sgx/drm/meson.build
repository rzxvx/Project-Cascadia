# Copyright 2026 Project Cascadia
# SPDX-License-Identifier: MIT

libsgxwinsys = static_library(
  'sgxwinsys',
  files('sgx_drm_winsys.c'),
  include_directories : [
    inc_src, inc_include,
    inc_gallium, inc_gallium_aux, inc_gallium_drivers,
  ],
  dependencies : idep_mesautil,
)
