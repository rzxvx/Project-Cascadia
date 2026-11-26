# SPDX-License-Identifier: MIT
libg9pwinsys = static_library(
  'g9pwinsys',
  files('g9p_drm_winsys.c'),
  include_directories : [
    inc_src, inc_include,
    inc_gallium, inc_gallium_aux, inc_gallium_drivers,
  ],
  dependencies : idep_mesautil,
)
