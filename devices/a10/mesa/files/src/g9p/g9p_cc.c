/* SPDX-License-Identifier: MIT */
/*
 * g9p-cc: GLSL -> NIR (Mesa's GLSL front end, standalone) -> G9P code, on
 * the build host or the iPad. For checking the compiler kernel by kernel
 * (devices/a10/tools/gpu/isa/cc-check.py runs the results on the GPU).
 *
 *   g9p-cc [-v VERSION] [-n] FILE.comp OUT.bin NAME
 *       -n: print the NIR the backend gets, then the code
 *   OUT.bin: a G9PT pack with NAME (the code) and NAME.info (u32: registers)
 */
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "main/mtypes.h"
#include "compiler/glsl/standalone.h"
#include "compiler/nir/nir.h"
#include "compiler/glsl/gl_nir.h"

#include "g9p_compile.h"

int
main(int argc, char **argv)
{
   struct standalone_options opts = { .glsl_version = 310, .do_link = 1 };
   /* generously sized: the library's gl_context may be larger than this
    * file's idea of it (configuration macros) */
   struct gl_context *ctx = calloc(1, 8 << 20);
   bool print = false;
   int c;

   while ((c = getopt(argc, argv, "v:n")) != -1) {
      if (c == 'v')
         opts.glsl_version = atoi(optarg);
      else if (c == 'n')
         print = true;
   }
   if (argc - optind < 3) {
      fprintf(stderr, "usage: g9p-cc [-v VERSION] [-n] FILE.comp OUT.bin NAME\n");
      return 2;
   }

   struct gl_shader_program *prog = standalone_compile_shader(&opts, 1, &argv[optind], ctx);
   if (!prog || !prog->data->LinkStatus) {
      fprintf(stderr, "g9p-cc: GLSL compile/link failed\n");
      return 1;
   }
   struct gl_linked_shader *sh = prog->_LinkedShaders[MESA_SHADER_COMPUTE];
   if (!sh || !sh->Program->nir) {
      fprintf(stderr, "g9p-cc: no compute shader\n");
      return 1;
   }

   /* buffer blocks -> load_ssbo / store_ssbo with the block index (as the
    * GL state tracker does) */
   gl_nir_lower_buffers(sh->Program->nir, prog);

   /* the linker's block order -> the bindings (the buffers' common-store slots) */
   unsigned binding[32] = { 0 };
   for (unsigned i = 0; i < sh->Program->info.num_ssbos && i < 32; i++)
      binding[i] = sh->Program->sh.ShaderStorageBlocks[i]->Binding;

   struct g9p_shader out;
   const char *err = g9p_compile(sh->Program->nir, print, binding, &out);
   if (err) {
      fprintf(stderr, "g9p-cc: %s\n", err);
      return 1;
   }
   if (g9p_write_pack(argv[optind + 1], argv[optind + 2], &out)) {
      perror(argv[optind + 1]);
      return 1;
   }
   printf("%s: %u bytes of code, %u registers, workgroup %ux%ux%u\n", argv[optind + 2], out.size,
          out.nregs, out.local_size[0], out.local_size[1], out.local_size[2]);
   return 0;
}
