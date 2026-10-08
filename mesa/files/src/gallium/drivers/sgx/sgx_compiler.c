/*
 * Copyright 2026 Project Cascadia
 * SPDX-License-Identifier: MIT
 *
 * NIR to USSE, fragment shaders (sgx_compiler.h; docs/research/p105-mesa.md,
 * M13c).  The first compiler is deliberately plain:
 *
 *  - every value is a scalar F32 (NIR scalarised, ints and bools lowered to
 *    floats as for the other float-only GPUs);
 *  - the shader is one block: NIR unrolls loops and turns ifs into
 *    selects, and what it cannot flatten is not compiled;
 *  - values live in temporaries, one each, always the even (x) lane of a
 *    64-bit register, so no instruction ever has to line lanes up; the
 *    varyings stay where the PDS iterated them (input i in pa4i..pa4i+3,
 *    F32) and the uniforms where it loaded them (constant buffer word n in
 *    san), any lane, which the float instructions' swizzles reach;
 *  - a temporary is taken when a value is made and given back after its
 *    last use, in the order NIR has the instructions;
 *  - constants are loaded (LIMM) where they are used;
 *  - negation and absolute value fold into the operands that have them;
 *  - comparisons subtract and test the difference (a conditional move);
 *  - the colour is moved into four registers in a row and packed to 8 bits
 *    a channel into o0.
 *
 * Speed (F16 for mediump, two lanes an instruction, constants from the
 * hardware's table, sharing loaded constants) comes later.
 */
#include "sgx_compiler.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler/glsl_types.h"
#include "compiler/nir/nir.h"
#include "compiler/nir/nir_lower_blend.h"
#include "util/format/u_formats.h"
#include "util/ralloc.h"
#include "util/u_dynarray.h"
#include "util/u_math.h"
#include "util/u_memory.h"

#include "sgx_usse.h"

/* 32-bit temporaries the compiler hands out; values made by the ALU take
 * even ones, a texture's four values a block of four */
#define MAX_TEMPS 64
#define MAX_UNITS 32

struct operand {
   unsigned id;         /* the value: its def's index * 4 + the component */
   bool neg, abs;
   bool is_const;
   float c;
};

/* temporaries made for one instruction, given back after it */
struct scratch {
   unsigned reg[8];
   unsigned n;
};

struct comp {
   struct sgx_fs *fs;
   struct util_dynarray code;           /* uint64_t */
   struct usse_reg *loc;                /* where each value is */
   bool *owned;                         /* loc is a temporary of its own (any lane) */
   int *last_use;                       /* the last instruction that reads it */
   bool used[MAX_TEMPS];
   unsigned top;                        /* temporaries used: highest + 1 */
   int input_of_slot[VARYING_SLOT_MAX];
   unsigned ninputs, nuniforms;
   int slot_of_unit[MAX_UNITS];         /* a texture unit's state words, by slot */
   unsigned nsamplers, sampler_sa;
   int blend_sa;                        /* the blend colour's words, -1 none */
   struct operand colour[4];
   bool have_colour[4];
   char *why;
   unsigned why_size;
   bool failed;
};

static void
fail(struct comp *c, const char *fmt, ...)
{
   va_list ap;

   if (c->failed)
      return;
   va_start(ap, fmt);
   vsnprintf(c->why, c->why_size, fmt, ap);
   va_end(ap);
   c->failed = true;
}

static void
emit(struct comp *c, uint64_t w)
{
   util_dynarray_append(&c->code, w);
}

/* n free temporaries in a row, the first a multiple of align */
static unsigned
block(struct comp *c, unsigned n, unsigned align)
{
   for (unsigned r = 0; r + n <= MAX_TEMPS; r += align) {
      unsigned i;

      for (i = 0; i < n && !c->used[r + i]; i++)
         ;
      if (i < n)
         continue;
      for (i = 0; i < n; i++)
         c->used[r + i] = true;
      c->top = MAX2(c->top, r + n);
      return r;
   }
   fail(c, "more values live at once than %u temporaries hold", MAX_TEMPS);
   return 0;
}

static unsigned
take(struct comp *c)            /* an even temporary */
{
   return block(c, 1, 2);
}

static void
give_back(struct comp *c, unsigned r)
{
   c->used[r] = false;
}

static unsigned
scratch_take(struct comp *c, struct scratch *s)
{
   unsigned r = take(c);

   assert(s->n < ARRAY_SIZE(s->reg));
   s->reg[s->n++] = r;
   return r;
}

static void
scratch_give_back(struct comp *c, struct scratch *s)
{
   for (unsigned i = 0; i < s->n; i++)
      give_back(c, s->reg[i]);
   s->n = 0;
}

static uint32_t
f32_bits(float f)
{
   uint32_t u;

   memcpy(&u, &f, 4);
   return u;
}

/* a constant in a temporary made for this instruction */
static struct usse_reg
constant(struct comp *c, struct scratch *s, float f)
{
   struct usse_reg r = usse_reg(USSE_TEMP, scratch_take(c, s));

   emit(c, usse_limm(r, f32_bits(f)));
   return r;
}

/* The value a source reads, through moves, vectors, negation and absolute
 * value (outermost first: an fabs keeps an outer negation, an fneg under an
 * fabs does nothing). */
static struct operand
resolve(nir_scalar s)
{
   struct operand o = { 0 };

   for (;;) {
      s = nir_scalar_chase_movs(s);
      if (!nir_scalar_is_alu(s))
         break;
      if (nir_scalar_alu_op(s) == nir_op_fneg) {
         if (!o.abs)
            o.neg = !o.neg;
      } else if (nir_scalar_alu_op(s) == nir_op_fabs) {
         o.abs = true;
      } else {
         break;
      }
      s = nir_scalar_chase_alu_src(s, 0);
   }
   o.id = s.def->index * 4 + s.comp;
   if (nir_scalar_is_const(s)) {
      float f = nir_scalar_as_float(s);

      o.is_const = true;
      o.c = o.abs ? fabsf(f) : f;
      o.c = o.neg ? -o.c : o.c;
      o.neg = o.abs = false;
   }
   return o;
}

static struct operand
alu_operand(nir_alu_instr *alu, unsigned i)
{
   return resolve(nir_get_scalar(alu->src[i].src.ssa, alu->src[i].swizzle[0]));
}

/* operand slots: what each takes without a move first */
enum {
   TAKES_NEG = 1 << 0,
   TAKES_ABS = 1 << 1,
   TAKES_LANE_Y = 1 << 2,
   TAKES_SA = 1 << 3,
   TAKES_ALL = TAKES_NEG | TAKES_ABS | TAKES_LANE_Y | TAKES_SA,
};

/* The register for an operand in a slot that takes what `takes` says: the
 * value where it is, or a temporary made for this instruction. */
static struct usse_reg
get(struct comp *c, struct operand o, unsigned takes, struct scratch *s)
{
   struct usse_reg r, t;

   if (o.is_const)
      return constant(c, s, o.c);
   r = c->loc[o.id];
   r.neg = o.neg;
   r.abs = o.abs;
   if ((r.neg && !(takes & TAKES_NEG)) || (r.abs && !(takes & TAKES_ABS)) ||
       ((r.num & 1) && !(takes & TAKES_LANE_Y)) ||
       (r.bank == USSE_SA && !(takes & TAKES_SA))) {
      t = usse_reg(USSE_TEMP, scratch_take(c, s));
      if (r.neg || r.abs)        /* the modifiers applied: times one */
         emit(c, usse_fop(USSE_NMAD_MUL, t, r, constant(c, s, 1.0f)));
      else
         emit(c, usse_fmov(t, r));
      return t;
   }
   return r;
}

/* the value an instruction makes, in a temporary of its own */
static struct usse_reg
define(struct comp *c, nir_def *def)
{
   unsigned id = def->index * 4, r = take(c);

   c->loc[id] = usse_reg(USSE_TEMP, r);
   c->owned[id] = true;
   if (c->last_use[id] < 0) {    /* never read */
      give_back(c, r);
      c->owned[id] = false;
   }
   return c->loc[id];
}

static void
binary(struct comp *c, enum usse_nmad_op op, nir_alu_instr *alu, struct scratch *s)
{
   struct operand a = alu_operand(alu, 0), b = alu_operand(alu, 1), t;
   struct usse_reg ra, rb;

   /* the second source takes no negation: swap (all four commute), and
    * a product of two negations is the product */
   if (b.neg && !a.neg) {
      t = a;
      a = b;
      b = t;
   }
   if (op == USSE_NMAD_MUL && a.neg && b.neg)
      a.neg = b.neg = false;
   ra = get(c, a, TAKES_ALL, s);
   rb = get(c, b, TAKES_ALL & ~TAKES_NEG, s);
   emit(c, usse_fop(op, define(c, &alu->def), ra, rb));
}

static void
comparison(struct comp *c, nir_alu_instr *alu, struct scratch *s)
{
   struct operand a = alu_operand(alu, 0), b = alu_operand(alu, 1);
   struct usse_reg diff = usse_reg(USSE_TEMP, scratch_take(c, s)), one, zero, ra, rb;
   enum usse_test test = alu->op == nir_op_slt || alu->op == nir_op_sge ?
                         USSE_TEST_LT0 : USSE_TEST_EQ0;
   bool swap = alu->op == nir_op_sge || alu->op == nir_op_sne;
   uint64_t w;

   /* a - b = -b + a */
   b.neg = !b.neg;
   if (b.is_const)
      b.c = -b.c, b.neg = false;
   ra = get(c, a, TAKES_ALL & ~TAKES_NEG, s);
   rb = get(c, b, TAKES_ALL, s);
   emit(c, usse_fop(USSE_NMAD_ADD, diff, rb, ra));
   one = constant(c, s, 1.0f);
   zero = constant(c, s, 0.0f);
   usse_fmovc(&w, test, define(c, &alu->def), diff, swap ? zero : one, swap ? one : zero);
   emit(c, w);
}

static void
select_(struct comp *c, nir_alu_instr *alu, struct scratch *s)
{
   /* fcsel: c != 0 ? a : b -- a conditional move: no modifiers, one lane */
   struct usse_reg rc = get(c, alu_operand(alu, 0), TAKES_SA, s);
   struct usse_reg ra = get(c, alu_operand(alu, 1), TAKES_SA, s);
   struct usse_reg rb = get(c, alu_operand(alu, 2), TAKES_SA, s);
   uint64_t w;

   if (!usse_fmovc(&w, USSE_TEST_NE0, define(c, &alu->def), rc, ra, rb))
      fail(c, "a select's operands in different lanes");
   emit(c, w);
}

static void
alu(struct comp *c, nir_alu_instr *alu, struct scratch *s)
{
   struct usse_reg d, a, t;

   if (alu->op == nir_op_mov || alu->op == nir_op_fneg || alu->op == nir_op_fabs ||
       nir_op_is_vec(alu->op))
      return;           /* read through by resolve() */
   if (alu->def.bit_size != 32 || alu->def.num_components != 1) {
      fail(c, "%s: %u x %u bits", nir_op_infos[alu->op].name, alu->def.num_components,
           alu->def.bit_size);
      return;
   }
   switch (alu->op) {
   case nir_op_fadd: binary(c, USSE_NMAD_ADD, alu, s); return;
   case nir_op_fmul: binary(c, USSE_NMAD_MUL, alu, s); return;
   case nir_op_fmin: binary(c, USSE_NMAD_MIN, alu, s); return;
   case nir_op_fmax: binary(c, USSE_NMAD_MAX, alu, s); return;
   case nir_op_frcp:
   case nir_op_frsq:
   case nir_op_flog2:
   case nir_op_fexp2:
      a = get(c, alu_operand(alu, 0), TAKES_ALL, s);
      emit(c, usse_fcomp(alu->op == nir_op_frcp ? USSE_COMP_RCP :
                         alu->op == nir_op_frsq ? USSE_COMP_RSQ :
                         alu->op == nir_op_flog2 ? USSE_COMP_LOG2 : USSE_COMP_EXP2,
                         define(c, &alu->def), a));
      return;
   case nir_op_fsqrt:   /* 1 / (1 / sqrt x): 0 for 0, where x * rsq x is not */
      a = get(c, alu_operand(alu, 0), TAKES_ALL, s);
      t = usse_reg(USSE_TEMP, scratch_take(c, s));
      emit(c, usse_fcomp(USSE_COMP_RSQ, t, a));
      emit(c, usse_fcomp(USSE_COMP_RCP, define(c, &alu->def), t));
      return;
   case nir_op_ffract:
      a = get(c, alu_operand(alu, 0), TAKES_LANE_Y | TAKES_SA, s);
      emit(c, usse_fop(USSE_NMAD_FRC, define(c, &alu->def), a, a));
      return;
   case nir_op_fsat:
      a = get(c, alu_operand(alu, 0), TAKES_ALL, s);
      t = usse_reg(USSE_TEMP, scratch_take(c, s));
      emit(c, usse_fop(USSE_NMAD_MAX, t, a, constant(c, s, 0.0f)));
      d = define(c, &alu->def);
      emit(c, usse_fop(USSE_NMAD_MIN, d, t, constant(c, s, 1.0f)));
      return;
   case nir_op_slt:
   case nir_op_sge:
   case nir_op_seq:
   case nir_op_sne:
      comparison(c, alu, s);
      return;
   case nir_op_fcsel:
      select_(c, alu, s);
      return;
   default:
      fail(c, "no %s yet", nir_op_infos[alu->op].name);
      return;
   }
}

static bool uniform_word(struct comp *c, nir_intrinsic_instr *in, unsigned *at);

/* an operand into a given register (any lane) */
static void
move_into(struct comp *c, struct usse_reg d, struct operand o, struct scratch *s)
{
   struct usse_reg r;

   if (o.is_const) {
      emit(c, usse_limm(d, f32_bits(o.c)));
      return;
   }
   r = c->loc[o.id];
   r.neg = o.neg;
   r.abs = o.abs;
   if (r.neg || r.abs)          /* the modifiers applied: times one */
      emit(c, usse_fop(USSE_NMAD_MUL, d, r, constant(c, s, 1.0f)));
   else
      emit(c, usse_fmov(d, r));
}

static struct operand
tex_operand(nir_tex_instr *tex, int src, unsigned comp)
{
   return resolve(nir_get_scalar(tex->src[src].src.ssa, comp));
}

/* texture2D, with a bias or a level: the coordinates into a register pair,
 * SMP for an F32 texel into four temporaries in a row, wait for it.  The
 * texture's four state words are in sa, after the uniforms (the draw puts
 * them there, sgx_draw.c). */
static void
texture(struct comp *c, nir_tex_instr *tex, struct scratch *s)
{
   int coord = nir_tex_instr_src_index(tex, nir_tex_src_coord);
   int bias = nir_tex_instr_src_index(tex, nir_tex_src_bias);
   int lod = nir_tex_instr_src_index(tex, nir_tex_src_lod);
   enum usse_smp_lod mode = USSE_SMP_NONE;
   struct usse_reg lodreg = usse_reg(USSE_TEMP, 0);
   unsigned pair, d, id = tex->def.index * 4;

   if ((tex->op != nir_texop_tex && tex->op != nir_texop_txb && tex->op != nir_texop_txl) ||
       (tex->sampler_dim != GLSL_SAMPLER_DIM_2D && tex->sampler_dim != GLSL_SAMPLER_DIM_EXTERNAL) ||
       tex->is_shadow || tex->is_array ||
       coord < 0 || tex->texture_index >= MAX_UNITS ||
       c->slot_of_unit[tex->texture_index] < 0) {
      fail(c, "no %s texture lookups yet", tex->sampler_dim == GLSL_SAMPLER_DIM_CUBE ?
           "cube" : "such");
      return;
   }
   pair = block(c, 2, 2);
   s->reg[s->n++] = pair;
   s->reg[s->n++] = pair + 1;
   move_into(c, usse_reg(USSE_TEMP, pair), tex_operand(tex, coord, 0), s);
   move_into(c, usse_reg(USSE_TEMP, pair + 1), tex_operand(tex, coord, 1), s);
   if (bias >= 0 || lod >= 0) {
      mode = bias >= 0 ? USSE_SMP_BIAS : USSE_SMP_LOD;
      lodreg = usse_reg(USSE_TEMP, scratch_take(c, s));
      move_into(c, lodreg, tex_operand(tex, bias >= 0 ? bias : lod, 0), s);
   }
   d = block(c, 4, 4);
   emit(c, usse_smp2d(USSE_SMP_F32, USSE_SMP_COORD_F32, usse_reg(USSE_TEMP, d),
                      usse_reg(USSE_TEMP, pair),
                      usse_reg(USSE_SA, c->sampler_sa + 4 * c->slot_of_unit[tex->texture_index]),
                      mode, lodreg));
   emit(c, USSE_WDF0);
   /* the sampler hands an 8-bit channel back as its integer value (0..255,
    * checked with gltex): to 0..1 (the copy it reads is always RGBA8) */
   {
      struct usse_reg k = constant(c, s, 1.0f / 255.0f);

      for (unsigned i = 0; i < 4; i++)
         if (i < tex->def.num_components && c->last_use[id + i] >= 0)
            emit(c, usse_fop(USSE_NMAD_MUL, usse_reg(USSE_TEMP, d + i),
                             usse_reg(USSE_TEMP, d + i), k));
   }
   for (unsigned i = 0; i < 4; i++) {
      c->loc[id + i] = usse_reg(USSE_TEMP, d + i);
      c->owned[id + i] = i < tex->def.num_components && c->last_use[id + i] >= 0;
      if (!c->owned[id + i])
         give_back(c, d + i);
   }
}

/* Before anything is emitted: how many uniform words the shader reads (the
 * texture states go after them) and a slot for each texture unit. */
static void
scan(struct comp *c, nir_function_impl *impl)
{
   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         if (instr->type == nir_instr_type_intrinsic) {
            nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);
            unsigned at;

            if ((in->intrinsic == nir_intrinsic_load_uniform ||
                 in->intrinsic == nir_intrinsic_load_ubo) && uniform_word(c, in, &at))
               c->nuniforms = MAX2(c->nuniforms, at + in->def.num_components);
            if (in->intrinsic == nir_intrinsic_load_blend_const_color_r_float ||
                in->intrinsic == nir_intrinsic_load_blend_const_color_g_float ||
                in->intrinsic == nir_intrinsic_load_blend_const_color_b_float ||
                in->intrinsic == nir_intrinsic_load_blend_const_color_a_float)
               c->blend_sa = 0;      /* placed below */
         } else if (instr->type == nir_instr_type_tex) {
            nir_tex_instr *tex = nir_instr_as_tex(instr);

            if (tex->texture_index < MAX_UNITS && c->slot_of_unit[tex->texture_index] < 0) {
               if (c->nsamplers == SGX_FS_MAX_SAMPLERS) {
                  fail(c, "more than %u textures", SGX_FS_MAX_SAMPLERS);
                  return;
               }
               c->fs->sampler_unit[c->nsamplers] = tex->texture_index;
               c->slot_of_unit[tex->texture_index] = c->nsamplers++;
            }
         }
      }
   }
   c->sampler_sa = align(c->nuniforms, 4);
   if (c->blend_sa >= 0)
      c->blend_sa = c->sampler_sa + 4 * c->nsamplers;
}

static int
type_size_vec4(const struct glsl_type *type, bool bindless)
{
   return glsl_count_attribute_slots(type, false);
}

/* the constant buffer word a uniform load starts at; false (and fail) if
 * it is not one we take */
static bool
uniform_word(struct comp *c, nir_intrinsic_instr *in, unsigned *at)
{
   if (in->intrinsic == nir_intrinsic_load_uniform) {
      /* vec4 slots (the screen does not pack uniforms) */
      if (!nir_src_is_const(in->src[0])) {
         fail(c, "an indirect uniform");
         return false;
      }
      *at = (nir_intrinsic_base(in) + nir_src_as_uint(in->src[0])) * 4;
   } else {
      if (!nir_src_is_const(in->src[0]) || nir_src_as_uint(in->src[0]) ||
          !nir_src_is_const(in->src[1])) {
         fail(c, "an indirect or second constant buffer");
         return false;
      }
      *at = nir_src_as_uint(in->src[1]) / 4;
   }
   if (*at + in->def.num_components > 128) {
      fail(c, "uniforms past word 128");
      return false;
   }
   return true;
}

static void
intrinsic(struct comp *c, nir_intrinsic_instr *in)
{
   nir_src *offset;
   unsigned at, id = in->def.index * 4;

   switch (in->intrinsic) {
   case nir_intrinsic_load_input: {
      unsigned slot;

      offset = nir_get_io_offset_src(in);
      if (!nir_src_is_const(*offset)) {
         fail(c, "an indirect varying");
         return;
      }
      slot = nir_intrinsic_io_semantics(in).location + nir_src_as_uint(*offset);
      if (slot < VARYING_SLOT_VAR0 && (slot < VARYING_SLOT_COL0 || slot > VARYING_SLOT_TEX7)) {
         fail(c, "input %s", gl_varying_slot_name_for_stage(slot, MESA_SHADER_FRAGMENT));
         return;
      }
      if (c->input_of_slot[slot] < 0) {
         if (c->ninputs == SGX_FRAME_MAX_VARYINGS) {
            fail(c, "more than %u varyings", SGX_FRAME_MAX_VARYINGS);
            return;
         }
         c->fs->input_slot[c->ninputs] = slot;
         c->input_of_slot[slot] = c->ninputs++;
      }
      at = 4 * c->input_of_slot[slot] + nir_intrinsic_component(in);
      for (unsigned i = 0; i < in->def.num_components; i++)
         c->loc[id + i] = usse_reg(USSE_PA, at + i);
      return;
   }
   case nir_intrinsic_load_uniform:
   case nir_intrinsic_load_ubo:
      if (!uniform_word(c, in, &at))
         return;
      for (unsigned i = 0; i < in->def.num_components; i++)
         c->loc[id + i] = usse_reg(USSE_SA, at + i);
      c->nuniforms = MAX2(c->nuniforms, at + in->def.num_components);
      return;
   case nir_intrinsic_store_output: {
      unsigned loc = nir_intrinsic_io_semantics(in).location;
      unsigned mask = nir_intrinsic_write_mask(in), comp = nir_intrinsic_component(in);

      if (loc != FRAG_RESULT_COLOR && loc != FRAG_RESULT_DATA0) {
         fail(c, "output %s", gl_frag_result_name(loc));
         return;
      }
      for (unsigned i = 0; i < in->src[0].ssa->num_components; i++) {
         if (!(mask & 1 << i) || comp + i >= 4)
            continue;
         c->colour[comp + i] = resolve(nir_get_scalar(in->src[0].ssa, i));
         c->have_colour[comp + i] = true;
      }
      return;
   }
   case nir_intrinsic_load_output: {
      /* the colour the tile holds (blending, nir_lower_blend): o0's four
       * 8-bit channels into four temporaries */
      unsigned d;

      if (!nir_intrinsic_io_semantics(in).fb_fetch_output ||
          (nir_intrinsic_io_semantics(in).location != FRAG_RESULT_DATA0 &&
           nir_intrinsic_io_semantics(in).location != FRAG_RESULT_COLOR)) {
         fail(c, "reads an output");
         return;
      }
      d = block(c, 4, 2);
      {
         /* SGX_DEBUG_FBFETCH=word: unpack a constant instead of o0 (a test
          * of the channel selects) */
         const char *dbg = getenv("SGX_DEBUG_FBFETCH");
         struct usse_reg src = usse_reg(USSE_OUTPUT, 0);
         unsigned k = 0;

         if (dbg) {
            k = take(c);
            src = usse_reg(USSE_TEMP, k);
            emit(c, usse_limm(src, strtoul(dbg, NULL, 0)));
         }
         emit(c, usse_unpack_unorm8(usse_reg(USSE_TEMP, d), src, 0));
         emit(c, usse_unpack_unorm8(usse_reg(USSE_TEMP, d + 2), src, 2));
         for (unsigned i = 0; i < 4; i++) {
            c->loc[id + i] = usse_reg(USSE_TEMP, d + i);
            c->owned[id + i] = i < in->def.num_components && c->last_use[id + i] >= 0;
            if (!c->owned[id + i])
               give_back(c, d + i);
         }
         if (dbg)
            give_back(c, k);
      }
      return;
   }
   case nir_intrinsic_load_blend_const_color_r_float:
   case nir_intrinsic_load_blend_const_color_g_float:
   case nir_intrinsic_load_blend_const_color_b_float:
   case nir_intrinsic_load_blend_const_color_a_float:
      /* (the intrinsics are not in rgba order) */
      c->loc[id] = usse_reg(USSE_SA, c->blend_sa +
         (in->intrinsic == nir_intrinsic_load_blend_const_color_r_float ? 0 :
          in->intrinsic == nir_intrinsic_load_blend_const_color_g_float ? 1 :
          in->intrinsic == nir_intrinsic_load_blend_const_color_b_float ? 2 : 3));
      return;
   case nir_intrinsic_load_barycentric_pixel:
   case nir_intrinsic_load_barycentric_centroid:
   case nir_intrinsic_load_barycentric_sample:
      return;
   default:
      fail(c, "no %s yet", nir_intrinsic_infos[in->intrinsic].name);
      return;
   }
}

/* what an instruction reads, for liveness: f(c, operand) for each */
static void
for_each_read(struct comp *c, nir_instr *instr, void (*f)(struct comp *, struct operand, int),
              int index)
{
   if (instr->type == nir_instr_type_alu) {
      nir_alu_instr *alu = nir_instr_as_alu(instr);

      if (alu->op == nir_op_mov || alu->op == nir_op_fneg || alu->op == nir_op_fabs ||
          nir_op_is_vec(alu->op))
         return;
      for (unsigned i = 0; i < nir_op_infos[alu->op].num_inputs; i++)
         f(c, alu_operand(alu, i), index);
   } else if (instr->type == nir_instr_type_intrinsic) {
      nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);

      if (in->intrinsic == nir_intrinsic_store_output)
         for (unsigned i = 0; i < in->src[0].ssa->num_components; i++)
            f(c, resolve(nir_get_scalar(in->src[0].ssa, i)), index);
   } else if (instr->type == nir_instr_type_tex) {
      nir_tex_instr *tex = nir_instr_as_tex(instr);

      for (unsigned i = 0; i < tex->num_srcs; i++) {
         if (tex->src[i].src_type == nir_tex_src_coord) {
            f(c, tex_operand(tex, i, 0), index);
            if (tex->src[i].src.ssa->num_components > 1)
               f(c, tex_operand(tex, i, 1), index);
         } else if (tex->src[i].src_type == nir_tex_src_bias ||
                    tex->src[i].src_type == nir_tex_src_lod) {
            f(c, tex_operand(tex, i, 0), index);
         }
      }
   }
}

static void
note_use(struct comp *c, struct operand o, int index)
{
   if (!o.is_const)
      c->last_use[o.id] = index;
}

static void
release(struct comp *c, struct operand o, int index)
{
   /* the colour's values stay until output() has moved them */
   if (c->have_colour[0] || c->have_colour[1] || c->have_colour[2] || c->have_colour[3])
      for (unsigned i = 0; i < 4; i++)
         if (c->have_colour[i] && !c->colour[i].is_const && c->colour[i].id == o.id)
            return;
   if (!o.is_const && c->last_use[o.id] == index && c->owned[o.id]) {
      give_back(c, c->loc[o.id].num);
      c->owned[o.id] = false;
   }
}

/* the colour: four registers in a row, packed into o0 */
static void
output(struct comp *c)
{
   struct scratch s = { 0 };
   unsigned base;

   if (!c->have_colour[0] && !c->have_colour[1] && !c->have_colour[2] && !c->have_colour[3]) {
      fail(c, "no colour written");
      return;
   }
   base = block(c, 4, 2);
   for (unsigned i = 0; i < 4; i++) {
      struct operand o = c->colour[i];

      if (!c->have_colour[i]) {
         o.is_const = true;
         o.c = i == 3 ? 1.0f : 0.0f;
      }
      move_into(c, usse_reg(USSE_TEMP, base + i), o, &s);
      scratch_give_back(c, &s);
   }
   emit(c, usse_pack_unorm8(0, usse_reg(USSE_TEMP, base)) | USSE_END);
}

static void
optimize(nir_shader *s)
{
   const nir_opt_peephole_select_options flatten = {
      .limit = 1000, .indirect_load_ok = true, .expensive_alu_ok = true,
   };
   bool progress;

   do {
      progress = false;
      NIR_PASS(progress, s, nir_lower_vars_to_ssa);
      NIR_PASS(progress, s, nir_lower_alu_to_scalar, NULL, NULL);
      NIR_PASS(progress, s, nir_lower_phis_to_scalar, NULL, NULL);
      NIR_PASS(progress, s, nir_opt_copy_prop);
      NIR_PASS(progress, s, nir_opt_remove_phis);
      NIR_PASS(progress, s, nir_opt_dce);
      NIR_PASS(progress, s, nir_opt_dead_cf);
      NIR_PASS(progress, s, nir_opt_cse);
      NIR_PASS(progress, s, nir_opt_if, nir_opt_if_optimize_phi_true_false);
      NIR_PASS(progress, s, nir_opt_peephole_select, &flatten);
      NIR_PASS(progress, s, nir_opt_algebraic);
      NIR_PASS(progress, s, nir_opt_constant_folding);
      NIR_PASS(progress, s, nir_opt_undef);
      NIR_PASS(progress, s, nir_opt_loop_unroll);
   } while (progress);
}

/* GL's blending (and colour mask) for render target 0 into the shader:
 * NIR's own lowering, which reads the destination with load_output */
static void
lower_blend(nir_shader *s, const struct sgx_blend_key *k)
{
   nir_lower_blend_options o = { .scalar_blend_const = true };

   o.rt[0].format = PIPE_FORMAT_B8G8R8A8_UNORM;
   o.rt[0].colormask = k->colormask;
   if (k->enable) {
      o.rt[0].rgb.func = k->rgb_func;
      o.rt[0].rgb.src_factor = k->rgb_src;
      o.rt[0].rgb.dst_factor = k->rgb_dst;
      o.rt[0].alpha.func = k->alpha_func;
      o.rt[0].alpha.src_factor = k->alpha_src;
      o.rt[0].alpha.dst_factor = k->alpha_dst;
   } else {
      o.rt[0].rgb.func = o.rt[0].alpha.func = PIPE_BLEND_ADD;
      o.rt[0].rgb.src_factor = o.rt[0].alpha.src_factor = PIPE_BLENDFACTOR_ONE;
      o.rt[0].rgb.dst_factor = o.rt[0].alpha.dst_factor = PIPE_BLENDFACTOR_ZERO;
   }
   NIR_PASS(_, s, nir_lower_blend, &o);
}

struct sgx_fs *
sgx_compile_fs(const nir_shader *fs, const struct sgx_blend_key *blend, char *why,
               unsigned why_size)
{
   bool blending = blend && (blend->enable || blend->colormask != 0xf);
   struct comp c = { 0 };
   nir_function_impl *impl;
   nir_shader *s;
   unsigned n;
   int index;

   c.why = why;
   c.why_size = why_size;
   why[0] = 0;
   if (!(c.fs = CALLOC_STRUCT(sgx_fs)))
      return NULL;
   for (unsigned i = 0; i < ARRAY_SIZE(c.input_of_slot); i++)
      c.input_of_slot[i] = -1;

   for (unsigned i = 0; i < ARRAY_SIZE(c.slot_of_unit); i++)
      c.slot_of_unit[i] = -1;
   c.blend_sa = -1;
   if (blend)
      c.fs->blend = *blend;
   else
      c.fs->blend.colormask = 0xf;

   s = nir_shader_clone(NULL, fs);
   if (blending)
      NIR_PASS(_, s, nir_lower_fragcolor, 1);
   NIR_PASS(_, s, nir_lower_io, nir_var_shader_in | nir_var_shader_out | nir_var_uniform,
            type_size_vec4, 0);
   if (blending)
      lower_blend(s, blend);
   {
      nir_lower_tex_options tex = { .lower_txp = ~0u };

      /* the linear sampler reads B G R A: other orders swizzled back */
      for (unsigned u = 0; blend && u < 8; u++) {
         if (!((blend->tex_swap | blend->tex_x8) >> u & 1))
            continue;
         tex.swizzle_result |= 1u << u;
         tex.swizzles[u][0] = blend->tex_swap >> u & 1 ? 2 : 0;
         tex.swizzles[u][1] = 1;
         tex.swizzles[u][2] = blend->tex_swap >> u & 1 ? 0 : 2;
         tex.swizzles[u][3] = blend->tex_x8 >> u & 1 ? PIPE_SWIZZLE_1 : 3;
      }
      NIR_PASS(_, s, nir_lower_tex, &tex);
   }
   optimize(s);
   NIR_PASS(_, s, nir_lower_int_to_float);
   NIR_PASS(_, s, nir_lower_bool_to_float, true);
   NIR_PASS(_, s, nir_opt_algebraic_late);
   NIR_PASS(_, s, nir_lower_alu_to_scalar, NULL, NULL);
   NIR_PASS(_, s, nir_opt_copy_prop);
   NIR_PASS(_, s, nir_opt_cse);
   NIR_PASS(_, s, nir_opt_dce);

   impl = nir_shader_get_entrypoint(s);
   if (exec_list_length(&impl->body) != 1) {
      fail(&c, "control flow left after flattening");
      goto out;
   }
   nir_index_ssa_defs(impl);
   n = impl->ssa_alloc * 4;
   c.loc = calloc(n, sizeof(*c.loc));
   c.owned = calloc(n, sizeof(*c.owned));
   c.last_use = malloc(n * sizeof(*c.last_use));
   if (!c.loc || !c.owned || !c.last_use) {
      fail(&c, "out of memory");
      goto out;
   }
   for (unsigned i = 0; i < n; i++)
      c.last_use[i] = -1;

   scan(&c, impl);
   if (c.failed)
      goto out;

   /* liveness, then the code, in the same order */
   index = 0;
   nir_foreach_block(block, impl)
      nir_foreach_instr(instr, block)
         for_each_read(&c, instr, note_use, index++);

   util_dynarray_init(&c.code, NULL);
   emit(&c, USSE_PHAS);
   index = 0;
   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         struct scratch sc = { 0 };

         switch (instr->type) {
         case nir_instr_type_alu:
            alu(&c, nir_instr_as_alu(instr), &sc);
            break;
         case nir_instr_type_intrinsic:
            intrinsic(&c, nir_instr_as_intrinsic(instr));
            break;
         case nir_instr_type_tex:
            texture(&c, nir_instr_as_tex(instr), &sc);
            break;
         case nir_instr_type_load_const:
         case nir_instr_type_undef:
            break;
         default:
            fail(&c, "no such instructions yet");
            break;
         }
         scratch_give_back(&c, &sc);
         for_each_read(&c, instr, release, index++);
         if (c.failed)
            goto out;
      }
   }
   output(&c);
   if (c.failed)
      goto out;

   c.fs->prog.ncode = util_dynarray_num_elements(&c.code, uint64_t);
   c.fs->prog.code = MALLOC(c.fs->prog.ncode * sizeof(uint64_t));
   if (!c.fs->prog.code) {
      fail(&c, "out of memory");
      goto out;
   }
   memcpy(c.fs->prog.code, util_dynarray_begin(&c.code), c.fs->prog.ncode * sizeof(uint64_t));
   /* A program that samples runs every instruction for the pixels around
    * the triangle too (skipinv, bit 55, clear -- as iOS's dependent reads
    * have it, corpus t01 and t07): the sampler takes its level of detail
    * from the coordinates of the 2x2 block, and a neighbour that skipped
    * the moves into them handed it garbage (gltex linear: point sampling
    * along the diagonal seam). */
   if (c.nsamplers)
      for (unsigned i = 0; i < c.fs->prog.ncode; i++)
         c.fs->prog.code[i] &= ~(1ull << 55);
   c.fs->prog.ntemps = c.top;
   c.fs->prog.ninputs = c.ninputs;
   c.fs->nuniforms = c.nuniforms;
   c.fs->nsamplers = c.nsamplers;
   c.fs->sampler_sa = c.sampler_sa;
   c.fs->prog.nsa = c.blend_sa >= 0 ? c.blend_sa + 4 :
                    c.nsamplers ? c.sampler_sa + 4 * c.nsamplers : c.nuniforms;
   c.fs->blend_sa = c.blend_sa;

out:
   util_dynarray_fini(&c.code);
   free(c.loc);
   free(c.owned);
   free(c.last_use);
   ralloc_free(s);
   if (c.failed) {
      sgx_fs_destroy(c.fs);
      return NULL;
   }
   return c.fs;
}

void
sgx_fs_destroy(struct sgx_fs *fs)
{
   if (!fs)
      return;
   FREE(fs->prog.code);
   FREE(fs);
}
