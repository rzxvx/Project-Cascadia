/* SPDX-License-Identifier: MIT */
/*
 * NIR -> G9P (applegpu_g9p, the A10's USC), compute shaders first.
 *
 * The encodings are the ones checked on the GPU in devices/a10/docs/gpu.md
 * "G8.1" (tools/gpu/isa/g9pasm.py: every encoder below has its twin and a
 * test there).
 *
 * The compute ABI (what the driver -- or g9p-probe in the tests -- provides):
 *  - r0 = the local invocation index (the kernel PDS program's instance
 *    DOUTW), workgroup sizes up to 512 in x only for now;
 *  - threadgroup-memory word 0 = the workgroup ID (its shared-phase DOUTW);
 *  - common store: dwords 2b, 2b + 1 = the address of the buffer at
 *    binding b; dword G9P_CS_THREADS = the threads per group (the lane test
 *    in the prologue reads it).
 */
#include "g9p_compile.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"

#define G9P_CS_THREADS 30
#define G9P_MAX_REGS   31          /* bank 1 r0-r30, the mask stack after them */

/* ---- encoders (twins of g9pasm.py's) -------------------------------------- */

struct emit {
   uint16_t *w;
   unsigned n, cap;
   const char *error;
};

static void
put(struct emit *e, const uint16_t *w, unsigned n)
{
   if (e->n + n > e->cap) {
      e->cap = (e->n + n) * 2 + 64;
      e->w = realloc(e->w, e->cap * sizeof(*e->w));
   }
   memcpy(e->w + e->n, w, n * sizeof(*w));
   e->n += n;
}

#define EMIT(e, ...) do { const uint16_t w_[] = { __VA_ARGS__ }; put(e, w_, sizeof(w_) / 2); } while (0)

#define BANKS 0x3000                /* W0: both sources from bank 1 */
#define NEG_B 0x4000

static uint16_t dst(unsigned d) { return 0x0300 | d << 1; }
static uint16_t srcs(unsigned a, unsigned b) { return 0x0100 | a << 2 | b << 10; }

static void
alu2(struct emit *e, uint16_t op, unsigned d, unsigned a, unsigned b)
{
   EMIT(e, BANKS | 0x0199, dst(d), srcs(a, b), op);
}

static void
ffma(struct emit *e, unsigned d, unsigned a, unsigned b, unsigned c)
{
   EMIT(e, BANKS | 0x0800 | 0x019a, dst(d), srcs(a, b), 0x0100 | c << 2, 0x004c);
}

static void
funary(struct emit *e, uint16_t op, unsigned d, unsigned a)
{
   EMIT(e, 0x219c, dst(d), 0x0100 | a << 2, op);
}

static void
convert(struct emit *e, uint16_t op, unsigned d, unsigned a)
{
   EMIT(e, 0x1199, dst(d), 0x0101 | op | a << 10, 0x0120, (op & 4) ? 0x0468 : 0x0460);
}

static void
shift(struct emit *e, unsigned d, unsigned a, unsigned b, bool right)
{
   EMIT(e, 0x199e, dst(d), 0x0101 | a << 10, 0x0120, 0x0100 | b << 2, right ? 0x04f4 : 0x04f0);
}

static void
mov_imm(struct emit *e, unsigned d, uint32_t v)
{
   static const uint8_t map[32][2] = {
      {2, 1}, {2, 2}, {2, 3}, {2, 4}, {2, 5}, {2, 6}, {3, 1}, {3, 2}, {3, 3}, {0, 13}, {3, 4},
      {3, 12}, {3, 13}, {3, 14}, {3, 6}, {3, 7}, {4, 0}, {4, 1}, {4, 2}, {4, 3}, {4, 4}, {4, 5},
      {4, 6}, {4, 7}, {3, 15}, {4, 9}, {4, 10}, {4, 11}, {4, 12}, {4, 13}, {4, 14}, {4, 15},
   };
   const bool wide = v >> 16;
   uint16_t w[7] = { 0x0198, dst(d), 0x0101, 0x0120 | wide, 0x0100 };

   for (unsigned b = 0; b < 32; b++)
      if (v >> b & 1)
         w[map[b][0]] |= 1 << map[b][1];
   if (wide) {
      w[5] = 0x0154;
      w[6] = 0x0038;
      put(e, w, 7);
   } else {
      w[4] = 0x0154;
      w[5] = 0x0038;
      put(e, w, 6);
   }
}

/* r[d]:r[d+1] = common-store dwords u, u + 1 + (r[idx] << shift) */
static void
addr(struct emit *e, unsigned d, unsigned u, unsigned idx, unsigned sh)
{
   EMIT(e, 0x1189, dst(d), (1 | idx << 2) << 8 | 0x81 | 4 * (u & 31), 0x4100 | (u >> 5) << 1, 0x01a1,
        0x0600 | sh << 13);
}

static void
mem_at(struct emit *e, bool store, unsigned size, unsigned reg, unsigned pair)
{
   EMIT(e, size << 11 | 0x0310, (1 | pair << 2) << 8 | (store ? 0x80 : 0) | reg << 1, 0x8800);
}

static void
tg_load(struct emit *e, unsigned d, unsigned word)
{
   EMIT(e, 0x2314, 0x0300 | d << 1, 0x8100 | (word & 0xff), 0x0200 | word >> 8);
}

static void wait(struct emit *e) { EMIT(e, 0x004d); }

/* ---- the program frame ---------------------------------------------------- */

static void
jump(uint16_t *w, uint16_t op, unsigned offset)
{
   w[0] = op;
   w[1] = 0x0100 | offset;
   w[2] = 0;
}

/* prologue (lane test, mask stack start), jump-none to the epilogue, body,
 * pop, jump to the end word at the next 64-byte block */
static void
frame(struct emit *out, const struct emit *body, unsigned nregs)
{
   const unsigned st = nregs;
   uint16_t j[3];

   EMIT(out, 0x1181, 0x8181 | 2 * (2 * G9P_CS_THREADS), 0x4100, 0xacb7);
   EMIT(out, 0x01c8, dst(st), 0x0101, 0x0120, 0x46d1);
   if (2 * (3 + body->n) >= 0x100) {
      out->error = "a body past 0x100 bytes needs the long jump (to do)";
      return;
   }
   jump(j, 0x0900, 2 * (3 + body->n));
   put(out, j, 3);
   put(out, body->w, body->n);
   EMIT(out, 0x21c8, dst(st), 0x0180 | st << 2, 0x46d2);
   unsigned jpos = 2 * out->n, end = (jpos + 6 + 63) / 64 * 64;
   jump(j, 0x0100, end - jpos);
   put(out, j, 3);
   while (2 * out->n < end)
      EMIT(out, 0xc000);
   EMIT(out, 0x8000);
   while (out->n % 32)
      EMIT(out, 0xc000);
}

/* ---- register allocation: linear scan over one block ---------------------- */

struct ctx {
   nir_shader *nir;
   struct emit e;
   int *reg;           /* SSA index -> register (one per scalar def), -1 none */
   unsigned *last;     /* SSA index -> instruction index of its last use */
   bool used[G9P_MAX_REGS];
   unsigned nregs;     /* highest register used + 1 */
   unsigned ip;        /* the instruction being translated */
   const unsigned *ssbo_binding;
};

static int
grab(struct ctx *c, unsigned n)
{
   for (unsigned r = 1; r + n <= G9P_MAX_REGS; r++) {
      unsigned k;

      for (k = 0; k < n && !c->used[r + k]; k++)
         ;
      if (k == n) {
         for (k = 0; k < n; k++)
            c->used[r + k] = true;
         if (r + n > c->nregs)
            c->nregs = r + n;
         return r;
      }
   }
   c->e.error = "out of registers (spilling: to do)";
   return 1;
}

static void
release(struct ctx *c, unsigned r, unsigned n)
{
   for (unsigned k = 0; k < n; k++)
      c->used[r + k] = false;
}

static bool
note_use(nir_src *src, void *data)
{
   struct ctx *c = data;

   c->last[src->ssa->index] = c->ip;
   return true;
}

static unsigned
reg_of(struct ctx *c, nir_src s)
{
   if (c->reg[s.ssa->index] < 0)
      c->e.error = "a value without a register";
   return c->reg[s.ssa->index];
}

/* free the registers of the sources whose last use this instruction is */
static bool
free_dead(nir_src *src, void *data)
{
   struct ctx *c = data;
   unsigned i = src->ssa->index;

   if (c->last[i] == c->ip && c->reg[i] > 0) {
      release(c, c->reg[i], 1);
      c->reg[i] = -c->reg[i];     /* dead: kept negative so a second source of the same def is not freed twice */
   }
   return true;
}

static unsigned
def_reg(struct ctx *c, nir_def *d)
{
   if (d->num_components != 1)
      c->e.error = "vector values (scalarize first)";
   c->reg[d->index] = grab(c, 1);
   return c->reg[d->index];
}

/* ---- instructions ------------------------------------------------------------ */

static void
emit_alu(struct ctx *c, nir_alu_instr *alu)
{
   unsigned s[3] = { 0 };

   for (unsigned i = 0; i < nir_op_infos[alu->op].num_inputs; i++) {
      if (alu->src[i].swizzle[0] != 0) {
         c->e.error = "swizzled source (scalarize first)";
         return;
      }
      s[i] = reg_of(c, alu->src[i].src);
   }
   /* the destination may take a source's register: sources read first */
   nir_foreach_src(&alu->instr, free_dead, c);
   unsigned d = def_reg(c, &alu->def);
   struct emit *e = &c->e;

   switch (alu->op) {
   case nir_op_iadd: alu2(e, 0xa1, d, s[0], s[1]); break;
   case nir_op_isub: alu2(e, 0xa5, d, s[0], s[1]); break;
   case nir_op_imul: alu2(e, 0xa2, d, s[0], s[1]); break;
   case nir_op_iand: alu2(e, 0xc8, d, s[0], s[1]); break;
   case nir_op_ior: alu2(e, 0xce, d, s[0], s[1]); break;
   case nir_op_ixor: alu2(e, 0xc6, d, s[0], s[1]); break;
   case nir_op_mov: alu2(e, 0xce, d, s[0], s[0]); break;
   case nir_op_fadd: alu2(e, 0x44, d, s[0], s[1]); break;
   case nir_op_fmul: alu2(e, 0x48, d, s[0], s[1]); break;
   case nir_op_fsat: c->e.error = "fsat alone (to do)"; break;
   case nir_op_ffma: ffma(e, d, s[0], s[1], s[2]); break;
   case nir_op_ishl: shift(e, d, s[0], s[1], false); break;
   case nir_op_ushr: shift(e, d, s[0], s[1], true); break;
   case nir_op_u2f32: convert(e, 0x14, d, s[0]); break;
   case nir_op_i2f32: convert(e, 0x16, d, s[0]); break;
   case nir_op_f2u32: convert(e, 0x10, d, s[0]); break;
   case nir_op_f2i32: convert(e, 0x12, d, s[0]); break;
   case nir_op_frcp: funary(e, 0x80, d, s[0]); break;
   case nir_op_frsq: funary(e, 0x81, d, s[0]); break;
   case nir_op_flog2: funary(e, 0x84, d, s[0]); break;
   case nir_op_fexp2: funary(e, 0x85, d, s[0]); break;
   case nir_op_ffloor: funary(e, 0x40, d, s[0]); break;
   case nir_op_ineg:
   case nir_op_fneg:
   case nir_op_fabs: {
      /* with a constant in a scratch register: 0 - a; a ^ sign; a & ~sign */
      unsigned t = grab(c, 1);

      if (alu->op == nir_op_ineg) {
         mov_imm(e, t, 0);
         alu2(e, 0xa5, d, t, s[0]);
      } else if (alu->op == nir_op_fneg) {
         mov_imm(e, t, 0x80000000);
         alu2(e, 0xc6, d, s[0], t);
      } else {
         mov_imm(e, t, 0x7fffffff);
         alu2(e, 0xc8, d, s[0], t);
      }
      release(c, t, 1);
      break;
   }
   default:
      c->e.error = "an ALU operation not taken yet";
      fprintf(stderr, "g9p: %s\n", nir_op_infos[alu->op].name);
   }
}

static void
emit_intrinsic(struct ctx *c, nir_intrinsic_instr *in)
{
   struct emit *e = &c->e;

   switch (in->intrinsic) {
   case nir_intrinsic_load_local_invocation_index:
      c->reg[in->def.index] = 0;      /* r0, as the PDS left it */
      return;
   case nir_intrinsic_load_workgroup_id: {
      /* x only (one-dimensional grids so far): the uses read .x, which is
       * all the register holds */
      if (nir_def_components_read(&in->def) & ~1u) {
         c->e.error = "workgroup ID y / z (to do)";
         return;
      }
      unsigned d = grab(c, 1);
      c->reg[in->def.index] = d;
      tg_load(e, d, 0);
      wait(e);
      return;
   }
   case nir_intrinsic_load_ssbo: {
      if (!nir_src_is_const(in->src[0]) || in->def.num_components != 1 || in->def.bit_size != 32) {
         c->e.error = "load_ssbo: a constant block, one 32-bit component only (to do)";
         return;
      }
      unsigned slot = c->ssbo_binding[nir_src_as_uint(in->src[0])];
      unsigned off = reg_of(c, in->src[1]);
      unsigned pair = grab(c, 2);

      addr(e, pair, 2 * slot, off, 0);
      nir_foreach_src(&in->instr, free_dead, c);
      unsigned d = def_reg(c, &in->def);
      mem_at(e, false, 4, d, pair);
      wait(e);
      release(c, pair, 2);
      return;
   }
   case nir_intrinsic_store_ssbo: {
      if (!nir_src_is_const(in->src[1]) || nir_src_num_components(in->src[0]) != 1) {
         c->e.error = "store_ssbo: a constant block, one component only (to do)";
         return;
      }
      unsigned slot = c->ssbo_binding[nir_src_as_uint(in->src[1])];
      unsigned v = reg_of(c, in->src[0]), off = reg_of(c, in->src[2]);
      unsigned pair = grab(c, 2);

      addr(e, pair, 2 * slot, off, 0);
      mem_at(e, true, 4, v, pair);
      release(c, pair, 2);
      nir_foreach_src(&in->instr, free_dead, c);
      return;
   }
   default:
      c->e.error = "an intrinsic not taken yet";
      fprintf(stderr, "g9p: %s\n", nir_intrinsic_infos[in->intrinsic].name);
   }
}

/* ---- the pipeline --------------------------------------------------------------- */

static void
optimize(nir_shader *nir)
{
   bool progress;

   do {
      progress = false;
      NIR_PASS(progress, nir, nir_opt_copy_prop);
      NIR_PASS(progress, nir, nir_opt_dce);
      NIR_PASS(progress, nir, nir_opt_cse);
      NIR_PASS(progress, nir, nir_opt_algebraic);
      NIR_PASS(progress, nir, nir_opt_constant_folding);
      NIR_PASS(progress, nir, nir_opt_dead_cf);
   } while (progress);
}

static const unsigned identity_binding[32] = {
   0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
   16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
};

const char *
g9p_compile(nir_shader *nir, bool print, const unsigned *ssbo_binding, struct g9p_shader *out)
{
   struct ctx c = { .nir = nir, .nregs = 1, .ssbo_binding = ssbo_binding ? ssbo_binding : identity_binding };

   memset(out, 0, sizeof(*out));
   if (nir->info.stage != MESA_SHADER_COMPUTE)
      return "compute shaders only so far";
   if (nir->info.workgroup_size[1] != 1 || nir->info.workgroup_size[2] != 1 ||
       nir->info.workgroup_size[0] > 512)
      return "workgroups of up to 512 in x only (to do)";
   for (unsigned i = 0; i < 3; i++)
      out->local_size[i] = nir->info.workgroup_size[i];

   NIR_PASS(_, nir, nir_lower_vars_to_ssa);
   NIR_PASS(_, nir, nir_lower_system_values);
   /* the local ID from the index (r0): one dimension, x = the index */
   const nir_lower_compute_system_values_options csv = { .lower_cs_local_id_to_index = true };
   NIR_PASS(_, nir, nir_lower_compute_system_values, &csv);
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_ssbo, nir_address_format_32bit_index_offset);
   NIR_PASS(_, nir, nir_lower_alu_to_scalar, NULL, NULL);
   NIR_PASS(_, nir, nir_lower_io_to_scalar, nir_var_mem_ssbo, NULL, NULL);
   optimize(nir);
   if (print)
      nir_print_shader(nir, stdout);

   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   if (!nir_cf_list_is_empty_block(&impl->body) && exec_list_length(&impl->body) != 1)
      return "control flow (to do)";
   nir_index_ssa_defs(impl);
   c.reg = malloc(impl->ssa_alloc * sizeof(int));
   c.last = calloc(impl->ssa_alloc, sizeof(unsigned));
   for (unsigned i = 0; i < impl->ssa_alloc; i++)
      c.reg[i] = -1;
   c.used[0] = true;      /* r0: the local index */

   /* last uses */
   c.ip = 0;
   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         nir_foreach_src(instr, note_use, &c);
         c.ip++;
      }
   }
   /* translate */
   c.ip = 0;
   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         switch (instr->type) {
         case nir_instr_type_load_const: {
            nir_load_const_instr *lc = nir_instr_as_load_const(instr);

            if (lc->def.num_components != 1 || lc->def.bit_size != 32) {
               c.e.error = "a constant other than one 32-bit scalar";
               break;
            }
            if (nir_def_is_unused(&lc->def))
               break;
            mov_imm(&c.e, def_reg(&c, &lc->def), lc->value[0].u32);
            break;
         }
         case nir_instr_type_alu:
            emit_alu(&c, nir_instr_as_alu(instr));
            break;
         case nir_instr_type_intrinsic:
            emit_intrinsic(&c, nir_instr_as_intrinsic(instr));
            break;
         case nir_instr_type_jump:
            break;
         default:
            c.e.error = "an instruction type not taken yet";
         }
         if (c.e.error)
            break;
         c.ip++;
      }
   }
   free(c.reg);
   free(c.last);
   if (c.e.error) {
      free(c.e.w);
      return c.e.error;
   }

   struct emit prog = { 0 };
   frame(&prog, &c.e, c.nregs);
   free(c.e.w);
   if (prog.error) {
      free(prog.w);
      return prog.error;
   }
   out->code = prog.w;
   out->size = 2 * prog.n;
   out->nregs = c.nregs;
   return NULL;
}

int
g9p_write_pack(const char *path, const char *name, const struct g9p_shader *s)
{
   char info_name[64];
   uint32_t info[4] = { s->nregs, s->local_size[0], s->local_size[1], s->local_size[2] };
   FILE *f = fopen(path, "wb");
   uint32_t hdr[2] = { 0x54503947, 2 };      /* "G9PT", 2 entries */
   uint32_t off = 8 + 2 * 32;
   char ent[32];

   if (!f)
      return -1;
   snprintf(info_name, sizeof(info_name), "%s.info", name);
   fwrite(hdr, 4, 2, f);
   memset(ent, 0, sizeof(ent));
   strncpy(ent, name, 23);
   memcpy(ent + 24, &off, 4);
   memcpy(ent + 28, &s->size, 4);
   fwrite(ent, 1, 32, f);
   memset(ent, 0, sizeof(ent));
   strncpy(ent, info_name, 23);
   off += (s->size + 3) & ~3u;
   memcpy(ent + 24, &off, 4);
   uint32_t isz = sizeof(info);
   memcpy(ent + 28, &isz, 4);
   fwrite(ent, 1, 32, f);
   fwrite(s->code, 1, s->size, f);
   static const uint8_t zero[4];
   fwrite(zero, 1, ((s->size + 3) & ~3u) - s->size, f);
   fwrite(info, 4, 4, f);
   return fclose(f);
}
