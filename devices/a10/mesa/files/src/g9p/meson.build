# SPDX-License-Identifier: MIT
# The A10's (applegpu_g9p) shader compiler, Cascadia's own
# (devices/a10/docs/gpu.md "G8.2"), for the Gallium driver (g9p) and its
# command-line front end (-Dtools=g9p).

libg9p_compiler = static_library(
  'g9p_compiler',
  files('g9p_compile.c'),
  include_directories : [inc_include, inc_src],
  dependencies : [idep_nir, idep_mesautil],
  gnu_symbol_visibility : 'hidden',
)

if with_tools.contains('g9p')
g9p_cc = executable(
  'g9p-cc',
  files('g9p_cc.c'),
  include_directories : [inc_include, inc_src, inc_mesa, inc_gallium, inc_gallium_aux],
  link_with : [libg9p_compiler, libglsl_standalone],
  dependencies : [idep_nir, idep_mesautil, idep_getopt],
  build_by_default : true,
)
endif
