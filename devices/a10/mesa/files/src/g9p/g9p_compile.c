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

/* d = a << n / a >> n (logical), n < 32 */
static void
shift_imm(struct emit *e, unsigned d, unsigned a, unsigned n, bool right)
{
   EMIT(e, right ? 0x519e : 0x119a, (right ? 0 : 0x8000) | dst(d), 0x0101 | a << 10, 0x0120,
        0x0101 | (n & 31) << 1, 0x0120, right ? 0x14f4 : 0x14f0);
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

/* compares: the condition codes (g9pasm.py COND) */
#define C_ULT 0xb6
#define C_UGT 0xb7
#define C_EQ  0xbd
#define C_SLT 0xbe
#define C_SGT 0xbf
#define C_FLT 0x59
#define C_FGT 0x5d
#define C_FEQ 0x55

static bool cond_is_float(uint8_t code) { return code == C_FLT || code == C_FGT || code == C_FEQ; }

static void
cmp_reg(struct emit *e, uint8_t code, unsigned a, unsigned b)
{
   EMIT(e, BANKS | 0x0181, srcs(a, b), (cond_is_float(code) ? 0x6000 : 0xa000) | code);
}

/* compare r with an immediate below 1024 */
static void
cmp_imm(struct emit *e, uint8_t code, unsigned r, unsigned imm)
{
   EMIT(e, 0x1181 | ((imm >> 9) & 1) << 13, 0x0101 | (imm & 0x3f) << 1 | r << 10,
        0x0120 | ((imm >> 6) & 7) << 1, 0xa100 | code, 0x0070);
}

/* d = c in the lanes where (a COND b) holds; movf() sets the others */
static void
sel(struct emit *e, uint8_t code, unsigned d, unsigned a, unsigned b, unsigned c)
{
   EMIT(e, BANKS | 0x099e, dst(d), srcs(a, b), 0x0180 | c << 2, 0x6000 | code);
}

static void
movf(struct emit *e, unsigned d, unsigned other)
{
   EMIT(e, 0x21f8, dst(d), 0x0180 | other << 2, 0x0154, 0x0038);
}

/* ---- the execution-mask stack (in register st, the one after the program's) */

static uint16_t st_src(unsigned st) { return 0x0100 | st << 2; }

static void push_init(struct emit *e, unsigned st) { EMIT(e, 0x01c8, dst(st), 0x0101, 0x0120, 0x46d1); }

/* keep the lanes whose compare held (invert: failed) */
static void
push_if(struct emit *e, unsigned st, bool invert)
{
   EMIT(e, 0x21c8, dst(st), st_src(st), 0x46d1 | (invert ? 0x2000 : 0));
}

static void else_(struct emit *e, unsigned st) { EMIT(e, 0x21c8, dst(st), st_src(st), 0x46d4); }

static void
pop(struct emit *e, unsigned st, unsigned levels)
{
   EMIT(e, 0x21c8, dst(st), 0x0080 | st_src(st), 0x0600 | 0xd2 | levels << 14);
}

/* a loop opens two levels: break and continue */
static void loop_begin(struct emit *e, unsigned st) { EMIT(e, 0x21c8, dst(st), st_src(st), 0x86d0); }

/* take the lanes whose compare failed (when_true: held) out of the loop;
 * after an always-true compare it brings the continued lanes back */
static void
loop_while(struct emit *e, unsigned st, bool when_true)
{
   EMIT(e, 0x21c8, dst(st), st_src(st), 0x86d7 | (when_true ? 0x2000 : 0));
}

/* set the active lanes' level to n: break (enclosing ifs + 2), continue (+ 1) */
static void
set_level(struct emit *e, unsigned st, unsigned n)
{
   EMIT(e, 0x0188, dst(st), 0x0101 | n << 1, 0x0120, 0x86d6);
}

#define JMP_ANY  0x0101     /* while any lane is active */
#define JMP_NONE 0x0900     /* when no lane is */

/* a jump `offset` bytes from its own start; offset bits 8 / 24 in bits 0 */
static void
jump_at(uint16_t *w, uint16_t op, int offset)
{
   uint32_t o = offset;

   w[0] = op;
   w[1] = (o & 0xfefe) | 0x0100 | (o >> 8 & 1);
   w[2] = (o >> 16 & 0xfefe) | (o >> 24 & 1);
}

static unsigned
jump(struct emit *e, uint16_t op, int offset)
{
   uint16_t w[3];
   unsigned at = e->n;

   jump_at(w, op, offset);
   put(e, w, 3);
   return at;
}

/* point the jump at word `at` to the current end of the code */
static void
patch_here(struct emit *e, unsigned at)
{
   jump_at(e->w + at, e->w[at], 2 * (int)(e->n - at));
}

/* ---- the program frame ---------------------------------------------------- */

/* prologue (lane test, mask stack start), jump-none to the epilogue, body,
 * pop, jump to the end word at the next 64-byte block */
static void
frame(struct emit *out, const struct emit *body, unsigned st)
{
   EMIT(out, 0x1181, 0x8181 | 2 * (2 * G9P_CS_THREADS), 0x4100, 0xacb7);
   push_init(out, st);
   unsigned j = jump(out, JMP_NONE, 0);
   put(out, body->w, body->n);
   patch_here(out, j);
   pop(out, st, 1);
   unsigned jpos = 2 * out->n, end = (jpos + 6 + 63) / 64 * 64;
   jump(out, 0x0100, end - jpos);
   while (2 * out->n < end)
      EMIT(out, 0xc000);
   EMIT(out, 0x8000);
   while (out->n % 32)
      EMIT(out, 0xc000);
}

/* ---- values and registers ----------------------------------------------------
 *
 * The program is translated twice by the same code. The first pass (analyse)
 * numbers the instructions and the control-flow points in program order
 * (ip) and records each value's live interval in positions: a use at 2 ip, a
 * definition at 2 ip + 1 (2 ip when it must not share a register with the
 * instruction's sources: "early"). A value live into a loop stays live to the
 * loop's end (the back edge). Registers are then given by a linear scan over
 * these intervals, and the second pass emits the code.
 *
 * Intervals in program order are what lanes need too: a value written in the
 * lanes of one branch of an if keeps its register through the other branch,
 * so the other lanes cannot overwrite it.
 */

struct val {
   int start, end;     /* positions; start < 0: no register */
   int reg;
   uint8_t size;       /* registers: 1, or 2 for an address pair */
   bool fixed;         /* r0, the local index */
};

struct ctx {
   nir_shader *nir;
   struct emit e;
   bool analyse;
   struct val *v;
   unsigned nv, cap, nssa;
   unsigned next_scratch;
   bool *fused;        /* SSA index: a compare emitted at its one use */
   unsigned ip;
   unsigned st;        /* the mask stack's register */
   unsigned depth;     /* ifs inside the innermost loop */
   bool loop_continues;
   unsigned nregs;
   const unsigned *ssbo_binding;
};

static void
touch(struct ctx *c, unsigned i, int p)
{
   struct val *v = &c->v[i];

   if (v->start < 0 || p < v->start)
      v->start = p;
   if (p > v->end)
      v->end = p;
}

static unsigned
R(struct ctx *c, nir_src s)
{
   unsigned i = s.ssa->index;

   if (c->v[i].fixed)
      return c->v[i].reg;
   if (c->analyse) {
      touch(c, i, 2 * c->ip);
      return 0;
   }
   if (c->v[i].reg < 0)
      c->e.error = "a value without a register";
   return c->v[i].reg < 0 ? 0 : c->v[i].reg;
}

static unsigned
D(struct ctx *c, nir_def *d, bool early)
{
   if (d->num_components != 1 || d->bit_size != 32) {
      c->e.error = "a value other than one 32-bit scalar (to do)";
      return 0;
   }
   if (c->analyse) {
      touch(c, d->index, 2 * c->ip + !early);
      return 0;
   }
   return c->v[d->index].reg < 0 ? 0 : c->v[d->index].reg;
}

/* registers for this instruction only */
static unsigned
scratch(struct ctx *c, unsigned size)
{
   if (c->analyse) {
      if (c->nv == c->cap) {
         c->cap *= 2;
         c->v = realloc(c->v, c->cap * sizeof(*c->v));
      }
      c->v[c->nv++] = (struct val){ .start = 2 * c->ip, .end = 2 * c->ip + 1, .reg = -1, .size = size };
      return 0;
   }
   return c->v[c->next_scratch++].reg;
}

/* a NIR register (from out of SSA: the phi webs) */
static unsigned
reg_access(struct ctx *c, nir_src decl, bool write)
{
   unsigned i = decl.ssa->index;

   if (c->analyse) {
      touch(c, i, 2 * c->ip);
      if (write)
         touch(c, i, 2 * c->ip + 1);
      return 0;
   }
   return c->v[i].reg < 0 ? 0 : c->v[i].reg;
}

static int
by_start(const void *a, const void *b, void *data)
{
   const struct val *v = data;
   const unsigned *x = a, *y = b;

   if (v[*x].start != v[*y].start)
      return v[*x].start - v[*y].start;
   return (int)*x - (int)*y;
}

static void
allocate(struct ctx *c)
{
   unsigned *order = malloc(c->nv * sizeof(unsigned)), n = 0;
   unsigned *active = malloc(c->nv * sizeof(unsigned)), na = 0;
   bool used[G9P_MAX_REGS] = { true };      /* r0: the local index */

   for (unsigned i = 0; i < c->nv; i++)
      if (c->v[i].start >= 0 && !c->v[i].fixed)
         order[n++] = i;
   qsort_r(order, n, sizeof(unsigned), by_start, c->v);
   c->nregs = 1;
   for (unsigned k = 0; k < n && !c->e.error; k++) {
      struct val *v = &c->v[order[k]];

      for (unsigned a = 0; a < na;) {
         struct val *o = &c->v[active[a]];

         if (o->end < v->start) {
            for (unsigned r = 0; r < o->size; r++)
               used[o->reg + r] = false;
            active[a] = active[--na];
         } else {
            a++;
         }
      }
      int r;
      for (r = 1; r + v->size <= G9P_MAX_REGS; r++) {
         unsigned q;

         for (q = 0; q < v->size && !used[r + q]; q++)
            ;
         if (q == v->size)
            break;
      }
      if (r + v->size > G9P_MAX_REGS) {
         c->e.error = "out of registers (spilling: to do)";
         break;
      }
      v->reg = r;
      for (unsigned q = 0; q < v->size; q++)
         used[r + q] = true;
      if (r + v->size > c->nregs)
         c->nregs = r + v->size;
      active[na++] = order[k];
   }
   free(order);
   free(active);
}

/* ---- conditions ---------------------------------------------------------------- */

struct cond {
   uint8_t code;
   unsigned a, b;
   bool imm0;          /* compare a with 0 (b unused) */
   bool inv;           /* the condition is the compare's failing */
};

static bool
cmp_info(nir_op op, uint8_t *code, bool *inv)
{
   *inv = false;
   switch (op) {
   case nir_op_ilt32: *code = C_SLT; return true;
   case nir_op_ige32: *code = C_SLT; *inv = true; return true;
   case nir_op_ult32: *code = C_ULT; return true;
   case nir_op_uge32: *code = C_ULT; *inv = true; return true;
   case nir_op_ieq32: *code = C_EQ; return true;
   case nir_op_ine32: *code = C_EQ; *inv = true; return true;
   case nir_op_flt32: *code = C_FLT; return true;
   case nir_op_fge32: *code = C_FLT; *inv = true; return true;    /* NaN: to check */
   case nir_op_feq32: *code = C_FEQ; return true;
   case nir_op_fneu32: *code = C_FEQ; *inv = true; return true;
   default: return false;
   }
}

/* a compare used once, by an if or as a select's condition: emitted there */
static bool
fusable(nir_alu_instr *alu)
{
   uint8_t code;
   bool inv;

   if (!cmp_info(alu->op, &code, &inv) || !list_is_singular(&alu->def.uses))
      return false;
   for (unsigned i = 0; i < 2; i++)
      if (alu->src[i].swizzle[0] != 0)
         return false;
   nir_src *use = list_first_entry(&alu->def.uses, nir_src, use_link);
   if (nir_src_is_if(use))
      return true;
   nir_instr *ui = nir_src_parent_instr(use);
   if (ui->type != nir_instr_type_alu)
      return false;
   nir_alu_instr *u = nir_instr_as_alu(ui);
   return u->op == nir_op_b32csel && use == &u->src[0].src;
}

static struct cond
cond_of(struct ctx *c, nir_src s)
{
   struct cond k = { 0 };

   if (c->fused[s.ssa->index]) {
      nir_alu_instr *alu = nir_def_as_alu(s.ssa);

      cmp_info(alu->op, &k.code, &k.inv);
      k.a = R(c, alu->src[0].src);
      k.b = R(c, alu->src[1].src);
   } else {
      /* a 0 / ~0 value: true where it is not 0 */
      k.code = C_EQ;
      k.a = R(c, s);
      k.imm0 = true;
      k.inv = true;
   }
   return k;
}

static void
emit_cmp(struct ctx *c, const struct cond *k)
{
   if (k->imm0)
      cmp_imm(&c->e, k->code, k->a, 0);
   else
      cmp_reg(&c->e, k->code, k->a, k->b);
}

/* d = k ? x : y */
static void
select_(struct ctx *c, struct cond k, unsigned d, unsigned x, unsigned y)
{
   if (k.imm0) {
      k.b = scratch(c, 1);
      mov_imm(&c->e, k.b, 0);
   }
   if (k.inv) {
      unsigned t = x;
      x = y;
      y = t;
   }
   sel(&c->e, k.code, d, k.a, k.b, x);
   movf(&c->e, d, y);
}

/* ---- instructions ------------------------------------------------------------ */

/* d = a OP constant, through a scratch register */
static void
alu_const(struct ctx *c, uint16_t op, unsigned d, unsigned a, uint32_t k, bool const_first)
{
   unsigned t = scratch(c, 1);

   mov_imm(&c->e, t, k);
   if (const_first)
      alu2(&c->e, op, d, t, a);
   else
      alu2(&c->e, op, d, a, t);
}

static void
emit_alu(struct ctx *c, nir_alu_instr *alu)
{
   const nir_op op = alu->op;
   struct emit *e = &c->e;
   unsigned s[3] = { 0 };
   uint8_t code;
   bool inv;

   if (c->fused[alu->def.index])
      return;                                   /* emitted at its use */
   if (op == nir_op_b32csel) {
      struct cond k = cond_of(c, alu->src[0].src);
      unsigned x = R(c, alu->src[1].src), y = R(c, alu->src[2].src);

      select_(c, k, D(c, &alu->def, true), x, y);
      return;
   }
   for (unsigned i = 0; i < nir_op_infos[op].num_inputs; i++) {
      if (alu->src[i].swizzle[0] != 0) {
         c->e.error = "swizzled source (scalarize first)";
         return;
      }
      s[i] = R(c, alu->src[i].src);
   }
   if (cmp_info(op, &code, &inv)) {
      /* a 0 / ~0 value */
      unsigned d = D(c, &alu->def, false), t = scratch(c, 1);

      mov_imm(e, t, inv ? 0 : ~0u);
      sel(e, code, d, s[0], s[1], t);
      mov_imm(e, t, inv ? ~0u : 0);
      movf(e, d, t);
      return;
   }
   switch (op) {
   case nir_op_extract_u8:
   case nir_op_extract_u16: {
      /* (a >> (bits * k)) & mask, k a constant */
      const unsigned bits = op == nir_op_extract_u8 ? 8 : 16;
      unsigned d = D(c, &alu->def, false);

      if (!nir_src_is_const(alu->src[1].src)) {
         c->e.error = "extract with a variable index";
         return;
      }
      unsigned k = nir_alu_src_as_uint(alu->src[1]);
      if (k) {
         shift_imm(e, d, s[0], bits * k, true);
         s[0] = d;
      }
      if (bits * (k + 1) < 32)
         alu_const(c, 0xc8, d, s[0], (1u << bits) - 1, false);
      else if (!k)
         alu2(e, 0xce, d, s[0], s[0]);
      return;
   }
   case nir_op_imin: case nir_op_imax: case nir_op_umin: case nir_op_umax:
   case nir_op_fmin: case nir_op_fmax: {
      static const uint8_t codes[] = {
         [nir_op_imin] = C_SLT, [nir_op_imax] = C_SGT, [nir_op_umin] = C_ULT,
         [nir_op_umax] = C_UGT, [nir_op_fmin] = C_FLT, [nir_op_fmax] = C_FGT,
      };
      unsigned d = D(c, &alu->def, true);

      sel(e, codes[op], d, s[0], s[1], s[0]);
      movf(e, d, s[1]);
      return;
   }
   default:
      break;
   }

   unsigned d = D(c, &alu->def, false);

   switch (op) {
   case nir_op_iadd: alu2(e, 0xa1, d, s[0], s[1]); break;
   case nir_op_isub: alu2(e, 0xa5, d, s[0], s[1]); break;
   case nir_op_imul: alu2(e, 0xa2, d, s[0], s[1]); break;
   case nir_op_iand: alu2(e, 0xc8, d, s[0], s[1]); break;
   case nir_op_ior: alu2(e, 0xce, d, s[0], s[1]); break;
   case nir_op_ixor: alu2(e, 0xc6, d, s[0], s[1]); break;
   case nir_op_mov: alu2(e, 0xce, d, s[0], s[0]); break;
   case nir_op_fadd: alu2(e, 0x44, d, s[0], s[1]); break;
   case nir_op_fmul: alu2(e, 0x48, d, s[0], s[1]); break;
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
   /* with a constant in a scratch register */
   case nir_op_ineg: alu_const(c, 0xa5, d, s[0], 0, true); break;            /* 0 - a */
   case nir_op_fneg: alu_const(c, 0xc6, d, s[0], 0x80000000, false); break;  /* a ^ sign */
   case nir_op_fabs: alu_const(c, 0xc8, d, s[0], 0x7fffffff, false); break;  /* a & ~sign */
   case nir_op_inot: alu_const(c, 0xc6, d, s[0], ~0u, false); break;
   case nir_op_b2i32: alu_const(c, 0xc8, d, s[0], 1, false); break;          /* ~0 -> 1 */
   case nir_op_b2f32: alu_const(c, 0xc8, d, s[0], 0x3f800000, false); break; /* ~0 -> 1.0 */
   case nir_op_fsat: alu_const(c, 0x4a, d, s[0], 0x3f800000, false); break;  /* sat(a * 1.0) */
   default:
      c->e.error = "an ALU operation not taken yet";
      fprintf(stderr, "g9p: %s\n", nir_op_infos[op].name);
   }
}

static void
emit_intrinsic(struct ctx *c, nir_intrinsic_instr *in)
{
   struct emit *e = &c->e;

   switch (in->intrinsic) {
   case nir_intrinsic_load_local_invocation_index:
      c->v[in->def.index].fixed = true;      /* r0, as the PDS left it */
      c->v[in->def.index].reg = 0;
      return;
   case nir_intrinsic_load_workgroup_id: {
      /* x only (one-dimensional grids so far): the uses read .x, which is
       * all the register holds */
      if (nir_def_components_read(&in->def) & ~1u) {
         c->e.error = "workgroup ID y / z (to do)";
         return;
      }
      unsigned d;

      if (c->analyse) {
         touch(c, in->def.index, 2 * c->ip + 1);
         d = 0;
      } else {
         d = c->v[in->def.index].reg;
      }
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
      unsigned off = R(c, in->src[1]), pair = scratch(c, 2);
      unsigned d = D(c, &in->def, false);

      addr(e, pair, 2 * slot, off, 0);
      mem_at(e, false, 4, d, pair);
      wait(e);
      return;
   }
   case nir_intrinsic_store_ssbo: {
      if (!nir_src_is_const(in->src[1]) || nir_src_num_components(in->src[0]) != 1) {
         c->e.error = "store_ssbo: a constant block, one component only (to do)";
         return;
      }
      unsigned slot = c->ssbo_binding[nir_src_as_uint(in->src[1])];
      unsigned v = R(c, in->src[0]), off = R(c, in->src[2]), pair = scratch(c, 2);

      addr(e, pair, 2 * slot, off, 0);
      mem_at(e, true, 4, v, pair);
      return;
   }
   case nir_intrinsic_decl_reg:
      if (nir_intrinsic_num_components(in) != 1 || nir_intrinsic_bit_size(in) != 32 ||
          nir_intrinsic_num_array_elems(in))
         c->e.error = "a register other than one 32-bit scalar";
      return;
   case nir_intrinsic_load_reg: {
      unsigned r = reg_access(c, in->src[0], false);
      unsigned d = D(c, &in->def, false);

      alu2(e, 0xce, d, r, r);
      return;
   }
   case nir_intrinsic_store_reg: {
      if (nir_intrinsic_write_mask(in) != 1) {
         c->e.error = "store_reg: one component only";
         return;
      }
      unsigned x = R(c, in->src[0]);
      unsigned r = reg_access(c, in->src[1], true);

      alu2(e, 0xce, r, x, x);
      return;
   }
   default:
      c->e.error = "an intrinsic not taken yet";
      fprintf(stderr, "g9p: %s\n", nir_intrinsic_infos[in->intrinsic].name);
   }
}

static void
emit_instr(struct ctx *c, nir_instr *instr)
{
   switch (instr->type) {
   case nir_instr_type_load_const: {
      nir_load_const_instr *lc = nir_instr_as_load_const(instr);

      if (lc->def.num_components != 1 || lc->def.bit_size != 32) {
         c->e.error = "a constant other than one 32-bit scalar";
         break;
      }
      if (!nir_def_is_unused(&lc->def))
         mov_imm(&c->e, D(c, &lc->def, false), lc->value[0].u32);
      break;
   }
   case nir_instr_type_alu:
      emit_alu(c, nir_instr_as_alu(instr));
      break;
   case nir_instr_type_intrinsic:
      emit_intrinsic(c, nir_instr_as_intrinsic(instr));
      break;
   case nir_instr_type_jump: {
      nir_jump_instr *j = nir_instr_as_jump(instr);

      if (j->type == nir_jump_break) {
         set_level(&c->e, c->st, c->depth + 2);
      } else if (j->type == nir_jump_continue) {
         set_level(&c->e, c->st, c->depth + 1);
         c->loop_continues = true;
      } else {
         c->e.error = "a jump other than break / continue";
      }
      break;
   }
   default:
      c->e.error = "an instruction type not taken yet";
   }
   c->ip++;
}

/* ---- control flow ------------------------------------------------------------ */

static void emit_cf_list(struct ctx *c, struct exec_list *list);

static void
emit_if(struct ctx *c, nir_if *nif)
{
   struct cond k = cond_of(c, nif->condition);

   emit_cmp(c, &k);
   push_if(&c->e, c->st, k.inv);
   unsigned j = jump(&c->e, JMP_NONE, 0);      /* past the then-side when no lane takes it */
   c->ip++;
   c->depth++;
   emit_cf_list(c, &nif->then_list);
   if (!nir_cf_list_is_empty_block(&nif->else_list)) {
      patch_here(&c->e, j);
      else_(&c->e, c->st);
      j = jump(&c->e, JMP_NONE, 0);
      c->ip++;
      emit_cf_list(c, &nif->else_list);
   }
   patch_here(&c->e, j);
   c->depth--;
   pop(&c->e, c->st, 1);
   c->ip++;
}

static void
emit_loop(struct ctx *c, nir_loop *loop)
{
   const unsigned depth = c->depth;
   const bool continues = c->loop_continues;
   const int start = 2 * c->ip;

   if (nir_loop_has_continue_construct(loop)) {
      c->e.error = "a loop continue construct (lower first)";
      return;
   }
   loop_begin(&c->e, c->st);
   c->ip++;
   unsigned top = c->e.n;
   c->depth = 0;
   c->loop_continues = false;
   emit_cf_list(c, &loop->body);
   if (c->loop_continues) {
      cmp_reg(&c->e, C_EQ, 0, 0);
      loop_while(&c->e, c->st, false);
   }
   jump(&c->e, JMP_ANY, -2 * (int)(c->e.n - top));
   pop(&c->e, c->st, 2);
   const int end = 2 * c->ip + 1;
   c->ip++;
   c->depth = depth;
   c->loop_continues = continues;

   /* a value live into the loop stays live through it (the back edge) */
   if (c->analyse)
      for (unsigned i = 0; i < c->nv; i++)
         if (c->v[i].start >= 0 && c->v[i].start < start && c->v[i].end >= start && c->v[i].end < end)
            c->v[i].end = end;
}

static void
emit_cf_list(struct ctx *c, struct exec_list *list)
{
   foreach_list_typed(nir_cf_node, node, node, list) {
      if (c->e.error)
         return;
      switch (node->type) {
      case nir_cf_node_block:
         nir_foreach_instr(instr, nir_cf_node_as_block(node)) {
            emit_instr(c, instr);
            if (c->e.error)
               return;
         }
         break;
      case nir_cf_node_if:
         emit_if(c, nir_cf_node_as_if(node));
         break;
      case nir_cf_node_loop:
         emit_loop(c, nir_cf_node_as_loop(node));
         break;
      default:
         c->e.error = "a control-flow node not taken";
      }
   }
}

/* ---- the pipeline --------------------------------------------------------------- */

static void
optimize(nir_shader *nir)
{
   bool progress;

   do {
      progress = false;
      NIR_PASS(progress, nir, nir_lower_vars_to_ssa);
      NIR_PASS(progress, nir, nir_opt_copy_prop);
      NIR_PASS(progress, nir, nir_opt_dce);
      NIR_PASS(progress, nir, nir_opt_cse);
      NIR_PASS(progress, nir, nir_opt_algebraic);
      NIR_PASS(progress, nir, nir_opt_constant_folding);
      NIR_PASS(progress, nir, nir_opt_dead_cf);
      NIR_PASS(progress, nir, nir_opt_remove_phis);
      NIR_PASS(progress, nir, nir_opt_if, nir_opt_if_optimize_phi_true_false);
      const nir_opt_peephole_select_options ps = { .limit = 8, .expensive_alu_ok = true };
      NIR_PASS(progress, nir, nir_opt_peephole_select, &ps);
   } while (progress);
}

static const unsigned identity_binding[32] = {
   0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
   16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
};

static void
translate(struct ctx *c, nir_function_impl *impl, bool analyse)
{
   c->analyse = analyse;
   c->e.n = 0;
   c->ip = 0;
   c->depth = 0;
   c->loop_continues = false;
   c->next_scratch = c->nssa;
   emit_cf_list(c, &impl->body);
}

const char *
g9p_compile(nir_shader *nir, bool print, const unsigned *ssbo_binding, struct g9p_shader *out)
{
   struct ctx c = { .nir = nir, .ssbo_binding = ssbo_binding ? ssbo_binding : identity_binding };

   memset(out, 0, sizeof(*out));
   if (nir->info.stage != MESA_SHADER_COMPUTE)
      return "compute shaders only so far";
   if (nir->info.workgroup_size[1] != 1 || nir->info.workgroup_size[2] != 1 ||
       nir->info.workgroup_size[0] > 512)
      return "workgroups of up to 512 in x only (to do)";
   for (unsigned i = 0; i < 3; i++)
      out->local_size[i] = nir->info.workgroup_size[i];

   NIR_PASS(_, nir, nir_lower_continue_constructs);
   NIR_PASS(_, nir, nir_lower_vars_to_ssa);
   NIR_PASS(_, nir, nir_lower_system_values);
   /* the local ID from the index (r0): one dimension, x = the index */
   const nir_lower_compute_system_values_options csv = { .lower_cs_local_id_to_index = true };
   NIR_PASS(_, nir, nir_lower_compute_system_values, &csv);
   NIR_PASS(_, nir, nir_lower_explicit_io, nir_var_mem_ssbo, nir_address_format_32bit_index_offset);
   NIR_PASS(_, nir, nir_lower_alu_to_scalar, NULL, NULL);
   NIR_PASS(_, nir, nir_lower_io_to_scalar, nir_var_mem_ssbo, NULL, NULL);
   NIR_PASS(_, nir, nir_lower_phis_to_scalar, NULL, NULL);
   optimize(nir);
   NIR_PASS(_, nir, nir_lower_bool_to_int32);
   NIR_PASS(_, nir, nir_opt_copy_prop);
   NIR_PASS(_, nir, nir_opt_dce);
   NIR_PASS(_, nir, nir_convert_from_ssa, true, false);
   NIR_PASS(_, nir, nir_opt_dce);

   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   nir_index_ssa_defs(impl);
   if (print)
      nir_print_shader(nir, stdout);

   c.nssa = c.nv = impl->ssa_alloc;
   c.cap = c.nv + 64;
   c.v = malloc(c.cap * sizeof(*c.v));
   c.fused = calloc(impl->ssa_alloc + 1, sizeof(bool));
   for (unsigned i = 0; i < c.nv; i++)
      c.v[i] = (struct val){ .start = -1, .end = -1, .reg = -1, .size = 1 };
   nir_foreach_block(block, impl)
      nir_foreach_instr(instr, block)
         if (instr->type == nir_instr_type_alu && fusable(nir_instr_as_alu(instr)))
            c.fused[nir_instr_as_alu(instr)->def.index] = true;

   translate(&c, impl, true);
   if (!c.e.error)
      allocate(&c);
   if (!c.e.error) {
      c.st = c.nregs;
      translate(&c, impl, false);
   }
   if (print && !c.e.error)
      printf("g9p: %u registers, %u values, %u words of body\n", c.nregs, c.nv, c.e.n);
   free(c.v);
   free(c.fused);
   if (c.e.error) {
      free(c.e.w);
      return c.e.error;
   }

   struct emit prog = { 0 };
   frame(&prog, &c.e, c.st);
   free(c.e.w);
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
