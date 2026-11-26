/* SPDX-License-Identifier: MIT */
/*
 * g9p-cc: GLSL -> NIR (Mesa's GLSL front end, standalone) -> G9P code, on
 * the build host or the iPad. For checking the compiler shader by shader
 * (devices/a10/tools/gpu/isa/cc-check.py and render-check.py run the
 * results on the GPU).
 *
 *   g9p-cc [-v VERSION] [-n] FILE.comp OUT.bin NAME
 *   g9p-cc [-v VERSION] [-n] FILE.vert FILE.frag OUT.bin NAME
 *       -n: print the NIR the backend gets
 *       -a N: vertex buffer stride N vec4s (default: the attributes read)
 *       -b MODE: blend in the fragment shader: add, alpha (over) or mul
 *   OUT.bin: a G9PT pack -- compute: NAME (the code) and NAME.info (u32:
 *   registers, local size x, y, z); vertex + fragment: NAME.vs, NAME.fs and
 *   NAME.vsinfo (u32: registers, output dwords, attributes, varyings, then
 *   per varying its components and 1 = perspective-correct), NAME.fsinfo
 *   (u32: registers)
 */
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "main/mtypes.h"
#include "compiler/glsl/standalone.h"
#include "compiler/nir/nir.h"
#include "compiler/glsl/gl_nir.h"

#include "util/blend.h"
#include "util/format/u_formats.h"

#include "g9p_compile.h"

static nir_shader *
stage_nir(struct gl_shader_program *prog, mesa_shader_stage stage)
{
   struct gl_linked_shader *sh = prog->_LinkedShaders[stage];

   return sh && sh->Program->nir ? sh->Program->nir : NULL;
}

int
main(int argc, char **argv)
{
   struct standalone_options opts = { .glsl_version = 310, .do_link = 1 };
   /* generously sized: the library's gl_context may be larger than this
    * file's idea of it (configuration macros) */
   struct gl_context *ctx = calloc(1, 8 << 20);
   bool print = false;
   unsigned nattribs = 0;
   const char *blend = NULL;
   int c;

   while ((c = getopt(argc, argv, "v:na:b:")) != -1) {
      if (c == 'b')
         blend = optarg;
      if (c == 'v')
         opts.glsl_version = atoi(optarg);
      else if (c == 'a')
         nattribs = atoi(optarg);
      else if (c == 'n')
         print = true;
   }
   const int nfiles = argc - optind - 2;
   if (nfiles != 1 && nfiles != 2) {
      fprintf(stderr, "usage: g9p-cc [-v VERSION] [-n] FILE.comp | FILE.vert FILE.frag  OUT.bin NAME\n");
      return 2;
   }
   const char *outp = argv[optind + nfiles], *name = argv[optind + nfiles + 1];

   struct gl_shader_program *prog = standalone_compile_shader(&opts, nfiles, &argv[optind], ctx);
   if (!prog || !prog->data->LinkStatus) {
      fprintf(stderr, "g9p-cc: GLSL compile/link failed\n");
      return 1;
   }

   if (nfiles == 1) {
      struct gl_linked_shader *sh = prog->_LinkedShaders[MESA_SHADER_COMPUTE];
      if (!sh || !sh->Program->nir) {
         fprintf(stderr, "g9p-cc: no compute shader\n");
         return 1;
      }
      /* buffer blocks -> load_ssbo / store_ssbo with the block index (as
       * the GL state tracker does) */
      gl_nir_lower_buffers(sh->Program->nir, prog);

      /* the linker's block order -> the bindings (the buffers' common-store slots) */
      unsigned binding[32] = { 0 };
      for (unsigned i = 0; i < sh->Program->info.num_ssbos && i < 32; i++)
         binding[i] = sh->Program->sh.ShaderStorageBlocks[i]->Binding;

      struct g9p_options o = { .ssbo_binding = binding };
      struct g9p_shader out;
      const char *err = g9p_compile(sh->Program->nir, print, &o, &out);
      if (err) {
         fprintf(stderr, "g9p-cc: %s\n", err);
         return 1;
      }
      if (g9p_write_pack(outp, name, &out)) {
         perror(outp);
         return 1;
      }
      printf("%s: %u bytes of code, %u registers, workgroup %ux%ux%u\n", name, out.size, out.nregs,
             out.local_size[0], out.local_size[1], out.local_size[2]);
      return 0;
   }

   nir_shader *vsn = stage_nir(prog, MESA_SHADER_VERTEX), *fsn = stage_nir(prog, MESA_SHADER_FRAGMENT);
   if (!vsn || !fsn) {
      fprintf(stderr, "g9p-cc: a vertex and a fragment shader, please\n");
      return 1;
   }
   /* sampler uniforms -> texture / sampler indices */
   gl_nir_lower_samplers(vsn, prog);
   gl_nir_lower_samplers(fsn, prog);
   struct g9p_varyings vary;
   struct g9p_options o = { .varyings = &vary, .nattribs = nattribs };
   /* blend modes for the tests: add, alpha (over), mul, onto BGRA8 */
   struct g9p_blend bl = { PIPE_BLEND_ADD, PIPE_BLENDFACTOR_ONE, PIPE_BLENDFACTOR_ONE,
                           PIPE_BLEND_ADD, PIPE_BLENDFACTOR_ONE, PIPE_BLENDFACTOR_ONE,
                           PIPE_FORMAT_B8G8R8A8_UNORM };
   if (blend) {
      if (!strcmp(blend, "alpha")) {
         bl.rgb_src = PIPE_BLENDFACTOR_SRC_ALPHA;
         bl.rgb_dst = PIPE_BLENDFACTOR_INV_SRC_ALPHA;
         bl.alpha_dst = PIPE_BLENDFACTOR_INV_SRC_ALPHA;
      } else if (!strcmp(blend, "mul")) {
         bl.rgb_src = PIPE_BLENDFACTOR_DST_COLOR;
         bl.rgb_dst = PIPE_BLENDFACTOR_ZERO;
         bl.alpha_src = PIPE_BLENDFACTOR_DST_ALPHA;
         bl.alpha_dst = PIPE_BLENDFACTOR_ZERO;
      } else if (strcmp(blend, "add")) {
         fprintf(stderr, "g9p-cc: blend add, alpha or mul\n");
         return 2;
      }
      o.blend = &bl;
   }
   struct g9p_shader vs, fs;
   const char *err = g9p_compile(vsn, print, &o, &vs);
   if (!err)
      err = g9p_compile(fsn, print, &o, &fs);
   if (err) {
      fprintf(stderr, "g9p-cc: %s\n", err);
      return 1;
   }

   uint32_t vsinfo[4 + 2 * G9P_MAX_VARYINGS] = { vs.nregs, vs.noutputs, vs.nattribs, vary.n };
   for (unsigned i = 0; i < vary.n; i++) {
      vsinfo[4 + 2 * i] = vary.comps[i];
      vsinfo[5 + 2 * i] = vary.persp[i];
   }
   uint32_t fsinfo[1] = { fs.nregs };
   char n0[64], n1[64], n2[64], n3[64];
   snprintf(n0, sizeof(n0), "%s.vs", name);
   snprintf(n1, sizeof(n1), "%s.fs", name);
   snprintf(n2, sizeof(n2), "%s.vsinfo", name);
   snprintf(n3, sizeof(n3), "%s.fsinfo", name);
   const char *names[4] = { n0, n1, n2, n3 };
   const void *data[4] = { vs.code, fs.code, vsinfo, fsinfo };
   const unsigned sizes[4] = { vs.size, fs.size, 4 * (4 + 2 * vary.n), sizeof(fsinfo) };
   if (g9p_write_entries(outp, 4, names, data, sizes)) {
      perror(outp);
      return 1;
   }
   printf("%s: vertex %u bytes, %u registers, %u outputs, %u attributes; fragment %u bytes, %u registers\n",
          name, vs.size, vs.nregs, vs.noutputs, vs.nattribs, fs.size, fs.nregs);
   return 0;
}
