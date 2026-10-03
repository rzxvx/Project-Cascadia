# Copyright 2026 Project Cascadia
# SPDX-License-Identifier: MIT

libsgx_noop_drm_shim = shared_library(
  'sgx_noop_drm_shim',
  'sgx_noop.c',
  include_directories : [inc_include, inc_src],
  dependencies : [dep_drm_shim, idep_mesautil],
  gnu_symbol_visibility : 'hidden',
  install : true,
)
