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
 * hardware's table, sharing loaded constants) comes later -- but for one
 * kind of program, a colour of varyings and texels multiplied and added,
 * which is all F16, four lanes an instruction (M38, f16_colour()).
 */
#include "sgx_compiler.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler/glsl_types.h"
#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "util/bitscan.h"
#include "compiler/nir/nir_format_convert.h"
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
   unsigned reg[12];
   unsigned n;
};

/* a loop's span: its first instruction's index, its end marker's */
struct loop_span {
   int start, end;
};

struct comp {
   struct sgx_fs *fs;                   /* a fragment shader's, or */
   struct sgx_vs *vs;                   /* a vertex shader's */
   struct util_dynarray code;           /* uint64_t */
   struct usse_reg *loc;                /* where each value is */
   bool *owned;                         /* loc is a temporary of its own (any lane) */
   unsigned nvalues;                    /* of loc, owned, last_use */
   int *last_use;                       /* the last instruction that reads it */
   bool used[MAX_TEMPS];
   unsigned top;                        /* temporaries used: highest + 1 */
   int input_of_slot[VARYING_SLOT_MAX];
   unsigned ninputs, nuniforms;
   /* the uniform words the shader reads (M32): in sa when they fit with
    * the rest -- otherwise words 0..sa_uniforms in sa, and all of them in
    * memory, read by VLDST from the address (less 4) in sa word ubuf_sa */
   unsigned uwords, sa_uniforms, ubuf_sa;
   /* (M43) the uniform buffers read (bit k for buffer k), each one's
    * address less 4 in sa word ubo_sa[k] */
   uint16_t ubo_mask;
   unsigned ubo_sa[SGX_FRAME_MAX_UBOS];
   bool ubuf;
   /* a fragment program whose branches go alike for every pixel (M33):
    * the pixels run together, the branches all of theirs -- or one with
    * some that do not, its pixels each on its own (M20's way) */
   bool divergent;
   bool derivatives;                    /* DSX or DSY in it */
   /* (M34) the colour's four registers, from the start: the values the
    * colour is made of are made there (colour_id: their operands' ids,
    * -1 none), not moved there at the end */
   int out_base;
   int colour_id[4];
   int slot_of_unit[MAX_UNITS];         /* a texture unit's state words, by slot */
   unsigned nsamplers, sampler_sa;
   bool smp;                            /* an SMP in the program */
   /* (M35) the lookups the PDS makes before the program runs -- a 2D
    * texture at a varying's x and y, nothing else -- their varyings, and
    * the varyings only they read (not iterated into the registers) */
   const nir_tex_instr *fetch[SGX_FRAME_MAX_FETCH];
   unsigned fetch_slot[SGX_FRAME_MAX_FETCH];
   unsigned nfetch;
   bool fetch_only[VARYING_SLOT_MAX];
   /* (M39) lookups lower_fetch_coords() gave a varying's x and y as they
    * are, the vertex side doing the rest: each one's SGX_FIX_*; and each
    * lookup's of the PDS (fetch[k]'s) */
   const nir_tex_instr *fixed[SGX_FRAME_MAX_FETCH];
   uint8_t fixed_fix[SGX_FRAME_MAX_FETCH];
   unsigned nfixed;
   uint8_t fetch_fix[SGX_FRAME_MAX_FETCH];
   const uint8_t *vary_fix;             /* a vertex shader's: each varying's SGX_FIX_* */
   /* (M37) the flat-shaded varyings (st's nir_lower_flatshade: GL's
    * glShadeModel(GL_FLAT) colours): the TA's colours, two at most, each
    * iterated into one register after the inputs, its bytes as o0's --
    * colour_of_slot[] their index, -1 not one */
   bool flat[VARYING_SLOT_MAX];
   int colour_of_slot[VARYING_SLOT_MAX];
   unsigned colour_slot[2], ncolours;
   /* a rectangle texture's 1 / width, 1 / height, two words a texture
    * after their state words (nir_lower_tex's lower_rect: GL's rectangle
    * textures are 2D ones to the sampler) -- -1 none */
   int scale_sa;
   bool scales;
   int blend_sa;                        /* the blend colour's words, -1 none */
   /* GL's blending by a SOP2 on the colour packed to bytes and the tile's
    * (o0): src1 the colour unless swapped (M25) */
   bool sop2, sop2_swap;
   struct usse_sop2 sop2_factors;
   bool f16;                            /* the whole program F16's (M38: f16_colour) */
   int logicop;                         /* GL's logic op (pipe_logicop), -1 none (M37) */
   struct operand colour[4];
   bool have_colour[4];
   int kill;                            /* the temporary that is not 0 for a pixel discarded, or -1 */
   bool kills;                          /* the shader discards (scan) */
   /* control flow (M20): the ifs and loops around the instruction, each
    * SSA def's instruction, each loop's span (its first instruction, its
    * end marker), the loops a walk is in, whether the code branches */
   int depth;
   int *def_at;
   struct util_dynarray loops;          /* struct loop_span */
   unsigned nloops;
   struct loop_span active[16];
   unsigned nactive;
   bool branches;
   bool front_when_set;                 /* gl_FrontFacing: the facing bit set is the front */
   /* a vertex shader's outputs: 0 the position, 1 + k varying k of the
    * layout (varying_slot[k]) -- which are written, and the position's
    * values (the varyings go to their registers as they are stored); the
    * attributes it reads */
   const unsigned *varying_slot;
   unsigned nvaryings, nattrs;
   unsigned pa_base;                    /* a vertex program's temporaries: pa from here */
   const uint8_t *attr;                 /* how each attribute comes (M26) */
   struct operand vout[1 + SGX_FRAME_MAX_VARYINGS][4];
   bool have_vout[1 + SGX_FRAME_MAX_VARYINGS][4];
   char *why;
   unsigned why_size;
   bool failed;
   bool keep_order;                     /* NIR's order, not nir_opt_move's (M36) */
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

/* block()'s, -1 when there is no room (and the compile goes on) */
static int
block_try(struct comp *c, unsigned n, unsigned align)
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
   return -1;
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

/* temporary r as a register: a vertex program's are primary attributes
 * after the vertex's (iOS's vertex programs use them as scratch too, the
 * corpus's x03: the fetch's register count covers them) */
static struct usse_reg
treg(struct comp *c, unsigned r)
{
   return c->vs ? usse_reg(USSE_PA, c->pa_base + r) : usse_reg(USSE_TEMP, r);
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
   struct usse_reg r = treg(c, scratch_take(c, s));

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
      t = treg(c, scratch_take(c, s));
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
   unsigned id = def->index * 4, r;

   for (unsigned i = 0; i < 4; i++)
      if (c->colour_id[i] == (int)id) {
         /* (the colour's registers taken at its first value: from the
          * start they held four the program wanted -- 67 a pixel) */
         if (c->out_base == -1 && (c->out_base = block_try(c, 4, 2)) < 0)
            c->out_base = -2;
         if (c->out_base < 0)
            break;
         c->loc[id] = treg(c, c->out_base + i);
         c->owned[id] = false;
         return c->loc[id];
      }
   r = take(c);

   c->loc[id] = treg(c, r);
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
   struct usse_reg diff = treg(c, scratch_take(c, s)), one, zero, ra, rb;
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
   /* fcsel: c != 0 ? a : b -- a conditional move: no modifiers, one lane;
    * the test not in sa (VMOV's first source read 0 from there, dEQP's
    * select_iteration_count loops; iOS tests a uniform with VTST, M31) */
   struct usse_reg rc = get(c, alu_operand(alu, 0), 0, s);
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
      t = treg(c, scratch_take(c, s));
      emit(c, usse_fcomp(USSE_COMP_RSQ, t, a));
      emit(c, usse_fcomp(USSE_COMP_RCP, define(c, &alu->def), t));
      return;
   case nir_op_ffract:
      a = get(c, alu_operand(alu, 0), TAKES_LANE_Y | TAKES_SA, s);
      emit(c, usse_fop(USSE_NMAD_FRC, define(c, &alu->def), a, a));
      return;
   case nir_op_fsat:
      a = get(c, alu_operand(alu, 0), TAKES_ALL, s);
      t = treg(c, scratch_take(c, s));
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

/* texture2D and textureCube, with a bias or a level: the coordinates into
 * registers in a row (a pair; a cube map's direction three), SMP for the
 * texel, wait for it.  The texture's four state words are in sa, after the
 * uniforms (the draw puts them there, sgx_draw.c). */
static void
texture(struct comp *c, nir_tex_instr *tex, struct scratch *s)
{
   int coord = nir_tex_instr_src_index(tex, nir_tex_src_coord);
   int bias = nir_tex_instr_src_index(tex, nir_tex_src_bias);
   int lod = nir_tex_instr_src_index(tex, nir_tex_src_lod);
   enum usse_smp_lod mode = USSE_SMP_NONE;
   struct usse_reg coords;
   struct usse_reg lodreg = treg(c, 0);
   unsigned pair, d, id = tex->def.index * 4;
   bool cube = tex->sampler_dim == GLSL_SAMPLER_DIM_CUBE;
   unsigned ncoord = cube ? 3 : 2;
   uint64_t (*smp)(enum usse_smp_out, enum usse_smp_coord, struct usse_reg, struct usse_reg,
                   struct usse_reg, enum usse_smp_lod, struct usse_reg) =
      cube ? usse_smp3d : usse_smp2d;

   for (unsigned k = 0; k < c->nfetch; k++) {
      if (c->fetch[k] != tex)
         continue;
      /* the PDS's lookup (M35): the texel in the primary attribute after
       * the inputs, as it is -- two VPCKs to F32 0..1 */
      d = block(c, 4, 4);
      emit(c, usse_unpack_unorm8(treg(c, d),
                                 usse_reg(USSE_PA, 4 * c->ninputs + c->ncolours + k), 0));
      emit(c, usse_unpack_unorm8(treg(c, d + 2),
                                 usse_reg(USSE_PA, 4 * c->ninputs + c->ncolours + k), 2));
      goto out;
   }
   /* (a rectangle texture's coordinates are scaled by now: load_texture_scale,
    * and the dimension left as it was) */
   if ((tex->op != nir_texop_tex && tex->op != nir_texop_txb && tex->op != nir_texop_txl) ||
       (tex->sampler_dim != GLSL_SAMPLER_DIM_2D && tex->sampler_dim != GLSL_SAMPLER_DIM_EXTERNAL &&
        tex->sampler_dim != GLSL_SAMPLER_DIM_RECT && !cube) ||
       tex->is_shadow || tex->is_array ||
       coord < 0 || tex->texture_index >= MAX_UNITS ||
       c->slot_of_unit[tex->texture_index] < 0) {
      fail(c, "no such texture lookups yet");
      return;
   }
   /* the coordinates where they are when they are registers in a row
    * from an even one -- a varying's x and y (M34) -- else moved there */
   {
      struct operand o0 = tex_operand(tex, coord, 0);
      bool in_place = true;

      for (unsigned i = 0; i < ncoord && in_place; i++) {
         struct operand o = tex_operand(tex, coord, i);

         in_place = !o.is_const && !o.neg && !o.abs && c->loc[o.id].bank == USSE_PA &&
                    !(c->loc[o0.id].num & 1) && c->loc[o.id].num == c->loc[o0.id].num + i;
      }
      if (in_place) {
         coords = c->loc[o0.id];
      } else {
         pair = block(c, ncoord, 2);
         for (unsigned i = 0; i < ncoord; i++) {
            s->reg[s->n++] = pair + i;
            move_into(c, treg(c, pair + i), tex_operand(tex, coord, i), s);
         }
         coords = treg(c, pair);
      }
   }
   if (bias >= 0 || lod >= 0) {
      /* (a bias where the pixels run each on their own: the level the bias
       * names, from level 0 -- see below) */
      mode = bias >= 0 && !c->divergent ? USSE_SMP_BIAS : USSE_SMP_LOD;
      lodreg = treg(c, scratch_take(c, s));
      move_into(c, lodreg, tex_operand(tex, bias >= 0 ? bias : lod, 0), s);
   } else if (c->divergent) {
      /* A program whose pixels each run on their own: a lookup with its
       * level of detail from the 2x2 block hangs the GPU or reads the wrong
       * level (the block's pixels are not there together) -- level 0
       * (exact for a texture without levels whose filters agree; GLSL
       * leaves the derivatives undefined where pixels part, M33) */
      mode = USSE_SMP_LOD;
      lodreg = constant(c, s, 0.0f);
      mesa_logw_once("sgx: a texture sampled in a program whose branches differ between "
                     "pixels: level 0");
   }
   d = block(c, 4, 4);
   if (c->fs->blend.tex_f16 >> tex->texture_index & 1) {
      /* (M43) a copy of half floats: four F32 back, as they are */
      emit(c, smp(USSE_SMP_F32, USSE_SMP_COORD_F32, treg(c, d), coords,
                  usse_reg(USSE_SA, c->sampler_sa + 4 * c->slot_of_unit[tex->texture_index]),
                  mode, lodreg));
      emit(c, USSE_WDF0);
   } else if (getenv("SGX_TEX_F32")) {
      emit(c, smp(USSE_SMP_F32, USSE_SMP_COORD_F32, treg(c, d),
                         coords,
                         usse_reg(USSE_SA, c->sampler_sa + 4 * c->slot_of_unit[tex->texture_index]),
                         mode, lodreg));
      emit(c, USSE_WDF0);
      /* the sampler hands an 8-bit channel back as its integer value
       * (0..255, checked with gltex): to 0..1 (the copy it reads is always
       * RGBA8) */
      struct usse_reg k = constant(c, s, 1.0f / 255.0f);

      for (unsigned i = 0; i < 4; i++)
         if (i < tex->def.num_components && c->last_use[id + i] >= 0)
            emit(c, usse_fop(USSE_NMAD_MUL, treg(c, d + i),
                             treg(c, d + i), k));
   } else {
      /* the texel as it is, four bytes in one register (the copy it reads
       * is always RGBA8), then two VPCKs to F32 0..1 -- not four F32s
       * scaled by 1/255 one by one (M25) */
      struct usse_reg raw = treg(c, scratch_take(c, s));

      emit(c, smp(USSE_SMP_RAW, USSE_SMP_COORD_F32, raw, coords,
                         usse_reg(USSE_SA, c->sampler_sa + 4 * c->slot_of_unit[tex->texture_index]),
                         mode, lodreg));
      emit(c, USSE_WDF0);
      if (c->fs->blend.tex_depth >> tex->texture_index & 1) {
         /* a depth texture's texel is F32 (M37): d, 0, 0, 1 through the
          * view's swizzle (a shadow lookup's comparison has its own) */
         const uint8_t *swz = c->fs->blend.depth_swizzle[tex->texture_index];
         bool shadow_done = c->fs->blend.tex_shadow >> tex->texture_index & 1;

         for (unsigned i = 0; i < 4; i++) {
            unsigned w = shadow_done ? PIPE_SWIZZLE_X : swz[i];

            if (w == PIPE_SWIZZLE_X)
               emit(c, usse_fmov(treg(c, d + i), raw));
            else
               emit(c, usse_fmov(treg(c, d + i), constant(c, s, w == PIPE_SWIZZLE_W ||
                                                                 w == PIPE_SWIZZLE_1 ? 1.0f : 0.0f)));
         }
      } else {
         emit(c, usse_unpack_unorm8(treg(c, d), raw, 0));
         emit(c, usse_unpack_unorm8(treg(c, d + 2), raw, 2));
      }
   }
   c->smp = true;
out:
   for (unsigned i = 0; i < 4; i++) {
      c->loc[id + i] = treg(c, d + i);
      c->owned[id + i] = i < tex->def.num_components && c->last_use[id + i] >= 0;
      if (!c->owned[id + i])
         give_back(c, d + i);
   }
}

/* a varying the vertex side hands over as it does the generic ones: the
 * colours, fog, texture coordinates -- and the back colours (M37: st's
 * nir_lower_two_sided_color picks them by gl_FrontFacing in the program) */
static bool
colour_or_generic(int slot)
{
   return slot >= VARYING_SLOT_VAR0 ||
          (slot >= VARYING_SLOT_COL0 && slot <= VARYING_SLOT_TEX7) ||
          slot == VARYING_SLOT_BFC0 || slot == VARYING_SLOT_BFC1;
}

/* a fragment shader input's varying slot, -1 if it is not a varying read
 * at a constant offset */
static int
input_slot(nir_intrinsic_instr *in)
{
   nir_src *offset;

   if (in->intrinsic == nir_intrinsic_load_frag_coord)
      return VARYING_SLOT_POS;
   if (in->intrinsic != nir_intrinsic_load_input)
      return -1;
   offset = nir_get_io_offset_src(in);
   if (!nir_src_is_const(*offset))
      return -1;
   return nir_intrinsic_io_semantics(in).location + nir_src_as_uint(*offset);
}

/* A lookup the PDS can make (M35): a 2D texture, the level of detail its
 * own, at x and y of a varying as the PDS iterates it -- what iOS's driver
 * does for them (corpus s00..s04, s11).  Its varying's slot. */
static int
fixed_index(const struct comp *c, const nir_tex_instr *tex)
{
   for (unsigned i = 0; i < c->nfixed; i++)
      if (c->fixed[i] == tex)
         return i;
   return -1;
}

static int
fetch_slot(const struct comp *c, nir_tex_instr *tex)
{
   int coord = nir_tex_instr_src_index(tex, nir_tex_src_coord), slot = -1;

   /* (a rectangle texture's: when the vertex side scales the coordinates) */
   if (tex->op != nir_texop_tex || tex->is_shadow || tex->is_array || tex->num_srcs != 1 ||
       (tex->sampler_dim != GLSL_SAMPLER_DIM_2D &&
        tex->sampler_dim != GLSL_SAMPLER_DIM_EXTERNAL &&
        (tex->sampler_dim != GLSL_SAMPLER_DIM_RECT || fixed_index(c, tex) < 0)) ||
       coord < 0 || tex->coord_components != 2 || tex->texture_index >= MAX_UNITS)
      return -1;
   for (unsigned i = 0; i < 2; i++) {
      nir_scalar x = nir_scalar_chase_movs(nir_get_scalar(tex->src[coord].src.ssa, i));
      nir_intrinsic_instr *in;
      int at;

      if (!nir_def_is_intrinsic(x.def) ||
          (in = nir_def_as_intrinsic(x.def))->intrinsic != nir_intrinsic_load_input)
         return -1;
      at = input_slot(in);
      if (at < 0 || !colour_or_generic(at) ||
          nir_intrinsic_component(in) + x.comp != i || (i && at != slot))
         return -1;
      slot = at;
   }
   return slot;
}

/* ---- coordinates the vertex side makes ready (M39) ------------------------
 *
 * A projective lookup's coordinates (texture2DProj, the fixed function's
 * TXP: x and y times 1 / z or w, NIR's lower_txp) and a rectangle
 * texture's (x and y times 1 / its size, load_texture_scale), at a varying
 * that nothing else reads: the lookup is made at the varying's x and y as
 * they are, the PDS's -- its projective kind divides them -- and the
 * vertex side does the rest to the varying (SGX_FIX_*: sgx_fs.vary_fix).
 * xfwm4's composition, the fixed function's texture2DRect of TEX0 /
 * TEX0.w, is one texel then. */

/* a varying's component, as a load_input's scalar: its slot and which */
static bool
input_component(nir_scalar s, int *slot, unsigned *comp)
{
   nir_intrinsic_instr *in;
   int at;

   if (!nir_scalar_is_intrinsic(s) ||
       (in = nir_def_as_intrinsic(s.def))->intrinsic != nir_intrinsic_load_input ||
       (at = input_slot(in)) < 0 || !colour_or_generic(at) || (*slot >= 0 && at != *slot))
      return false;
   *slot = at;
   *comp = nir_intrinsic_component(in) + s.comp;
   return true;
}

/* coordinate i of tex, a product: of the varying's component i (bit 0 of
 * have), the texture's scale's i (bit 1), 1 / the varying's z or w (bit 2,
 * which in *div) -- each once, nothing else */
static bool
coord_factors(nir_scalar s, unsigned i, const nir_tex_instr *tex, unsigned *have, int *slot,
              unsigned *div, unsigned depth)
{
   unsigned comp;

   s = nir_scalar_chase_movs(s);
   if (depth > 4)
      return false;
   if (nir_scalar_is_alu(s) && nir_scalar_alu_op(s) == nir_op_fmul)
      return coord_factors(nir_scalar_chase_alu_src(s, 0), i, tex, have, slot, div, depth + 1) &&
             coord_factors(nir_scalar_chase_alu_src(s, 1), i, tex, have, slot, div, depth + 1);
   if (nir_scalar_is_alu(s) && nir_scalar_alu_op(s) == nir_op_frcp) {
      if ((*have & 4) ||
          !input_component(nir_scalar_chase_movs(nir_scalar_chase_alu_src(s, 0)), slot, &comp) ||
          comp < 2 || comp > 3 || (*div && *div != comp))
         return false;
      *div = comp;
      *have |= 4;
      return true;
   }
   if (nir_scalar_is_intrinsic(s) &&
       nir_def_as_intrinsic(s.def)->intrinsic == nir_intrinsic_load_texture_scale) {
      nir_intrinsic_instr *in = nir_def_as_intrinsic(s.def);

      if ((*have & 2) || tex->sampler_dim != GLSL_SAMPLER_DIM_RECT ||
          !nir_src_is_const(in->src[0]) || nir_src_as_uint(in->src[0]) != tex->texture_index ||
          s.comp != i)
         return false;
      *have |= 2;
      return true;
   }
   if ((*have & 1) || !input_component(s, slot, &comp) || comp != i)
      return false;
   *have |= 1;
   return true;
}

/* def's every use, through ALU instructions, ends in the coordinates of
 * one of the lookups tex[] */
static bool
feeds_only(nir_def *def, nir_tex_instr *const *tex, unsigned n, unsigned depth)
{
   if (depth > 6)
      return false;
   nir_foreach_use_including_if(src, def) {
      nir_instr *p;
      bool ok = false;

      if (nir_src_is_if(src))
         return false;
      p = nir_src_parent_instr(src);
      if (p->type == nir_instr_type_alu) {
         if (!feeds_only(&nir_instr_as_alu(p)->def, tex, n, depth + 1))
            return false;
         continue;
      }
      for (unsigned k = 0; k < n && !ok; k++)
         ok = p == &tex[k]->instr;
      if (!ok)
         return false;
   }
   return true;
}

static void
lower_fetch_coords(struct comp *c, nir_shader *s, nir_function_impl *impl)
{
   nir_tex_instr *cand[SGX_FRAME_MAX_FETCH];
   int cand_slot[SGX_FRAME_MAX_FETCH];
   uint8_t cand_fix[SGX_FRAME_MAX_FETCH];
   unsigned ncand = 0, nplain = 0;
   bool progress = false;

   if (getenv("SGX_NOFETCH") || getenv("SGX_NO_PROJ_FETCH"))
      return;
   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         nir_tex_instr *tex;
         unsigned have[2] = { 0, 0 }, div = 0, fix;
         int coord, slot = -1;

         if (instr->type != nir_instr_type_tex)
            continue;
         tex = nir_instr_as_tex(instr);
         coord = nir_tex_instr_src_index(tex, nir_tex_src_coord);
         if (tex->op != nir_texop_tex || tex->is_shadow || tex->is_array ||
             tex->num_srcs != 1 || coord < 0 || tex->coord_components != 2 ||
             tex->texture_index >= MAX_UNITS ||
             ((c->fs->blend.tex_depth | c->fs->blend.tex_f16) >> tex->texture_index & 1) ||
             (tex->sampler_dim != GLSL_SAMPLER_DIM_2D &&
              tex->sampler_dim != GLSL_SAMPLER_DIM_EXTERNAL &&
              tex->sampler_dim != GLSL_SAMPLER_DIM_RECT))
            continue;
         for (unsigned i = 0; i < 2; i++)
            if (!coord_factors(nir_get_scalar(tex->src[coord].src.ssa, i), i, tex, &have[i],
                               &slot, &div, 0))
               have[0] = 0;
         if (!(have[0] & 1) || !(have[1] & 1) || ((have[0] ^ have[1]) & 6) ||
             (tex->sampler_dim == GLSL_SAMPLER_DIM_RECT && !(have[0] & 2)) || c->flat[slot])
            continue;
         fix = (have[0] & 2 ? SGX_FIX_SCALE : 0) | (have[0] & 4 ? div : 0);
         if (!fix) {
            nplain++;            /* the PDS's already */
            continue;
         }
         if (ncand == SGX_FRAME_MAX_FETCH)
            continue;
         cand[ncand] = tex;
         cand_slot[ncand] = slot;
         cand_fix[ncand++] = fix;
      }
   }
   for (unsigned k = 0; k < ncand; k++) {
      nir_tex_instr *same[SGX_FRAME_MAX_FETCH];
      nir_intrinsic_instr *first = NULL;
      unsigned n = 0;
      bool ok = cand[k] != NULL && nplain + c->nfixed < SGX_FRAME_MAX_FETCH;

      if (!cand[k])
         continue;
      /* every lookup of the varying the same (and the same texture's
       * scale), nothing else reading it */
      for (unsigned j = k; j < ncand; j++)
         if (cand[j] && cand_slot[j] == cand_slot[k]) {
            ok &= cand_fix[j] == cand_fix[k] &&
                  (!(cand_fix[k] & SGX_FIX_SCALE) ||
                   cand[j]->texture_index == cand[k]->texture_index);
            same[n++] = cand[j];
         }
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            nir_intrinsic_instr *in;

            if (!ok || instr->type != nir_instr_type_intrinsic ||
                (in = nir_instr_as_intrinsic(instr))->intrinsic != nir_intrinsic_load_input ||
                input_slot(in) != cand_slot[k])
               continue;
            ok = feeds_only(&in->def, same, n, 0) && nir_intrinsic_component(in) == 0 &&
                 nir_src_is_const(*nir_get_io_offset_src(in));
            if (!first)
               first = in;
         }
      }
      ok &= first != NULL && c->nfixed + n + nplain <= SGX_FRAME_MAX_FETCH;
      for (unsigned j = k; j < ncand; j++) {
         if (!cand[j] || cand_slot[j] != cand_slot[k])
            continue;
         if (ok) {
            nir_tex_instr *tex = cand[j];
            nir_builder b = nir_builder_at(nir_before_instr(&tex->instr));
            nir_def *xy = nir_load_input(&b, 2, 32,
                                         nir_imm_int(&b, nir_src_as_uint(*nir_get_io_offset_src(first))),
                                         .base = nir_intrinsic_base(first), .component = 0,
                                         .dest_type = nir_intrinsic_dest_type(first),
                                         .io_semantics = nir_intrinsic_io_semantics(first));

            nir_src_rewrite(&tex->src[nir_tex_instr_src_index(tex, nir_tex_src_coord)].src, xy);
            c->fixed[c->nfixed] = tex;
            c->fixed_fix[c->nfixed++] = cand_fix[j];
            progress = true;
         }
         if (j != k)
            cand[j] = NULL;
      }
   }
   if (progress) {
      nir_progress(true, impl, nir_metadata_control_flow);
      NIR_PASS(_, s, nir_opt_dce);
   }
}

static bool
is_fetch(const struct comp *c, const nir_instr *instr)
{
   for (unsigned k = 0; k < c->nfetch; k++)
      if (&c->fetch[k]->instr == instr)
         return true;
   return false;
}

/* Before anything is emitted, a fragment shader's lookups the PDS makes
 * (M35; SGX_NOFETCH=1 none), the varyings only they read, and the inputs
 * (the varyings iterated into the registers) in their order -- the
 * texels go after them */
static void
plan_inputs(struct comp *c, nir_function_impl *impl)
{
   bool read[VARYING_SLOT_MAX] = { false };

   if (!getenv("SGX_NOFETCH")) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            int slot;

            int fix;

            if (instr->type != nir_instr_type_tex || c->nfetch == SGX_FRAME_MAX_FETCH ||
                (slot = fetch_slot(c, nir_instr_as_tex(instr))) < 0 || c->flat[slot] ||
                ((c->fs->blend.tex_depth | c->fs->blend.tex_f16) >>
                 nir_instr_as_tex(instr)->texture_index & 1))
               continue;
            fix = fixed_index(c, nir_instr_as_tex(instr));
            c->fetch_fix[c->nfetch] = fix < 0 ? 0 : c->fixed_fix[fix];
            c->fetch_slot[c->nfetch] = slot;
            c->fetch[c->nfetch++] = nir_instr_as_tex(instr);
         }
      }
   }
   /* the varyings something besides the lookups reads */
   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         nir_intrinsic_instr *in;
         int slot;

         if (instr->type != nir_instr_type_intrinsic ||
             (slot = input_slot(in = nir_instr_as_intrinsic(instr))) < 0)
            continue;
         nir_foreach_use_including_if(src, &in->def)
            read[slot] |= nir_src_is_if(src) || !is_fetch(c, nir_src_parent_instr(src));
      }
   }
   for (unsigned k = 0; k < c->nfetch; k++)
      c->fetch_only[c->fetch_slot[k]] = !read[c->fetch_slot[k]];
   /* (a lookup lower_fetch_coords() made ready is the PDS's, its varying
    * only the lookups') */
   for (unsigned i = 0; i < c->nfixed; i++) {
      bool ok = false;

      for (unsigned k = 0; k < c->nfetch && !ok; k++)
         ok = c->fetch[k] == c->fixed[i] && c->fetch_only[c->fetch_slot[k]];
      if (!ok)
         fail(c, "a lookup made ready for the PDS not its");
   }
   /* the inputs, as intrinsic() would number them */
   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         int slot;

         if (instr->type != nir_instr_type_intrinsic ||
             (slot = input_slot(nir_instr_as_intrinsic(instr))) < 0 ||
             c->fetch_only[slot] || c->input_of_slot[slot] >= 0 ||
             c->colour_of_slot[slot] >= 0 || c->ninputs == SGX_FRAME_MAX_VARYINGS)
            continue;
         /* (two of the TA's colours: flat shading with two-sided colours
          * besides has its back ones smooth) */
         if (c->flat[slot] && c->ncolours < 2) {
            c->colour_slot[c->ncolours] = slot;
            c->colour_of_slot[slot] = c->ncolours++;
            continue;
         }
         c->fs->input_slot[c->ninputs] = slot;
         c->input_of_slot[slot] = c->ninputs++;
      }
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

            if (in->intrinsic == nir_intrinsic_load_ubo) {
               /* (M43) a uniform buffer: its address in sa.  The uniforms
                * are load_uniform's (lower_uniforms_to_ubo is off): block
                * k is the state tracker's constant buffer k + 1 */
               if (!nir_src_is_const(in->src[0]) ||
                   nir_src_as_uint(in->src[0]) + 1 >= SGX_FRAME_MAX_UBOS) {
                  fail(c, "a uniform buffer chosen by an index");
                  return;
               }
               c->ubo_mask |= 1u << (nir_src_as_uint(in->src[0]) + 1);
            } else if (in->intrinsic == nir_intrinsic_load_uniform &&
                !nir_src_is_const(in->src[0])) {
               /* an array read through an index: all of it (M21) */
               at = (nir_intrinsic_base(in) + nir_intrinsic_range(in)) * 4;
               c->uwords = MAX2(c->uwords, at);
            } else if (in->intrinsic == nir_intrinsic_load_uniform && uniform_word(c, in, &at)) {
               c->uwords = MAX2(c->uwords, at + in->def.num_components);
            }
            if (in->intrinsic == nir_intrinsic_terminate ||
                in->intrinsic == nir_intrinsic_terminate_if ||
                in->intrinsic == nir_intrinsic_demote ||
                in->intrinsic == nir_intrinsic_demote_if)
               c->kills = true;
            if (c->vs && in->intrinsic == nir_intrinsic_load_input &&
                nir_src_is_const(*nir_get_io_offset_src(in)))
               c->nattrs = MAX2(c->nattrs, nir_intrinsic_base(in) +
                                           nir_src_as_uint(*nir_get_io_offset_src(in)) + 1);
            if (in->intrinsic == nir_intrinsic_load_texture_scale)
               c->scales = true;
            if (in->intrinsic == nir_intrinsic_load_blend_const_color_r_float ||
                in->intrinsic == nir_intrinsic_load_blend_const_color_g_float ||
                in->intrinsic == nir_intrinsic_load_blend_const_color_b_float ||
                in->intrinsic == nir_intrinsic_load_blend_const_color_a_float)
               c->blend_sa = 0;      /* placed below */
         } else if (instr->type == nir_instr_type_tex) {
            nir_tex_instr *tex = nir_instr_as_tex(instr);

            if (c->vs) {
               fail(c, "texture lookups in a vertex shader");
               return;
            }
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
   /* sa: the uniforms, then a fragment shader's textures' state words (four
    * each, from a multiple of four) and the blend colour, a vertex shader's
    * constant attributes and its clip words -- 128 words at most.  Past that
    * the uniforms are in memory too (M32): the first ones still in sa, the
    * block's address after them */
   {
      unsigned nconst = 0, extra;

      for (unsigned a = 0; c->vs && a < c->nattrs; a++)
         nconst += SGX_ATTR_KIND(c->attr[a]) == SGX_ATTR_CONST;
      extra = c->vs ? 4 * nconst + 4 :
                      (c->nsamplers ? 3 + 4 * c->nsamplers : 0) + (c->blend_sa >= 0 ? 4 : 0) +
                      (c->scales ? 4 + 2 * c->nsamplers : 0);
      extra += util_bitcount(c->ubo_mask);
      c->ubuf = c->uwords + extra > 128;
      if (c->ubuf) {
         c->sa_uniforms = MIN2(c->uwords, (128 - extra - 4) & ~3u);
         c->ubuf_sa = c->sa_uniforms;
         c->nuniforms = c->ubuf_sa + 1;
      } else {
         c->sa_uniforms = c->nuniforms = c->uwords;
      }
      /* (M43) the uniform buffers' addresses */
      u_foreach_bit(k, c->ubo_mask)
         c->ubo_sa[k] = c->nuniforms++;
   }
   c->sampler_sa = align(c->nuniforms, 4);
   c->pa_base = 4 * MAX2(c->nattrs, 1);
   /* a vertex shader's attributes that are constants (M26): four sa words
    * each after the uniforms */
   for (unsigned a = 0; c->vs && a < c->nattrs; a++)
      if (SGX_ATTR_KIND(c->attr[a]) == SGX_ATTR_CONST) {
         c->vs->attr_sa[a] = c->nuniforms;
         c->nuniforms += 4;
      }
   if (c->scales)
      c->scale_sa = c->sampler_sa + 4 * c->nsamplers;
   if (c->blend_sa >= 0)
      c->blend_sa = c->scales ? align(c->scale_sa + 2 * c->nsamplers, 4) :
                                c->sampler_sa + 4 * c->nsamplers;
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
   /* vec4 slots (the screen does not pack uniforms) */
   if (!nir_src_is_const(in->src[0])) {
      fail(c, "an indirect uniform");
      return false;
   }
   *at = (nir_intrinsic_base(in) + nir_src_as_uint(in->src[0])) * 4;
   return true;
}

/* A uniform array's element through an index (M21): index1 = its first
 * sa word -- 4 x the offset (an integer as a float here) + the array's base,
 * made an integer in the low bits of 2^23 + it -- then each word read
 * through index1 by a 32-bit move, as iOS's c04_loop_break reads its
 * array */
static void
indirect_uniform(struct comp *c, nir_intrinsic_instr *in)
{
   unsigned id = in->def.index * 4, base = nir_intrinsic_base(in) * 4;
   struct operand off = resolve(nir_get_scalar(in->src[0].ssa, 0));
   struct scratch s = { 0 };
   struct usse_reg t = treg(c, scratch_take(c, &s));

   emit(c, usse_fmad(t, get(c, off, TAKES_ABS | TAKES_LANE_Y, &s), constant(c, &s, 4.0f),
                     constant(c, &s, 8388608.0f + base)));
   emit(c, usse_vbw_and(usse_reg(USSE_INDEX, 1), t, 0xffff));
   for (unsigned i = 0; i < in->def.num_components; i++) {
      unsigned r;

      if (c->last_use[id + i] < 0)
         continue;
      r = take(c);
      c->loc[id + i] = treg(c, r);
      c->owned[id + i] = true;
      emit(c, usse_vbw_or(treg(c, r), usse_reg(USSE_IDX1, 3 << 5 | i), 0));
   }
   scratch_give_back(c, &s);
}

/* Uniform words from memory (M32): VLDST of the value's words from word
 * at of the block -- through an index, at + 4 x the offset: made an integer
 * as indirect_uniform() makes it, with 4 in the high half (4 bytes a word)
 * -- into temporaries in a row, then WDF0 */
static void
memory_uniform(struct comp *c, nir_intrinsic_instr *in, unsigned at, bool indexed)
{
   unsigned id = in->def.index * 4, n = in->def.num_components, r = block(c, n, 1);
   struct scratch s = { 0 };

   if (indexed) {
      struct operand off = resolve(nir_get_scalar(in->src[0].ssa, 0));
      struct usse_reg t = treg(c, scratch_take(c, &s));

      emit(c, usse_fmad(t, get(c, off, TAKES_ABS | TAKES_LANE_Y, &s), constant(c, &s, 4.0f),
                        constant(c, &s, 8388608.0f + at)));
      emit(c, usse_vbw_and(t, t, 0xffff));
      emit(c, usse_vbw_or_rot(t, t, 4, 16));
      emit(c, usse_ldr_reg(treg(c, r), c->ubuf_sa, t, n));
   } else if (at < 128) {
      emit(c, usse_ldr_imm(treg(c, r), c->ubuf_sa, at, n));
   } else {
      struct usse_reg t = treg(c, scratch_take(c, &s));

      emit(c, usse_limm(t, 4u << 16 | at));
      emit(c, usse_ldr_reg(treg(c, r), c->ubuf_sa, t, n));
   }
   emit(c, USSE_WDF0);
   scratch_give_back(c, &s);
   for (unsigned i = 0; i < n; i++) {
      c->loc[id + i] = treg(c, r + i);
      c->owned[id + i] = c->last_use[id + i] >= 0;
      if (!c->owned[id + i])
         give_back(c, r + i);
   }
}

/* (M43) a uniform buffer's words (ARB_uniform_buffer_object): VLDST from
 * the buffer's address in sa word ubo_sa[k], at the offset in bytes --
 * a constant, or the program's (a float, the integers being floats here:
 * a quarter of it made an integer as memory_uniform() makes one) */
static void
ubo_load(struct comp *c, nir_intrinsic_instr *in, unsigned k)
{
   unsigned id = in->def.index * 4, n = in->def.num_components, r = block(c, n, 1);
   unsigned sa = c->ubo_sa[k];
   struct scratch s = { 0 };

   if (nir_src_is_const(in->src[1])) {
      /* (an offset made a float with the integers -- 16 bytes 0x41800000
       * -- or one made after them, an integer: a multiple of 4 under
       * 2^24, never as large as 1.0's bits) */
      uint32_t bits = nir_src_as_uint(in->src[1]);
      unsigned at = (bits >= 0x3f800000u ? (unsigned)nir_src_as_float(in->src[1]) : bits) / 4;

      if (at < 128) {
         emit(c, usse_ldr_imm(treg(c, r), sa, at, n));
      } else {
         struct usse_reg t = treg(c, scratch_take(c, &s));

         emit(c, usse_limm(t, 4u << 16 | at));
         emit(c, usse_ldr_reg(treg(c, r), sa, t, n));
      }
   } else {
      struct operand off = resolve(nir_get_scalar(in->src[1].ssa, 0));
      struct usse_reg t = treg(c, scratch_take(c, &s));

      emit(c, usse_fmad(t, get(c, off, TAKES_ABS | TAKES_LANE_Y, &s), constant(c, &s, 0.25f),
                        constant(c, &s, 8388608.0f)));
      emit(c, usse_vbw_and(t, t, 0xffff));
      emit(c, usse_vbw_or_rot(t, t, 4, 16));
      emit(c, usse_ldr_reg(treg(c, r), sa, t, n));
   }
   emit(c, USSE_WDF0);
   scratch_give_back(c, &s);
   for (unsigned i = 0; i < n; i++) {
      c->loc[id + i] = treg(c, r + i);
      c->owned[id + i] = c->last_use[id + i] >= 0;
      if (!c->owned[id + i])
         give_back(c, r + i);
   }
}

static void tile_read(struct comp *c, unsigned d);

static void
intrinsic(struct comp *c, nir_intrinsic_instr *in)
{
   nir_src *offset;
   unsigned at, id = in->def.index * 4;

   switch (in->intrinsic) {
   case nir_intrinsic_load_frag_coord:
   case nir_intrinsic_load_input: {
      unsigned slot, comp = 0;

      if (c->vs) {
         /* attribute n (its vertex element) where the fetch put it -- four
          * words from pa4n, vs_prologue() made floats -- or, a constant,
          * in sa */
         offset = nir_get_io_offset_src(in);
         if (in->intrinsic != nir_intrinsic_load_input || !nir_src_is_const(*offset)) {
            fail(c, "an indirect attribute");
            return;
         }
         at = nir_intrinsic_base(in) + nir_src_as_uint(*offset);
         if (at >= SGX_VS_MAX_ATTRIBS) {
            fail(c, "attribute %u", at);
            return;
         }
         if (at >= c->nattrs) {
            fail(c, "attribute %u read but not found", at);
            return;
         }
         for (unsigned i = 0; i < in->def.num_components; i++)
            c->loc[id + i] = SGX_ATTR_KIND(c->attr[at]) == SGX_ATTR_CONST ?
                             usse_reg(USSE_SA, c->vs->attr_sa[at] + nir_intrinsic_component(in) + i) :
                             usse_reg(USSE_PA, 4 * at + nir_intrinsic_component(in) + i);
         return;
      }
      if (in->intrinsic == nir_intrinsic_load_frag_coord) {
         /* the PDS iterates the pixel's position like a varying */
         slot = VARYING_SLOT_POS;
      } else {
         offset = nir_get_io_offset_src(in);
         if (!nir_src_is_const(*offset)) {
            fail(c, "an indirect varying");
            return;
         }
         slot = nir_intrinsic_io_semantics(in).location + nir_src_as_uint(*offset);
         comp = nir_intrinsic_component(in);
         if (!colour_or_generic(slot)) {
            fail(c, "input %s", gl_varying_slot_name_for_stage(slot, MESA_SHADER_FRAGMENT));
            return;
         }
      }
      if (c->fetch_only[slot]) {
         /* read by the PDS's lookups only (M35): not in the registers */
         for (unsigned i = 0; i < in->def.num_components; i++)
            c->loc[id + i] = usse_reg(USSE_PA, 0);
         return;
      }
      if (c->colour_of_slot[slot] >= 0) {
         /* one of the TA's colours (M37): its bytes, unpacked as o0's are
          * for blending */
         struct usse_reg raw = usse_reg(USSE_PA, 4 * c->ninputs + c->colour_of_slot[slot]);
         unsigned d = block(c, 4, 4);

         emit(c, usse_unpack_unorm8(treg(c, d), raw, 0));
         emit(c, usse_unpack_unorm8(treg(c, d + 2), raw, 2));
         for (unsigned i = 0; i < 4; i++) {
            bool mine = i >= comp && i < comp + in->def.num_components;

            if (mine) {
               c->loc[id + i - comp] = treg(c, d + i);
               c->owned[id + i - comp] = c->last_use[id + i - comp] >= 0;
            }
            if (!mine || c->last_use[id + i - comp] < 0)
               give_back(c, d + i);
         }
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
      at = 4 * c->input_of_slot[slot] + comp;
      for (unsigned i = 0; i < in->def.num_components; i++)
         c->loc[id + i] = usse_reg(USSE_PA, at + i);
      return;
   }
   case nir_intrinsic_load_uniform:
      if (!nir_src_is_const(in->src[0])) {
         /* (in sa when all of the array is) */
         if (c->ubuf && (nir_intrinsic_base(in) + nir_intrinsic_range(in)) * 4 > c->sa_uniforms)
            memory_uniform(c, in, nir_intrinsic_base(in) * 4, true);
         else
            indirect_uniform(c, in);
         return;
      }
      if (!uniform_word(c, in, &at))
         return;
      if (at + in->def.num_components > c->sa_uniforms) {
         memory_uniform(c, in, at, false);
         return;
      }
      for (unsigned i = 0; i < in->def.num_components; i++)
         c->loc[id + i] = usse_reg(USSE_SA, at + i);
      return;
   case nir_intrinsic_load_ubo:
      /* (M43: block k, constant buffer k + 1 -- scan()) */
      ubo_load(c, in, nir_src_as_uint(in->src[0]) + 1);
      return;
   case nir_intrinsic_ddx:
   case nir_intrinsic_ddx_fine:
   case nir_intrinsic_ddx_coarse:
   case nir_intrinsic_ddy:
   case nir_intrinsic_ddy_fine:
   case nir_intrinsic_ddy_coarse: {
      /* the difference across the pixel's 2x2 block (M33; scalar: the
       * screen's scalarize_ddx) */
      struct scratch sc = { 0 };
      struct usse_reg a = get(c, resolve(nir_get_scalar(in->src[0].ssa, 0)),
                              TAKES_LANE_Y | TAKES_SA, &sc);
      bool x = in->intrinsic == nir_intrinsic_ddx || in->intrinsic == nir_intrinsic_ddx_fine ||
               in->intrinsic == nir_intrinsic_ddx_coarse;

      emit(c, usse_fop(x ? USSE_NMAD_DSX : USSE_NMAD_DSY, define(c, &in->def), a, a));
      scratch_give_back(c, &sc);
      c->derivatives = true;
      return;
   }
   case nir_intrinsic_decl_reg:
      /* a phi web's register (M20): a temporary a component, the whole
       * program long */
      if (nir_intrinsic_num_array_elems(in) || nir_intrinsic_bit_size(in) != 32) {
         fail(c, "an array register");
         return;
      }
      for (unsigned i = 0; i < nir_intrinsic_num_components(in); i++)
         c->loc[id + i] = treg(c, take(c));
      return;
   case nir_intrinsic_store_reg: {
      unsigned reg = in->src[1].ssa->index * 4, mask = nir_intrinsic_write_mask(in);
      struct scratch sc = { 0 };

      for (unsigned i = 0; i < in->src[0].ssa->num_components; i++) {
         if (mask & 1 << i)
            move_into(c, c->loc[reg + i], resolve(nir_get_scalar(in->src[0].ssa, i)), &sc);
         scratch_give_back(c, &sc);
      }
      return;
   }
   case nir_intrinsic_load_reg: {
      /* a copy: the register changes while the value may still be read */
      unsigned reg = in->src[0].ssa->index * 4;

      for (unsigned i = 0; i < in->def.num_components; i++) {
         unsigned r;

         if (c->last_use[id + i] < 0)
            continue;
         r = take(c);
         c->loc[id + i] = treg(c, r);
         c->owned[id + i] = true;
         emit(c, usse_fmov(treg(c, r), c->loc[reg + i]));
      }
      return;
   }
   case nir_intrinsic_store_output: {
      unsigned loc = nir_intrinsic_io_semantics(in).location;
      unsigned mask = nir_intrinsic_write_mask(in), comp = nir_intrinsic_component(in);

      if (c->depth) {
         fail(c, "an output written in control flow");
         return;
      }

      if (c->vs) {
         int k = -1;

         /* the position, or a varying the fragment shader reads; the rest
          * (the point size, varyings nobody reads) go nowhere */
         loc += nir_src_as_uint(*nir_get_io_offset_src(in));
         if (loc == VARYING_SLOT_POS)
            k = 0;
         for (unsigned j = 0; j < c->nvaryings && k < 0; j++)
            if (c->varying_slot[j] == loc)
               k = 1 + j;
         for (unsigned i = 0; k >= 0 && i < in->src[0].ssa->num_components; i++) {
            struct operand o;

            if (!(mask & 1 << i) || comp + i >= 4)
               continue;
            o = resolve(nir_get_scalar(in->src[0].ssa, i));
            c->have_vout[k][comp + i] = true;
            /* (the position, and a varying the vertex side does more to:
             * at the end, vertex_output()) */
            if (k == 0 || (c->vary_fix && c->vary_fix[k - 1])) {
               c->vout[k][comp + i] = o;
            } else {
               /* a varying straight into its output register: kept until
                * the end, eight vec4s took more temporaries than there
                * are (M30) */
               struct scratch sc = { 0 };

               move_into(c, usse_reg(USSE_OUTPUT, 4 * k + comp + i), o, &sc);
               scratch_give_back(c, &sc);
            }
         }
         return;
      }
      /* (M43) the second colour of dual-source blending: nir_lower_blend
       * took it into the blending; with no blending, nothing */
      if (nir_intrinsic_io_semantics(in).dual_source_blend_index)
         return;
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
            src = treg(c, k);
            emit(c, usse_limm(src, strtoul(dbg, NULL, 0)));
         }
         if (c->fs->blend.rt_kind != SGX_RT_UNORM8 && !dbg) {
            tile_read(c, d);
         } else {
            emit(c, usse_unpack_unorm8(treg(c, d), src, 0));
            emit(c, usse_unpack_unorm8(treg(c, d + 2), src, 2));
         }
         for (unsigned i = 0; i < 4; i++) {
            c->loc[id + i] = treg(c, d + i);
            c->owned[id + i] = i < in->def.num_components && c->last_use[id + i] >= 0;
            if (!c->owned[id + i])
               give_back(c, d + i);
         }
         if (dbg)
            give_back(c, k);
      }
      return;
   }
   case nir_intrinsic_load_texture_scale: {
      unsigned unit = nir_src_is_const(in->src[0]) ? nir_src_as_uint(in->src[0]) : MAX_UNITS;

      if (unit >= MAX_UNITS || c->slot_of_unit[unit] < 0 || c->scale_sa < 0) {
         fail(c, "a texture's size for a texture not sampled");
         return;
      }
      c->loc[id] = usse_reg(USSE_SA, c->scale_sa + 2 * c->slot_of_unit[unit]);
      c->loc[id + 1] = usse_reg(USSE_SA, c->scale_sa + 2 * c->slot_of_unit[unit] + 1);
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
   case nir_intrinsic_terminate:
   case nir_intrinsic_terminate_if:
   case nir_intrinsic_demote:
   case nir_intrinsic_demote_if: {
      /* discard: the conditions gathered (their largest) into a temporary of
       * its own (two: output() copies it to the odd one), which output()
       * tests */
      bool cond = in->intrinsic == nir_intrinsic_terminate_if ||
                  in->intrinsic == nir_intrinsic_demote_if;
      struct operand o = cond ? resolve(nir_get_scalar(in->src[0].ssa, 0)) :
                                (struct operand){ .is_const = true, .c = 1.0f };
      struct scratch sc = { 0 };

      if (!c->fs || c->kill < 0) {
         fail(c, "discard in a vertex shader");
         break;
      }
      /* (kill is 0 from the program's start: a discard in a branch may
       * not run) */
      if (o.is_const)
         emit(c, usse_limm(treg(c, c->kill), f32_bits(o.c ? 1.0f : 0.0f)));
      else
         emit(c, usse_fop(USSE_NMAD_MAX, treg(c, c->kill), treg(c, c->kill),
                          get(c, o, TAKES_ALL & ~TAKES_NEG, &sc)));
      scratch_give_back(c, &sc);
      break;
   }
   case nir_intrinsic_load_front_face_fsign: {
      /* +1 front, -1 back, by the facing bit (iOS's v09_frontfacing) */
      struct usse_reg d = define(c, &in->def);

      emit(c, usse_limm(d, f32_bits(c->front_when_set ? -1.0f : 1.0f)));
      emit(c, usse_vtst_facing(0));
      emit(c, usse_limm_pred(d, f32_bits(c->front_when_set ? 1.0f : -1.0f), 1));
      break;
   }
   case nir_intrinsic_load_barycentric_pixel:
   case nir_intrinsic_load_barycentric_centroid:
   case nir_intrinsic_load_barycentric_sample:
      return;
   default:
      fail(c, "no %s yet", nir_intrinsic_infos[in->intrinsic].name);
      return;
   }
}

static bool
is_derivative(const nir_intrinsic_instr *in)
{
   switch (in->intrinsic) {
   case nir_intrinsic_ddx:
   case nir_intrinsic_ddx_fine:
   case nir_intrinsic_ddx_coarse:
   case nir_intrinsic_ddy:
   case nir_intrinsic_ddy_fine:
   case nir_intrinsic_ddy_coarse:
      return true;
   default:
      return false;
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
      if (in->intrinsic == nir_intrinsic_terminate_if ||
          in->intrinsic == nir_intrinsic_demote_if || is_derivative(in))
         f(c, resolve(nir_get_scalar(in->src[0].ssa, 0)), index);
      if (in->intrinsic == nir_intrinsic_store_reg)
         for (unsigned i = 0; i < in->src[0].ssa->num_components; i++)
            f(c, resolve(nir_get_scalar(in->src[0].ssa, i)), index);
      if (in->intrinsic == nir_intrinsic_load_uniform && !nir_src_is_const(in->src[0]))
         f(c, resolve(nir_get_scalar(in->src[0].ssa, 0)), index);
   } else if (instr->type == nir_instr_type_tex) {
      nir_tex_instr *tex = nir_instr_as_tex(instr);

      for (unsigned i = 0; i < tex->num_srcs; i++) {
         if (tex->src[i].src_type == nir_tex_src_coord) {
            for (unsigned k = 0; k < MIN2(tex->src[i].src.ssa->num_components, 3); k++)
               f(c, tex_operand(tex, i, k), index);
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
   if (o.is_const)
      return;
   /* read in a loop it was made before: alive until that loop's end (the
    * outermost such), every iteration reads it */
   for (unsigned i = 0; i < c->nactive; i++)
      if (c->active[i].start > c->def_at[o.id / 4]) {
         index = MAX2(index, c->active[i].end);
         break;
      }
   c->last_use[o.id] = MAX2(c->last_use[o.id], index);
}

static void
release(struct comp *c, struct operand o, int index)
{
   /* the colour's values stay until output() has moved them */
   if (c->have_colour[0] || c->have_colour[1] || c->have_colour[2] || c->have_colour[3])
      for (unsigned i = 0; i < 4; i++)
         if (c->have_colour[i] && !c->colour[i].is_const && c->colour[i].id == o.id)
            return;
   /* and the position's until vertex_output() has, and those of the
    * varyings the vertex side does more to (M39) */
   for (unsigned k = 0; c->vs && k <= c->nvaryings; k++) {
      if (k && !(c->vary_fix && c->vary_fix[k - 1]))
         continue;
      for (unsigned i = 0; i < 4; i++)
         if (c->have_vout[k][i] && !c->vout[k][i].is_const && c->vout[k][i].id == o.id)
            return;
   }
   if (!o.is_const && c->last_use[o.id] == index && c->owned[o.id]) {
      give_back(c, c->loc[o.id].num - (c->vs ? c->pa_base : 0));
      c->owned[o.id] = false;
   }
}

/* GL's logic op (M37) as iOS's driver does it (es1 e03..e10): the colour
 * packed to bytes, then a VBW with the tile's (o0) -- `xor o0, pa0, o0`;
 * not x is `and c56, ~x`, c56 all ones.  The colour mask after it, on the
 * bytes (o0 is B G R A, as the target), and a discarded pixel keeps o0:
 * both through a temporary, the result moved or unpacked at the end. */
static void
logic_output(struct comp *c, unsigned base)
{
   const unsigned mask = c->fs->blend.colormask;
   const bool direct = c->kill < 0 && mask == 0xf;
   struct usse_reg src = treg(c, take(c)), t = treg(c, take(c));
   struct usse_reg o0 = usse_reg(USSE_OUTPUT, 0), ones = USSE_ALL_ONES;
   struct usse_reg r = direct ? o0 : t;
   uint64_t w;

   emit(c, usse_pack_unorm8_to(src, treg(c, base)));
   switch (c->logicop) {
   case PIPE_LOGICOP_CLEAR:         w = usse_vbw(USSE_VBW_XOR, r, o0, o0, false); break;
   case PIPE_LOGICOP_NOR:
      emit(c, usse_vbw(USSE_VBW_OR, t, src, o0, false));
      w = usse_vbw(USSE_VBW_AND, r, ones, t, true);
      break;
   case PIPE_LOGICOP_AND_INVERTED:  w = usse_vbw(USSE_VBW_AND, r, o0, src, true); break;
   case PIPE_LOGICOP_COPY_INVERTED: w = usse_vbw(USSE_VBW_AND, r, ones, src, true); break;
   case PIPE_LOGICOP_AND_REVERSE:   w = usse_vbw(USSE_VBW_AND, r, src, o0, true); break;
   case PIPE_LOGICOP_INVERT:        w = usse_vbw(USSE_VBW_AND, r, ones, o0, true); break;
   case PIPE_LOGICOP_XOR:           w = usse_vbw(USSE_VBW_XOR, r, src, o0, false); break;
   case PIPE_LOGICOP_NAND:
      emit(c, usse_vbw(USSE_VBW_AND, t, src, o0, false));
      w = usse_vbw(USSE_VBW_AND, r, ones, t, true);
      break;
   case PIPE_LOGICOP_AND:           w = usse_vbw(USSE_VBW_AND, r, src, o0, false); break;
   case PIPE_LOGICOP_EQUIV:
      emit(c, usse_vbw(USSE_VBW_XOR, t, src, o0, false));
      w = usse_vbw(USSE_VBW_AND, r, ones, t, true);
      break;
   case PIPE_LOGICOP_NOOP:          w = usse_vbw(USSE_VBW_OR, r, o0, o0, false); break;
   case PIPE_LOGICOP_OR_INVERTED:   w = usse_vbw(USSE_VBW_OR, r, o0, src, true); break;
   case PIPE_LOGICOP_COPY:          w = usse_vbw(USSE_VBW_OR, r, src, src, false); break;
   case PIPE_LOGICOP_OR_REVERSE:    w = usse_vbw(USSE_VBW_OR, r, src, o0, true); break;
   case PIPE_LOGICOP_OR:            w = usse_vbw(USSE_VBW_OR, r, src, o0, false); break;
   default:                         w = usse_vbw(USSE_VBW_OR, r, o0, o0, true); break;  /* SET */
   }
   if (direct) {
      emit(c, w | USSE_END);
      return;
   }
   emit(c, w);
   if (mask != 0xf) {
      /* t = (t & m) | (o0 & ~m), m the bytes of the channels written */
      struct usse_reg m = treg(c, take(c));
      uint32_t bytes = (mask & PIPE_MASK_R ? 0x00ff0000u : 0) |
                       (mask & PIPE_MASK_G ? 0x0000ff00u : 0) |
                       (mask & PIPE_MASK_B ? 0x000000ffu : 0) |
                       (mask & PIPE_MASK_A ? 0xff000000u : 0);

      emit(c, usse_limm(m, bytes));
      emit(c, usse_vbw(USSE_VBW_AND, t, t, m, false));
      emit(c, usse_vbw(USSE_VBW_AND, src, o0, m, true));
      emit(c, usse_vbw(USSE_VBW_OR, t, t, src, false));
   }
   if (c->kill < 0) {
      emit(c, usse_vbw_or(o0, t, 0) | USSE_END);
      return;
   }
   /* discarded: o0 as it was -- the result and the tile's unpacked, the
    * channels chosen, packed again (exact for bytes) */
   {
      unsigned d = block(c, 4, 2);

      emit(c, usse_fmov(treg(c, c->kill + 1), treg(c, c->kill)));
      emit(c, usse_unpack_unorm8(treg(c, base), t, 0));
      emit(c, usse_unpack_unorm8(treg(c, base + 2), t, 2));
      emit(c, usse_unpack_unorm8(treg(c, d), o0, 0));
      emit(c, usse_unpack_unorm8(treg(c, d + 2), o0, 2));
      for (unsigned i = 0; i < 4; i++) {
         usse_fmovc(&w, USSE_TEST_NE0, treg(c, base + i), treg(c, c->kill + (i & 1)),
                    treg(c, d + i), treg(c, base + i));
         emit(c, w);
      }
      emit(c, usse_pack_unorm8(0, treg(c, base)) | USSE_END);
   }
}

/* the colour: four registers in a row, packed into o0 */
/* (M43) the colour the tile holds, R G B A as F32 into d..d + 3 (d even),
 * for a target not of RGBA bytes: half floats (o0, o1 or o0 alone), F32
 * (o0, o1 or o0 alone); the channels it has not 0, alpha 1 */
static void
tile_read(struct comp *c, unsigned d)
{
   struct usse_reg o0 = usse_reg(USSE_OUTPUT, 0);

   switch (c->fs->blend.rt_kind) {
   case SGX_RT_F16X4:
      emit(c, usse_unpack_f16(treg(c, d), o0, 0));
      emit(c, usse_unpack_f16(treg(c, d + 2), o0, 2));
      return;
   case SGX_RT_F16X2:
      emit(c, usse_unpack_f16(treg(c, d), o0, 0));
      break;
   case SGX_RT_F32:
      emit(c, usse_fmov(treg(c, d), o0));
      emit(c, usse_limm(treg(c, d + 1), 0));
      break;
   default:
      emit(c, usse_fmov(treg(c, d), o0));
      emit(c, usse_fmov(treg(c, d + 1), usse_reg(USSE_OUTPUT, 1)));
      break;
   }
   emit(c, usse_limm(treg(c, d + 2), 0));
   emit(c, usse_limm(treg(c, d + 3), 0x3f800000));
}

static void
output(struct comp *c)
{
   struct scratch s = { 0 };
   unsigned base;

   if (!c->have_colour[0] && !c->have_colour[1] && !c->have_colour[2] && !c->have_colour[3]) {
      /* a colour mask of nothing (nir_lower_blend took the output away),
       * or a shader that writes no colour (depth alone, M35): o0 keeps the
       * tile's colour, as it came in (M23) */
      emit(c, USSE_NOP_END);
      return;
   }
   base = c->out_base >= 0 ? (unsigned)c->out_base : block(c, 4, 2);
   for (unsigned i = 0; i < 4; i++) {
      struct operand o = c->colour[i];

      if (!c->have_colour[i]) {
         o.is_const = true;
         o.c = i == 3 ? 1.0f : 0.0f;
      }
      /* (made where it goes already) */
      if (!o.is_const && !o.neg && !o.abs && c->colour_id[i] == (int)o.id &&
          c->loc[o.id].bank == USSE_TEMP && c->loc[o.id].num == treg(c, base + i).num)
         continue;
      move_into(c, treg(c, base + i), o, &s);
      scratch_give_back(c, &s);
   }
   if (c->logicop >= 0) {
      logic_output(c, base);
      return;
   }
   if (c->kill >= 0) {
      /* discarded: the pixel keeps the colour its tile holds (o0, as the
       * blending reads it) -- every draw is a translucent object, shaded in
       * order.  The hardware's own way, iOS's c00_discard (a punch-through
       * pass, two phases, `p1? KILL`), does not work here yet (M19). */
      unsigned d = block(c, 4, 2);

      emit(c, usse_fmov(treg(c, c->kill + 1), treg(c, c->kill)));
      if (c->fs->blend.rt_kind != SGX_RT_UNORM8) {
         tile_read(c, d);
      } else {
         emit(c, usse_unpack_unorm8(treg(c, d), usse_reg(USSE_OUTPUT, 0), 0));
         emit(c, usse_unpack_unorm8(treg(c, d + 2), usse_reg(USSE_OUTPUT, 0), 2));
      }
      for (unsigned i = 0; i < 4; i++) {
         uint64_t w;

         usse_fmovc(&w, USSE_TEST_NE0, treg(c, base + i), treg(c, c->kill + (i & 1)),
                    treg(c, d + i), treg(c, base + i));
         emit(c, w);
      }
   }
   if (c->sop2) {
      struct usse_reg packed = treg(c, take(c)), o0 = usse_reg(USSE_OUTPUT, 0);

      emit(c, usse_pack_unorm8_to(packed, treg(c, base)));
      emit(c, usse_sop2(o0, c->sop2_swap ? o0 : packed, c->sop2_swap ? packed : o0,
                        &c->sop2_factors) | USSE_END);
      return;
   }
   switch (c->fs->blend.rt_kind) {
   case SGX_RT_F16X4:
      emit(c, usse_pack_f16(usse_reg(USSE_OUTPUT, 0), treg(c, base), 0xf) | USSE_END);
      break;
   case SGX_RT_F16X2:
      emit(c, usse_pack_f16(usse_reg(USSE_OUTPUT, 0), treg(c, base), 0x3) | USSE_END);
      break;
   case SGX_RT_F32:
      emit(c, usse_fmov(usse_reg(USSE_OUTPUT, 0), treg(c, base)) | USSE_END);
      break;
   case SGX_RT_F32X2:
      emit(c, usse_pack_f32x2(usse_reg(USSE_OUTPUT, 0), treg(c, base)) | USSE_END);
      break;
   default:
      emit(c, usse_pack_unorm8(0, treg(c, base)) | USSE_END);
   }
}

/* A blend factor as SOP2 takes it, the source being SOP2's first operand
 * (src1, swapped: its second); false: not one it has (the constant's) */
static bool
sop2_colour_factor(unsigned f, bool swapped, uint8_t *sel, uint8_t *mod)
{
   unsigned src = swapped ? USSE_SOP2_SRC2 : USSE_SOP2_SRC1;
   unsigned dst = swapped ? USSE_SOP2_SRC1 : USSE_SOP2_SRC2;

   *mod = f >= PIPE_BLENDFACTOR_ZERO;
   switch (f) {
   case PIPE_BLENDFACTOR_ONE: *sel = USSE_SOP2_ZERO; *mod = 1; return true;
   case PIPE_BLENDFACTOR_ZERO: *sel = USSE_SOP2_ZERO; *mod = 0; return true;
   case PIPE_BLENDFACTOR_SRC_COLOR: case PIPE_BLENDFACTOR_INV_SRC_COLOR:
      *sel = src;
      return true;
   case PIPE_BLENDFACTOR_SRC_ALPHA: case PIPE_BLENDFACTOR_INV_SRC_ALPHA:
      *sel = src + 2;
      return true;
   case PIPE_BLENDFACTOR_DST_COLOR: case PIPE_BLENDFACTOR_INV_DST_COLOR:
      *sel = dst;
      return true;
   case PIPE_BLENDFACTOR_DST_ALPHA: case PIPE_BLENDFACTOR_INV_DST_ALPHA:
      *sel = dst + 2;
      return true;
   default:
      return false;
   }
}

static bool
sop2_alpha_factor(unsigned f, bool swapped, uint8_t *sel, uint8_t *mod)
{
   unsigned src = swapped ? USSE_SOP2_A_SRC2 : USSE_SOP2_A_SRC1;
   unsigned dst = swapped ? USSE_SOP2_A_SRC1 : USSE_SOP2_A_SRC2;

   *mod = f >= PIPE_BLENDFACTOR_ZERO;
   switch (f) {
   case PIPE_BLENDFACTOR_ONE: case PIPE_BLENDFACTOR_SRC_ALPHA_SATURATE:
      *sel = USSE_SOP2_A_ZERO; *mod = 1; return true;
   case PIPE_BLENDFACTOR_ZERO: *sel = USSE_SOP2_A_ZERO; *mod = 0; return true;
   case PIPE_BLENDFACTOR_SRC_COLOR: case PIPE_BLENDFACTOR_INV_SRC_COLOR:
   case PIPE_BLENDFACTOR_SRC_ALPHA: case PIPE_BLENDFACTOR_INV_SRC_ALPHA:
      *sel = src;
      return true;
   case PIPE_BLENDFACTOR_DST_COLOR: case PIPE_BLENDFACTOR_INV_DST_COLOR:
   case PIPE_BLENDFACTOR_DST_ALPHA: case PIPE_BLENDFACTOR_INV_DST_ALPHA:
      *sel = dst;
      return true;
   default:
      return false;
   }
}

/* GL's blending as one SOP2 (M25): the colour and the tile's, 8 bits a
 * channel, as iOS blends -- not F32 arithmetic on the tile's colour
 * unpacked (nir_lower_blend), some 30 instructions more.  Not for the
 * blend colour's factors, a colour mask, or min/max with the other
 * channels' funcs not the same kind. */
static bool
sop2_blend(const struct sgx_blend_key *k, struct usse_sop2 *f, bool *swap)
{
   static const uint8_t op[] = {
      [PIPE_BLEND_ADD] = USSE_SOP2_ADD, [PIPE_BLEND_SUBTRACT] = USSE_SOP2_SUB,
      [PIPE_BLEND_REVERSE_SUBTRACT] = USSE_SOP2_SUB, [PIPE_BLEND_MIN] = USSE_SOP2_MIN,
      [PIPE_BLEND_MAX] = USSE_SOP2_MAX,
   };
   bool rev = k->rgb_func == PIPE_BLEND_REVERSE_SUBTRACT;
   unsigned rgb_src = k->rgb_src, rgb_dst = k->rgb_dst;
   unsigned alpha_src = k->alpha_src, alpha_dst = k->alpha_dst;

   if (!k->enable || k->colormask != 0xf || k->rgb_func > PIPE_BLEND_MAX ||
       k->alpha_func > PIPE_BLEND_MAX ||
       (k->alpha_func == PIPE_BLEND_REVERSE_SUBTRACT) != rev ||
       (rev && (rgb_src == PIPE_BLENDFACTOR_SRC_ALPHA_SATURATE)))
      return false;
   /* min and max ignore the factors */
   if (k->rgb_func == PIPE_BLEND_MIN || k->rgb_func == PIPE_BLEND_MAX)
      rgb_src = rgb_dst = PIPE_BLENDFACTOR_ONE;
   if (k->alpha_func == PIPE_BLEND_MIN || k->alpha_func == PIPE_BLEND_MAX)
      alpha_src = alpha_dst = PIPE_BLENDFACTOR_ONE;
   *swap = rev;
   memset(f, 0, sizeof(*f));
   f->cop = op[k->rgb_func];
   f->aop = op[k->alpha_func];
   if (rgb_src == PIPE_BLENDFACTOR_SRC_ALPHA_SATURATE)
      f->csel1 = USSE_SOP2_SRC_ALPHA_SAT;
   else if (!sop2_colour_factor(rgb_src, rev, rev ? &f->csel2 : &f->csel1,
                                rev ? &f->cmod2 : &f->cmod1))
      return false;
   /* (swapped: the source's factor goes with src2, the tile's with src1) */
   return sop2_colour_factor(rgb_dst, rev, rev ? &f->csel1 : &f->csel2,
                             rev ? &f->cmod1 : &f->cmod2) &&
          sop2_alpha_factor(alpha_src, rev, rev ? &f->asel2 : &f->asel1,
                            rev ? &f->amod2 : &f->amod1) &&
          sop2_alpha_factor(alpha_dst, rev, rev ? &f->asel1 : &f->asel2,
                            rev ? &f->amod1 : &f->amod2);
}

static void
optimize(nir_shader *s)
{
   /* ifs of up to 32 instructions a side flattened (selects); bigger ones
    * branch -- an unrolled loop with a break flattened whole needed 65
    * registers a pixel (M20).  SGX_FLATTEN_LIMIT=n to try others. */
   const nir_opt_peephole_select_options flatten = {
      .limit = getenv("SGX_FLATTEN_LIMIT") ? atoi(getenv("SGX_FLATTEN_LIMIT")) : 32,
      .indirect_load_ok = true, .expensive_alu_ok = true,
      .discard_ok = true,        /* if (c) discard: terminate_if(c) */
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
/* (M43) the colour to an intensity target: R into all four channels */
static bool
lower_intensity_target(nir_builder *b, nir_intrinsic_instr *in, void *data)
{
   nir_io_semantics sem;
   nir_def *v;

   if (in->intrinsic != nir_intrinsic_store_output)
      return false;
   sem = nir_intrinsic_io_semantics(in);
   if ((sem.location != FRAG_RESULT_COLOR && sem.location != FRAG_RESULT_DATA0) ||
       sem.dual_source_blend_index || nir_intrinsic_component(in))
      return false;
   v = in->src[0].ssa;
   b->cursor = nir_before_instr(&in->instr);
   nir_src_rewrite(&in->src[0], nir_replicate(b, nir_channel(b, v, 0), 4));
   nir_intrinsic_set_write_mask(in, 0xf);
   return true;
}

/* (M43) the colour to an sRGB target: its RGB encoded on the way out (a
 * store of the colour), decoded on the way in (blending's fetch of it) */
static bool
lower_srgb_target(nir_builder *b, nir_intrinsic_instr *in, void *data)
{
   nir_io_semantics sem;

   if (in->intrinsic != nir_intrinsic_store_output && in->intrinsic != nir_intrinsic_load_output)
      return false;
   sem = nir_intrinsic_io_semantics(in);
   if (sem.location != FRAG_RESULT_COLOR && sem.location != FRAG_RESULT_DATA0)
      return false;
   if (in->intrinsic == nir_intrinsic_store_output) {
      nir_def *v = in->src[0].ssa;

      if (v->num_components < 3)
         return false;
      b->cursor = nir_before_instr(&in->instr);
      nir_def *rgb = nir_format_linear_to_srgb(b, nir_channels(b, v, 0x7));
      nir_def *out = v->num_components == 4 ?
                     nir_vec4(b, nir_channel(b, rgb, 0), nir_channel(b, rgb, 1),
                              nir_channel(b, rgb, 2), nir_channel(b, v, 3)) : rgb;

      nir_src_rewrite(&in->src[0], out);
      return true;
   }
   if (!sem.fb_fetch_output || in->def.num_components < 3)
      return false;
   b->cursor = nir_after_instr(&in->instr);
   {
      nir_def *v = &in->def;
      nir_def *rgb = nir_format_srgb_to_linear(b, nir_channels(b, v, 0x7));
      nir_def *lin = v->num_components == 4 ?
                     nir_vec4(b, nir_channel(b, rgb, 0), nir_channel(b, rgb, 1),
                              nir_channel(b, rgb, 2), nir_channel(b, v, 3)) : rgb;

      nir_def_rewrite_uses_after(v, lin);
   }
   return true;
}

static void
lower_blend(nir_shader *s, const struct sgx_blend_key *k)
{
   nir_lower_blend_options o = { .scalar_blend_const = true };

   /* (floats: not clamped, M43) */
   o.rt[0].format = k->rt_kind == SGX_RT_F16X4 ? PIPE_FORMAT_R16G16B16A16_FLOAT :
                    k->rt_kind == SGX_RT_F16X2 ? PIPE_FORMAT_R16G16_FLOAT :
                    k->rt_kind == SGX_RT_F32 ? PIPE_FORMAT_R32_FLOAT :
                    k->rt_kind == SGX_RT_F32X2 ? PIPE_FORMAT_R32G32_FLOAT :
                    k->rt_srgb ? PIPE_FORMAT_B8G8R8A8_SRGB : PIPE_FORMAT_B8G8R8A8_UNORM;
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

/* A vertex's outputs into o0.. -- the position, then the varyings, four
 * words each, as the TA state's vertex size says -- and out to the tiler.
 * What the shader does not write is 0, w 1; the varyings it does are there
 * already. */
#define USSE_EMIT_VERTEX_END 0xfb275000a0200000ull
#define USSE_PHAS_BRANCHES   0xfa44270000000000ull

static void
vertex_output(struct comp *c)
{
   struct scratch s = { 0 };

   if (!c->have_vout[0][0] || !c->have_vout[0][1] || !c->have_vout[0][3]) {
      fail(c, "no position written");
      return;
   }
   for (unsigned k = 0, nscaled = 0; k <= c->nvaryings; k++) {
      unsigned fix = k && c->vary_fix ? c->vary_fix[k - 1] : 0;

      for (unsigned i = 0; i < 4; i++) {
         /* (M39) a lookup's varying: the divisor where the PDS divides */
         unsigned from = SGX_FIX_DIV(fix) && i == SGX_PROJ_DIV ? SGX_FIX_DIV(fix) : i;
         struct operand o = c->vout[k][from];

         if (k && c->have_vout[k][i] && !fix)
            continue;
         if (!c->have_vout[k][from]) {
            o.is_const = true;
            o.c = from == 3 ? 1.0f : 0.0f;
         }
         if (i < 2 && (fix & SGX_FIX_SCALE)) {
            /* x and y times a rectangle texture's 1 / width, 1 / height */
            emit(c, usse_fop(USSE_NMAD_MUL, usse_reg(USSE_OUTPUT, 4 * k + i),
                             get(c, o, TAKES_ALL, &s),
                             usse_reg(USSE_SA, c->vs->fix_sa + 2 * nscaled + i)));
         } else if (k == 0 && i < 2) {
            /* x and y as a x + b w, a and b the draw's (the TA's clip
             * rectangle, sgx_draw.c) */
            struct usse_reg t = treg(c, scratch_take(c, &s)), u = treg(c, scratch_take(c, &s));

            emit(c, usse_fop(USSE_NMAD_MUL, t, get(c, c->vout[0][3], TAKES_ALL, &s),
                             usse_reg(USSE_SA, c->vs->clip_sa + 2 * i + 1)));
            emit(c, usse_fop(USSE_NMAD_MUL, u, get(c, o, TAKES_ALL, &s),
                             usse_reg(USSE_SA, c->vs->clip_sa + 2 * i)));
            emit(c, usse_fop(USSE_NMAD_ADD, usse_reg(USSE_OUTPUT, i), u, t));
         } else {
            move_into(c, usse_reg(USSE_OUTPUT, 4 * k + i), o, &s);
         }
         scratch_give_back(c, &s);
      }
      nscaled += !!(fix & SGX_FIX_SCALE);
   }
   emit(c, USSE_EMIT_VERTEX_END);
}

/* A uniform's constant offset into its base: nir_lower_int_to_float makes
 * the integer constants floats, an offset of 1 among them (0x3f800000: a
 * matrix's second column read as word 4261412864) */
static bool
fold_uniform_offset(nir_builder *b, nir_intrinsic_instr *in, void *data)
{
   if (in->intrinsic != nir_intrinsic_load_uniform || !nir_src_is_const(in->src[0]) ||
       !nir_src_as_uint(in->src[0]))
      return false;
   b->cursor = nir_before_instr(&in->instr);
   nir_intrinsic_set_base(in, nir_intrinsic_base(in) + nir_src_as_uint(in->src[0]));
   nir_src_rewrite(&in->src[0], nir_imm_int(b, 0));
   return true;
}

/* ftrunc (int(x), made by nir_lower_int_to_float after the options'
 * lowering ran) as its sign times floor(|x|), floor y as y - fract y --
 * |x| a little larger first, by 2^-13 of itself (1/16 at most): a whole
 * number that comes a bit short is that number still.  An int division is
 * trunc(x * rcp(y)), and 24 / -3 came out -7.9999995; a varying that is 24
 * at every vertex comes 23.9995 into a viewport of 128 x 112 (dEQP's div
 * cases, glvary; M31) */
static bool
lower_ftrunc(nir_builder *b, nir_alu_instr *alu, void *data)
{
   nir_def *x, *a, *f;

   if (alu->op != nir_op_ftrunc)
      return false;
   b->cursor = nir_before_instr(&alu->instr);
   x = nir_ssa_for_alu_src(b, alu, 0);
   a = nir_fabs(b, x);
   a = nir_fadd(b, a, nir_fmin(b, nir_fmul_imm(b, a, 1.0 / (1 << 13)), nir_imm_float(b, 0.0625f)));
   f = nir_fadd(b, a, nir_fneg(b, nir_ffract(b, a)));
   nir_def_replace(&alu->def, nir_bcsel(b, nir_flt_imm(b, x, 0.0), nir_fneg(b, f), f));
   return true;
}

/* gl_FrontFacing as 0 < its sign, which intrinsic() makes */
static bool
lower_front_face(nir_builder *b, nir_intrinsic_instr *in, void *data)
{
   if (in->intrinsic != nir_intrinsic_load_front_face)
      return false;
   b->cursor = nir_before_instr(&in->instr);
   nir_def_replace(&in->def, nir_flt(b, nir_imm_float(b, 0.0f), nir_load_front_face_fsign(b)));
   return true;
}

/* A branch (iOS's c04_loop_break: `p0? br +21`, `br -21`): relative, in
 * instructions from itself, under an extended predicate (0 always, 1 p0,
 * 5 !p0) */
#define USSE_BR   0xf800004000000000ull

static void
branch_to(struct comp *c, unsigned at, unsigned target)
{
   uint64_t *w = util_dynarray_element(&c->code, uint64_t, at);

   *w = (*w & ~0xfffffull) | ((uint32_t)(target - at) & 0xfffff);
}

static unsigned
branch(struct comp *c, unsigned pred)
{
   unsigned at = util_dynarray_num_elements(&c->code, uint64_t);

   /* (pixels run together: taken when all of them take it -- bit 20,
    * Vita3K's all_inst; any_inst, bit 21, did the same with branches
    * that go alike, M33) */
   emit(c, USSE_BR | (uint64_t)pred << 56 | (!c->vs && !c->divergent ? 1ull << 20 : 0));
   c->branches = true;
   return at;
}

static unsigned
here(struct comp *c)
{
   return util_dynarray_num_elements(&c->code, uint64_t);
}

static void
instruction(struct comp *c, nir_instr *instr, struct util_dynarray *breaks, unsigned head)
{
   struct scratch sc = { 0 };

   switch (instr->type) {
   case nir_instr_type_alu:
      alu(c, nir_instr_as_alu(instr), &sc);
      break;
   case nir_instr_type_intrinsic:
      intrinsic(c, nir_instr_as_intrinsic(instr));
      break;
   case nir_instr_type_tex:
      texture(c, nir_instr_as_tex(instr), &sc);
      break;
   case nir_instr_type_load_const:
   case nir_instr_type_undef:
      break;
   case nir_instr_type_jump:
      switch (nir_instr_as_jump(instr)->type) {
      case nir_jump_break:
         if (breaks) {
            unsigned at = branch(c, 0);

            util_dynarray_append(breaks, at);
            break;
         }
         FALLTHROUGH;
      case nir_jump_continue:
         if (breaks) {
            branch_to(c, branch(c, 0), head);
            break;
         }
         FALLTHROUGH;
      default:
         fail(c, "a jump out of no loop");
         break;
      }
      break;
   default:
      fail(c, "no such instructions yet");
      break;
   }
   scratch_give_back(c, &sc);
}

/* What one walk over the shader's control flow does: number the
 * instructions (each def's, each loop's span), note what each reads (the
 * liveness), or make the code.  The three number alike: an instruction, an
 * if's test and a loop's end marker an index each. */
enum walk_pass { WALK_NUMBER, WALK_USES, WALK_CODE };

static void
walk(struct comp *c, struct exec_list *list, enum walk_pass pass, int *index,
     struct util_dynarray *breaks, unsigned head)
{
   foreach_list_typed(nir_cf_node, node, node, list) {
      if (c->failed)
         return;
      switch (node->type) {
      case nir_cf_node_block:
         nir_foreach_instr(instr, nir_cf_node_as_block(node)) {
            nir_def *def = nir_instr_def(instr);

            if (pass == WALK_NUMBER && def)
               c->def_at[def->index] = *index;
            else if (pass == WALK_USES)
               for_each_read(c, instr, note_use, *index);
            else if (pass == WALK_CODE) {
               instruction(c, instr, breaks, head);
               for_each_read(c, instr, release, *index);
            }
            (*index)++;
            if (c->failed)
               return;
         }
         break;
      case nir_cf_node_if: {
         /* the test -- p0 = cond's bits are 0 (a boolean is 0.0 or 1.0
          * here), then `p0? br`, as iOS's branches have it -- past the
          * then-list where it fails; the else-list past the then-list's
          * end */
         nir_if *nif = nir_cf_node_as_if(node);
         struct operand cond = resolve(nir_get_scalar(nif->condition.ssa, 0));
         int at = (*index)++;
         unsigned to_else = 0, to_end = 0;

         if (pass == WALK_USES)
            note_use(c, cond, at);
         if (pass == WALK_CODE) {
            struct scratch sc = { 0 };

            emit(c, usse_vtst_bits(0, get(c, cond, TAKES_LANE_Y | TAKES_SA, &sc), true));
            scratch_give_back(c, &sc);
            release(c, cond, at);
            to_else = branch(c, 1);
         }
         c->depth++;
         walk(c, &nif->then_list, pass, index, breaks, head);
         if (pass == WALK_CODE && !exec_list_is_empty(&nif->else_list) &&
             !nir_cf_list_is_empty_block(&nif->else_list))
            to_end = branch(c, 0);
         if (pass == WALK_CODE)
            branch_to(c, to_else, here(c));
         walk(c, &nif->else_list, pass, index, breaks, head);
         if (pass == WALK_CODE && to_end)
            branch_to(c, to_end, here(c));
         c->depth--;
         break;
      }
      case nir_cf_node_loop: {
         /* the body, a branch back to its start; a break past it */
         nir_loop *loop = nir_cf_node_as_loop(node);
         unsigned k = c->nloops++, start_code = here(c);
         struct util_dynarray mine;
         int start = *index, end;

         if (nir_loop_has_continue_construct(loop)) {
            fail(c, "a loop with a continue construct");
            return;
         }
         /* (a loop's span is k-th in the order the loops start, the same
          * in every pass: an inner loop's after its outer one's -- taken
          * at the end, the outer loop had the inner one's span, and what
          * it reads lived to the inner loop's end, M31) */
         if (pass == WALK_NUMBER) {
            struct loop_span span = { start, -1 };

            util_dynarray_append(&c->loops, span);
         }
         if (pass == WALK_USES) {
            if (c->nactive == ARRAY_SIZE(c->active)) {
               fail(c, "loops nested too deep");
               return;
            }
            c->active[c->nactive++] = *util_dynarray_element(&c->loops, struct loop_span, k);
         }
         util_dynarray_init(&mine, NULL);
         c->depth++;
         walk(c, &loop->body, pass, index, &mine, start_code);
         c->depth--;
         end = (*index)++;
         if (pass == WALK_NUMBER)
            util_dynarray_element(&c->loops, struct loop_span, k)->end = end;
         if (pass == WALK_USES)
            c->nactive--;
         if (pass == WALK_CODE) {
            branch_to(c, branch(c, 0), start_code);
            util_dynarray_foreach(&mine, unsigned, at)
               branch_to(c, *at, here(c));
            /* what was kept alive for the loop */
            for (unsigned id = 0; id < c->nvalues; id++)
               if (c->last_use[id] == end && c->owned[id]) {
                  give_back(c, c->loc[id].num - (c->vs ? c->pa_base : 0));
                  c->owned[id] = false;
               }
         }
         util_dynarray_fini(&mine);
         break;
      }
      default:
         fail(c, "no such control flow");
         return;
      }
   }
}

/* NIR to instructions, after the opening PHAS, for either stage: the
 * shader lowered to scalar float arithmetic, its ifs flattened where they
 * can be, what is left of them and the loops as branches (M20), its values
 * given temporaries in the order NIR has them.  False (c->failed, c->why)
 * when it cannot be. */
/* A vertex program's start (M26): each attribute the fetch put in pa4n..
 * as it is in memory made four floats -- bytes unpacked (the top two
 * first, the word they come from last), the components the format does
 * not have 0, 0, 1 */
static void
vs_prologue(struct comp *c)
{
   for (unsigned a = 0; a < c->nattrs; a++) {
      unsigned kind = SGX_ATTR_KIND(c->attr[a]), n = SGX_ATTR_COMPS(c->attr[a]);
      struct usse_reg r = usse_reg(USSE_PA, 4 * a);

      if (kind == SGX_ATTR_CONST)
         continue;
      if (kind == SGX_ATTR_U8N) {
         if (n > 2)
            emit(c, usse_unpack_unorm8(usse_reg(USSE_PA, 4 * a + 2), r, 2));
         emit(c, usse_unpack_unorm8(r, r, 0));
      }
      for (unsigned i = n; i < 4; i++)
         emit(c, usse_limm(usse_reg(USSE_PA, 4 * a + i), f32_bits(i == 3 ? 1.0f : 0.0f)));
   }
}

struct latest {
   nir_block *block;
   nir_instr *instr;
};

static bool
note_latest(nir_src *src, void *data)
{
   struct latest *l = data;
   nir_instr *d = nir_def_instr(src->ssa);

   if (d->block == l->block && (!l->instr || d->index > l->instr->index))
      l->instr = d;
   return true;
}

/* instr up to just after the last of what it reads in its block (or of
 * after, when that is later) -- the block's start when it reads nothing of
 * the block's */
static void
hoist(nir_function_impl *impl, nir_instr *instr, nir_instr *after)
{
   struct latest l = { instr->block, after };
   nir_block *b = instr->block;

   nir_foreach_src(instr, note_latest, &l);
   nir_instr_remove(instr);
   if (l.instr)
      nir_instr_insert_after(l.instr, instr);
   else
      nir_instr_insert(nir_before_block_after_phis(b), instr);
   nir_index_instrs(impl);
}

/* A vertex shader's outputs stored as soon as their values are there: the
 * GLSL compiler stores them all at the end, and every value for them was
 * alive until then -- eight vec4 varyings took more temporaries than there
 * are (glvary, M30).  A store stays after the ones before it to the same
 * output. */
static void
store_outputs_early(nir_shader *s)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(s);

   nir_foreach_block(b, impl)
      nir_foreach_instr(instr, b)
         if (instr->type == nir_instr_type_intrinsic &&
             nir_instr_as_intrinsic(instr)->intrinsic == nir_intrinsic_load_output)
            return;     /* (an output read back: left as it is) */
   nir_index_instrs(impl);
   nir_foreach_block(b, impl) {
      nir_foreach_instr_safe(instr, b) {
         nir_intrinsic_instr *in;
         nir_instr *vec, *after = NULL;

         if (instr->type != nir_instr_type_intrinsic)
            continue;
         in = nir_instr_as_intrinsic(instr);
         if (in->intrinsic != nir_intrinsic_store_output)
            continue;
         for (nir_instr *p = nir_instr_prev(instr); p && !after; p = nir_instr_prev(p))
            if (p->type == nir_instr_type_intrinsic &&
                nir_instr_as_intrinsic(p)->intrinsic == nir_intrinsic_store_output &&
                nir_intrinsic_base(nir_instr_as_intrinsic(p)) == nir_intrinsic_base(in))
               after = p;
         /* (the vec4 the value is made into first, when it is the store's
          * alone) */
         vec = nir_def_instr(in->src[0].ssa);
         if (vec->block == b && vec->type == nir_instr_type_alu &&
             nir_op_is_vec(nir_instr_as_alu(vec)->op) && list_is_singular(&in->src[0].ssa->uses))
            hoist(impl, vec, NULL);
         hoist(impl, instr, after);
      }
   }
}

/* ---- a colour of four F16 at once (M38) -----------------------------------
 *
 * iOS's SDL sprite (corpus s00, s03, s11): the colour iterated and the
 * texel fetched as F16 by the PDS, two registers each (four F16 a 64-bit
 * register), then `mul.f16 pa0.xyzw, pa2.xyzw, pa0.xyzw`, `pck.u8.f16`,
 * the SOP2 that blends -- four instructions where the general way takes
 * nine (texels unpacked to F32, a channel an instruction).  A pixel's cost
 * is its instructions (M34), and SuperTux's frame is SDL's sprites.
 *
 * Taken when the colour is, channel for channel, one tree of fmul and
 * fadd over varyings (each channel its own: no swizzle) and the PDS's
 * lookups, nothing else in the program: no other input, constant,
 * uniform, discard, branch, blending in the program or logic op.  The
 * colour goes to 8 bits: F16's 11 bits of mantissa are what mediump's are,
 * and enough for highp's colour too. */
#define F16_NODES 16

struct f16_node {
   enum { F16_INPUT, F16_TEXEL, F16_MUL, F16_ADD } kind;
   unsigned a, b;                       /* the operands' nodes; a leaf's index */
};

struct f16_tree {
   struct f16_node node[F16_NODES];
   unsigned n;
};

/* channel chan of the colour, s, as tree t's nodes (from 0: the same
 * shape for every channel), its root; -1 if it is not such a tree */
static int
f16_match(const struct comp *c, nir_scalar s, unsigned chan, struct f16_tree *t)
{
   s = nir_scalar_chase_movs(s);
   if (nir_scalar_is_alu(s)) {
      nir_op op = nir_scalar_alu_op(s);
      int a, b;

      if ((op != nir_op_fmul && op != nir_op_fadd) || t->n == F16_NODES ||
          (a = f16_match(c, nir_scalar_chase_alu_src(s, 0), chan, t)) < 0 ||
          (b = f16_match(c, nir_scalar_chase_alu_src(s, 1), chan, t)) < 0)
         return -1;
      t->node[t->n] = (struct f16_node){ op == nir_op_fmul ? F16_MUL : F16_ADD, a, b };
      return t->n++;
   }
   if (nir_def_instr(s.def)->type == nir_instr_type_tex) {
      for (unsigned k = 0; k < c->nfetch; k++)
         if (&c->fetch[k]->instr == nir_def_instr(s.def) && s.comp == chan && t->n < F16_NODES) {
            t->node[t->n] = (struct f16_node){ F16_TEXEL, k, 0 };
            return t->n++;
         }
      return -1;
   }
   if (nir_scalar_is_intrinsic(s) &&
       nir_def_as_intrinsic(s.def)->intrinsic == nir_intrinsic_load_input) {
      nir_intrinsic_instr *in = nir_def_as_intrinsic(s.def);
      int slot = input_slot(in);

      if (slot < 0 || c->input_of_slot[slot] < 0 || t->n == F16_NODES ||
          nir_intrinsic_component(in) + s.comp != chan)
         return -1;
      t->node[t->n] = (struct f16_node){ F16_INPUT, (unsigned)c->input_of_slot[slot], 0 };
      return t->n++;
   }
   return -1;
}

static bool
f16_colour(struct comp *c, nir_function_impl *impl)
{
   nir_intrinsic_instr *out = NULL;
   struct f16_tree t[4] = { 0 };
   int root[4];
   unsigned reg[F16_NODES], ntemp = 0;
   struct usse_reg o0 = usse_reg(USSE_OUTPUT, 0), colour;

   if (getenv("SGX_NO_F16") || c->vs || c->kills || c->ncolours || c->logicop >= 0 ||
       c->fs->blend.rt_kind != SGX_RT_UNORM8 || c->fs->blend.rt_srgb ||
       c->fs->blend.rt_intensity ||
       c->nuniforms || exec_list_length(&impl->body) != 1)
      return false;
   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         if (instr->type == nir_instr_type_intrinsic) {
            nir_intrinsic_instr *in = nir_instr_as_intrinsic(instr);

            if (in->intrinsic == nir_intrinsic_store_output) {
               if (out || nir_intrinsic_component(in) || nir_intrinsic_write_mask(in) != 0xf ||
                   in->src[0].ssa->num_components != 4)
                  return false;
               out = in;
            } else if (in->intrinsic != nir_intrinsic_load_input) {
               return false;
            }
         } else if (instr->type == nir_instr_type_tex) {
            if (!is_fetch(c, instr))
               return false;
         } else if (instr->type != nir_instr_type_alu &&
                    instr->type != nir_instr_type_load_const) {
            /* (constants: the outputs' offsets; one in the colour is not a
             * leaf f16_match() takes) */
            return false;
         }
      }
   }
   if (!out)
      return false;
   for (unsigned i = 0; i < c->ninputs; i++)
      if (c->fs->input_slot[i] == VARYING_SLOT_POS)
         return false;
   for (unsigned ch = 0; ch < 4; ch++) {
      if ((root[ch] = f16_match(c, nir_get_scalar(out->src[0].ssa, ch), ch, &t[ch])) < 0 ||
          t[ch].n != t[0].n || root[ch] != root[0] ||
          memcmp(t[ch].node, t[0].node, t[0].n * sizeof(t[0].node[0])))
         return false;
   }
   /* every input and lookup the program reads is in the tree */
   {
      unsigned inputs = 0, fetches = 0;

      for (unsigned k = 0; k < t[0].n; k++) {
         if (t[0].node[k].kind == F16_INPUT)
            inputs |= 1u << t[0].node[k].a;
         else if (t[0].node[k].kind == F16_TEXEL)
            fetches |= 1u << t[0].node[k].a;
      }
      if (inputs != (1u << c->ninputs) - 1 || fetches != (1u << c->nfetch) - 1)
         return false;
   }
   /* the code: inputs in pa(2i), texels after them, two registers each;
    * each operation into a temporary pair of its own */
   util_dynarray_init(&c->code, NULL);
   emit(c, USSE_PHAS);
   for (unsigned k = 0; k < t[0].n; k++) {
      const struct f16_node *nd = &t[0].node[k];

      switch (nd->kind) {
      case F16_INPUT:
         reg[k] = 2 * nd->a;
         break;
      case F16_TEXEL:
         reg[k] = 2 * c->ninputs + 2 * nd->a;
         break;
      default:
         reg[k] = 0x100 + 2 * ntemp++;
         emit(c, usse_hop(nd->kind == F16_MUL ? USSE_NMAD_MUL : USSE_NMAD_ADD,
                          treg(c, reg[k] - 0x100),
                          reg[nd->a] >= 0x100 ? treg(c, reg[nd->a] - 0x100) :
                                                usse_reg(USSE_PA, reg[nd->a]),
                          reg[nd->b] >= 0x100 ? treg(c, reg[nd->b] - 0x100) :
                                                usse_reg(USSE_PA, reg[nd->b])));
         break;
      }
   }
   colour = reg[root[0]] >= 0x100 ? treg(c, reg[root[0]] - 0x100) : usse_reg(USSE_PA, reg[root[0]]);
   if (c->sop2) {
      struct usse_reg packed = treg(c, 2 * ntemp++);

      emit(c, usse_pack_unorm8_f16(packed, colour));
      emit(c, usse_sop2(o0, c->sop2_swap ? o0 : packed, c->sop2_swap ? packed : o0,
                        &c->sop2_factors) | USSE_END);
   } else {
      emit(c, usse_pack_unorm8_f16(o0, colour) | USSE_END);
   }
   c->top = MAX2(c->top, 2 * ntemp);
   c->f16 = true;
   return true;
}

static bool
translate(struct comp *c, nir_shader *s)
{
   nir_function_impl *impl;
   unsigned n;
   int index;

   optimize(s);
   NIR_PASS(_, s, nir_shader_intrinsics_pass, fold_uniform_offset, nir_metadata_control_flow,
            NULL);
   NIR_PASS(_, s, nir_lower_int_to_float);
   NIR_PASS(_, s, nir_shader_alu_pass, lower_ftrunc, nir_metadata_control_flow, NULL);
   NIR_PASS(_, s, nir_lower_bool_to_float, true);
   NIR_PASS(_, s, nir_opt_algebraic_late);
   NIR_PASS(_, s, nir_lower_alu_to_scalar, NULL, NULL);
   NIR_PASS(_, s, nir_opt_copy_prop);
   NIR_PASS(_, s, nir_opt_cse);
   NIR_PASS(_, s, nir_opt_dce);
   if (c->vs)
      store_outputs_early(s);
   /* values made just before their first use, in their block (M35): a
    * value takes a register pair here, 32 of them, and the GLSL compiler's
    * order had piglit's varying-packing vertex shaders make all 32 sums of
    * a uniform first and use them after -- more than the temporaries hold.
    * (nir_schedule's least-pressure order made its fragment shader worse:
    * 32 comparisons first, then the chain that reads them.)  NIR moves ALU
    * operations with one source that is not a constant. */
   if (!c->keep_order)
      NIR_PASS(_, s, nir_opt_move, nir_move_const_undef | nir_move_load_uniform |
                                   nir_move_load_input | nir_move_comparisons | nir_move_alu);

   impl = nir_shader_get_entrypoint(s);
   if (!c->vs && exec_list_length(&impl->body) != 1) {
      /* what a loop does not change, out of it -- then whether any if
       * (a loop's breaks among them) differs between pixels (M33) */
      NIR_PASS(_, s, nir_opt_licm);
      nir_divergence_analysis(s);
      nir_foreach_block(block, impl) {
         nir_if *nif = nir_block_get_following_if(block);

         c->divergent |= nif && nir_src_is_divergent(&nif->condition);
      }
   }
   if (c->fs)
      lower_fetch_coords(c, s, impl);
   /* control flow left: the phis' webs registers */
   if (exec_list_length(&impl->body) != 1)
      NIR_PASS(_, s, nir_convert_from_ssa, true, false);
   nir_index_ssa_defs(impl);
   c->nvalues = n = impl->ssa_alloc * 4;
   c->loc = calloc(n, sizeof(*c->loc));
   c->owned = calloc(n, sizeof(*c->owned));
   c->last_use = malloc(n * sizeof(*c->last_use));
   c->def_at = calloc(impl->ssa_alloc, sizeof(*c->def_at));
   if (!c->loc || !c->owned || !c->last_use || !c->def_at) {
      fail(c, "out of memory");
      return false;
   }
   for (unsigned i = 0; i < n; i++)
      c->last_use[i] = -1;
   util_dynarray_init(&c->loops, NULL);

   /* SGX_DEBUG_NIR=1: the shader as the compiler takes it */
   if (getenv("SGX_DEBUG_NIR"))
      nir_print_shader(s, stderr);
   if (c->fs)
      plan_inputs(c, impl);
   scan(c, impl);
   if (c->failed)
      return false;
   /* M38: a colour of four F16 at once -- the whole program */
   if (c->fs && f16_colour(c, impl))
      return true;

   /* the numbering, the liveness, then the code, in the same order */
   index = 0;
   c->nloops = 0;
   walk(c, &impl->body, WALK_NUMBER, &index, NULL, 0);
   index = 0;
   c->nloops = 0;
   walk(c, &impl->body, WALK_USES, &index, NULL, 0);

   util_dynarray_init(&c->code, NULL);
   emit(c, USSE_PHAS);
   /* the colour's registers first, and which values go into them: those
    * an ALU instruction makes, each into one place (M34) */
   c->out_base = -1;
   for (unsigned i = 0; i < 4; i++)
      c->colour_id[i] = -1;
   if (!c->vs) {
      nir_foreach_block(block, impl)
         nir_foreach_instr(instr, block) {
            nir_intrinsic_instr *in;

            if (instr->type != nir_instr_type_intrinsic)
               continue;
            in = nir_instr_as_intrinsic(instr);
            if (in->intrinsic != nir_intrinsic_store_output || block->cf_node.parent != &impl->cf_node)
               continue;
            for (unsigned i = 0; i < in->src[0].ssa->num_components; i++) {
               unsigned comp = nir_intrinsic_component(in) + i;
               struct operand o = resolve(nir_get_scalar(in->src[0].ssa, i));
               bool taken = false;

               nir_scalar sc = nir_scalar_chase_movs(nir_get_scalar(in->src[0].ssa, i));

               /* (the value an ALU instruction makes, define()'s) */
               /* (V32NMAD's: the others' encodings take an even
                * destination -- compares, selects, RCP -- and two of the
                * four are odd) */
               if (comp >= 4 || !(nir_intrinsic_write_mask(in) & 1 << i) || o.is_const || o.neg ||
                   o.abs || !nir_scalar_is_alu(sc) ||
                   (nir_scalar_alu_op(sc) != nir_op_fmul && nir_scalar_alu_op(sc) != nir_op_fadd &&
                    nir_scalar_alu_op(sc) != nir_op_fmin && nir_scalar_alu_op(sc) != nir_op_fmax) ||
                   (unsigned)o.id != sc.def->index * 4 + sc.comp || sc.def->num_components != 1)
                  continue;
               for (unsigned k = 0; k < 4; k++)
                  taken |= c->colour_id[k] == (int)o.id;
               if (!taken)
                  c->colour_id[comp] = o.id;
            }
         }
   }
   if (c->kills) {
      c->kill = block(c, 2, 2);
      emit(c, usse_limm(treg(c, c->kill), 0));
   }
   if (c->vs)
      vs_prologue(c);
   index = 0;
   c->nloops = 0;
   walk(c, &impl->body, WALK_CODE, &index, NULL, 0);
   /* a program that branches: PHAS mode 1, as iOS's c04_loop_break has,
    * its pixels each on its own -- mode 0, the pixels together, when its
    * branches go alike for all of them (M33) */
   if (c->branches)
      *util_dynarray_element(&c->code, uint64_t, 0) =
         !c->vs && !c->divergent ? USSE_PHAS : USSE_PHAS_BRANCHES;
   return !c->failed;
}

static struct sgx_fs *
compile_fs(const nir_shader *fs, const struct sgx_blend_key *blend, char *why,
           unsigned why_size, bool keep_order)
{
   struct comp c = { .keep_order = keep_order };
   bool blending = blend && (blend->enable || blend->colormask != 0xf || blend->rt_srgb ||
                             blend->rt_intensity);
   nir_shader *s;
   unsigned out_at;

   c.why = why;
   c.kill = -1;
   c.logicop = -1;
   c.why_size = why_size;
   why[0] = 0;
   if (!(c.fs = CALLOC_STRUCT(sgx_fs)))
      return NULL;
   for (unsigned i = 0; i < ARRAY_SIZE(c.input_of_slot); i++)
      c.input_of_slot[i] = c.colour_of_slot[i] = -1;

   for (unsigned i = 0; i < ARRAY_SIZE(c.slot_of_unit); i++)
      c.slot_of_unit[i] = -1;
   c.blend_sa = -1;
   c.scale_sa = -1;
   if (blend)
      c.fs->blend = *blend;
   else
      c.fs->blend.colormask = 0xf;
   /* (discard keeps the tile's colour as the colour: blended again, not
    * kept, M19) */
   if (blending && !fs->info.fs.uses_discard && !blend->logicop &&
       blend->rt_kind == SGX_RT_UNORM8 && !blend->rt_srgb && !blend->rt_intensity &&
       !getenv("SGX_NO_SOP2") && sop2_blend(blend, &c.sop2_factors, &c.sop2_swap)) {
      c.sop2 = true;
      blending = false;
   }
   /* a logic op instead of the blending, and the colour mask with it: on
    * the bytes, in output() (nir_lower_blend's are integer operations,
    * which the compiler has not) */
   if (blend && blend->logicop && blend->rt_kind == SGX_RT_UNORM8) {
      c.logicop = blend->logicop - 1;
      blending = false;
   }

   s = nir_shader_clone(NULL, fs);
   /* the flat-shaded colours: the TA's (M37) -- GL's flat shading.  Not
    * other varyings: the linker makes one flat whose value is the same at
    * every vertex (piglit's glsl-sin: a uniform's sine), and the TA's
    * colours are bytes; a varying the same everywhere comes out so smooth */
   nir_foreach_shader_in_variable(var, s) {
      if (var->data.interpolation != INTERP_MODE_FLAT ||
          (var->data.location != VARYING_SLOT_COL0 && var->data.location != VARYING_SLOT_COL1 &&
           var->data.location != VARYING_SLOT_BFC0 && var->data.location != VARYING_SLOT_BFC1))
         continue;
      for (unsigned i = 0; i < glsl_count_attribute_slots(var->type, false); i++)
         if (var->data.location + i < VARYING_SLOT_MAX)
            c.flat[var->data.location + i] = true;
   }
   if (blending)
      NIR_PASS(_, s, nir_lower_fragcolor, 1);
   NIR_PASS(_, s, nir_lower_io, nir_var_shader_in | nir_var_shader_out | nir_var_uniform,
            type_size_vec4, 0);
   if (blending)
      lower_blend(s, blend);
   /* (M43) an sRGB target: the colour encoded as it goes out, the tile's
    * decoded as blending reads it (nir_lower_blend leaves both to us) */
   if (blend && blend->rt_srgb)
      NIR_PASS(_, s, nir_shader_intrinsics_pass, lower_srgb_target, nir_metadata_control_flow,
               NULL);
   /* (M43) an intensity target keeps R in all four: blending later in the
    * same render reads its alpha as the intensity, as GL has it (the
    * background loads it R R R R too) */
   if (blend && blend->rt_intensity)
      NIR_PASS(_, s, nir_shader_intrinsics_pass, lower_intensity_target,
               nir_metadata_control_flow, NULL);
   {
      /* (rectangle textures: the state tracker scales their coordinates
       * already -- caps.texrect off -- and leaves the dimension Rect) */
      nir_lower_tex_options tex = { .lower_txp = ~0u, .lower_1d = true };

      /* the linear sampler reads B G R A: other orders swizzled back --
       * and a view's swizzle on what that gives (M43) */
      for (unsigned u = 0; blend && u < 8; u++) {
         uint8_t base[4];

         if (!((blend->tex_swap | blend->tex_x8 | blend->tex_swz) >> u & 1))
            continue;
         base[0] = blend->tex_swap >> u & 1 ? 2 : 0;
         base[1] = 1;
         base[2] = blend->tex_swap >> u & 1 ? 0 : 2;
         base[3] = blend->tex_x8 >> u & 1 ? PIPE_SWIZZLE_1 : 3;
         tex.swizzle_result |= 1u << u;
         for (unsigned c = 0; c < 4; c++) {
            unsigned v = blend->tex_swz >> u & 1 ? blend->tex_swizzle[u][c] : c;

            tex.swizzles[u][c] = v <= PIPE_SWIZZLE_W ? base[v] : v;
         }
      }
      if (blend)
         tex.lower_srgb = blend->tex_srgb;
      /* shadow samplers: the depth compared in the program (M37: iOS's
       * m07 -- the reference clamped, `tex - ref >= 0`); one not compared
       * by its sampler state (or before a variant says) passes */
      {
         enum compare_func func[8];
         nir_lower_tex_shadow_swizzle swz[8];

         for (unsigned u = 0; u < 8; u++) {
            bool on = blend && (blend->tex_shadow >> u & 1);

            func[u] = on ? blend->shadow_func[u] : COMPARE_FUNC_ALWAYS;
            swz[u] = on ? (nir_lower_tex_shadow_swizzle){
                             blend->depth_swizzle[u][0], blend->depth_swizzle[u][1],
                             blend->depth_swizzle[u][2], blend->depth_swizzle[u][3] } :
                          (nir_lower_tex_shadow_swizzle){
                             PIPE_SWIZZLE_X, PIPE_SWIZZLE_X, PIPE_SWIZZLE_X, PIPE_SWIZZLE_1 };
         }
         NIR_PASS(_, s, nir_lower_tex_shadow, 8, func, swz, true);
      }
      NIR_PASS(_, s, nir_lower_tex, &tex);
   }
   /* the facing bit is set for triangles anticlockwise in the target, row
    * 0 at the top -- gallium's sense (glcull, M19) */
   c.front_when_set = blend && blend->front_ccw;
   NIR_PASS(_, s, nir_shader_intrinsics_pass, lower_front_face, nir_metadata_control_flow, NULL);
   if (!translate(&c, s))
      goto out;
   out_at = util_dynarray_num_elements(&c.code, uint64_t);
   if (!c.f16)
      output(&c);
   if (c.failed)
      goto out;
   /* a pixel's registers, primary attributes and temporaries: 65 (an
    * unrolled loop's) hung the GPU; 64 is what the temporaries alone may
    * take */
   if (align(4 * c.ninputs + c.ncolours + c.nfetch, 4) + c.top > 64) {
      fail(&c, "%u registers a pixel", align(4 * c.ninputs + c.ncolours + c.nfetch, 4) + c.top);
      goto out;
   }
   c.fs->prog.f16 = c.f16;

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
    * along the diagonal seam).  But not the writes of the output: those
    * pixels' tiles would take them too -- blended twice along each shared
    * edge, in 2x2 blocks (weston's panel, M16). */
   /* And a program that branches the same (iOS's c04_loop_break): the
    * pixels around the triangle skipped the tests of a loop's condition and
    * took its branches on a predicate left from before -- round the loop
    * for ever, now and then (M20). */
   /* (and a program with derivatives: the pixels around the triangle in
    * its 2x2 blocks are what DSX and DSY read, glflow -- M33) */
   /* (the PDS's lookups take theirs from the iterated coordinates: as
    * iOS's t00, the program keeps skipinv) */
   if (c.smp || c.branches || c.derivatives)
      for (unsigned i = 0; i < out_at; i++)
         c.fs->prog.code[i] &= ~(1ull << 55);
   c.fs->prog.ntemps = c.top;
   /* SGX_LDR_PROBE=word,...: the program those words, WDF0, then r0 into
    * o0 as it is (finding VLDST, M32) */
   if (getenv("SGX_LDR_PROBE")) {
      const char *e = getenv("SGX_LDR_PROBE");
      unsigned n = 0;

      c.fs->prog.code = REALLOC(c.fs->prog.code, 0, 32 * sizeof(uint64_t));
      c.fs->prog.code[n++] = USSE_PHAS;
      while (*e && n < 28) {
         char *end;

         c.fs->prog.code[n++] = strtoull(e, &end, 16);
         e = *end == ',' ? end + 1 : end;
      }
      c.fs->prog.code[n++] = USSE_WDF0;
      c.fs->prog.code[n++] = usse_fmov(usse_reg(USSE_OUTPUT, 0),
                                       usse_reg(USSE_TEMP, getenv("SGX_LDR_OUT") ?
                                                           atoi(getenv("SGX_LDR_OUT")) : 0)) | USSE_END;
      c.fs->prog.ncode = n;
      c.fs->prog.ntemps = MAX2(c.fs->prog.ntemps, 8);
      mesa_logi("sgx: SGX_LDR_PROBE: %u words, samplers' state at sa%u", n, c.sampler_sa);
   }
   c.fs->prog.branches = c.branches && c.divergent;
   c.fs->prog.ninputs = c.ninputs + c.ncolours;
   c.fs->prog.ncolours = c.ncolours;
   c.fs->prog.nvaryings = c.ncolours;
   for (unsigned j = 0; j < c.ncolours; j++) {
      c.fs->input_slot[c.ninputs + j] = c.colour_slot[j];
      c.fs->varying_slot[j] = c.colour_slot[j];
      c.fs->prog.iter_src[c.ninputs + j] = j;
   }
   for (unsigned i = 0; i < c.ninputs; i++) {
      if (c.fs->input_slot[i] == VARYING_SLOT_POS) {
         c.fs->prog.iter_src[i] = SGX_ITERATE_POSITION;
         continue;
      }
      c.fs->varying_slot[c.fs->prog.nvaryings] = c.fs->input_slot[i];
      c.fs->prog.iter_src[i] = c.fs->prog.nvaryings++;
   }
   /* the lookups' varyings, those not iterated after the rest */
   for (unsigned k = 0; k < c.nfetch; k++) {
      unsigned v = 0;

      while (v < c.fs->prog.nvaryings && c.fs->varying_slot[v] != c.fetch_slot[k])
         v++;
      if (v == SGX_FRAME_MAX_VARYINGS) {
         fail(&c, "more than %u varyings", SGX_FRAME_MAX_VARYINGS);
         goto out;
      }
      if (v == c.fs->prog.nvaryings)
         c.fs->varying_slot[c.fs->prog.nvaryings++] = c.fetch_slot[k];
      c.fs->prog.fetch_src[k] = v;
      c.fs->prog.fetch_sa[k] = c.sampler_sa + 4 * c.slot_of_unit[c.fetch[k]->texture_index];
      /* (M39) what the vertex side does to the varying, the PDS's kind */
      if (SGX_FIX_DIV(c.fetch_fix[k]))
         c.fs->prog.fetch_proj |= 1u << k;
      if (c.fetch_fix[k]) {
         c.fs->vary_fix[v] = c.fetch_fix[k];
         c.fs->vary_unit[v] = c.fetch[k]->texture_index;
      }
   }
   c.fs->prog.nfetch = c.nfetch;
   c.fs->nuniforms = c.sa_uniforms;
   c.fs->prog.nubuf = c.ubuf ? c.uwords : 0;
   c.fs->prog.ubuf_sa = c.ubuf_sa;
   c.fs->prog.ubo_mask = c.ubo_mask;
   for (unsigned k = 0; k < SGX_FRAME_MAX_UBOS; k++)
      c.fs->prog.ubo_sa[k] = c.ubo_mask >> k & 1 ? c.ubo_sa[k] : 0;
   c.fs->nsamplers = c.nsamplers;
   c.fs->sampler_sa = c.sampler_sa;
   c.fs->prog.nsa = c.blend_sa >= 0 ? c.blend_sa + 4 :
                    c.scales ? c.scale_sa + 2 * c.nsamplers :
                    c.nsamplers ? c.sampler_sa + 4 * c.nsamplers : c.nuniforms;
   c.fs->blend_sa = c.blend_sa;
   c.fs->scale_sa = c.scale_sa;

out:
   util_dynarray_fini(&c.code);
   free(c.loc);
   free(c.owned);
   free(c.last_use);
   free(c.def_at);
   util_dynarray_fini(&c.loops);
   ralloc_free(s);
   if (c.failed) {
      sgx_fs_destroy(c.fs);
      return NULL;
   }
   return c.fs;
}

unsigned
sgx_vs_attr_count(const nir_shader *vs)
{
   unsigned n = 0;

   nir_foreach_shader_in_variable(var, vs)
      n = MAX2(n, var->data.driver_location + glsl_count_attribute_slots(var->type, true));
   return MIN2(n, SGX_VS_MAX_ATTRIBS);
}

static struct sgx_vs *
compile_vs(const nir_shader *vs, const unsigned *varying_slot, unsigned nvaryings,
           const uint8_t *attr, const uint8_t *fix, char *why, unsigned why_size,
           bool keep_order)
{
   struct comp c = { .keep_order = keep_order };
   unsigned out_at, nattr = sgx_vs_attr_count(vs);
   nir_shader *s;

   c.why = why;
   c.kill = -1;
   c.logicop = -1;
   c.why_size = why_size;
   why[0] = 0;
   if (nvaryings > SGX_FRAME_MAX_VARYINGS || !(c.vs = CALLOC_STRUCT(sgx_vs)))
      return NULL;
   for (unsigned i = 0; i < ARRAY_SIZE(c.slot_of_unit); i++)
      c.slot_of_unit[i] = -1;
   c.blend_sa = -1;
   c.scale_sa = -1;
   c.varying_slot = varying_slot;
   c.nvaryings = nvaryings;
   memcpy(c.vs->varying_slot, varying_slot, nvaryings * sizeof(*varying_slot));
   c.vs->nvaryings = nvaryings;
   if (fix)
      memcpy(c.vs->vary_fix, fix, nvaryings);
   c.vary_fix = c.vs->vary_fix;
   /* (past what the shader declares: constants, never read) */
   for (unsigned a = 0; a < SGX_VS_MAX_ATTRIBS; a++)
      c.vs->attr[a] = a < nattr ? attr[a] : SGX_ATTR(SGX_ATTR_CONST, 4);
   memset(c.vs->attr_sa, 0xff, sizeof(c.vs->attr_sa));
   c.attr = c.vs->attr;

   s = nir_shader_clone(NULL, vs);
   NIR_PASS(_, s, nir_lower_io, nir_var_shader_in | nir_var_shader_out | nir_var_uniform,
            type_size_vec4, 0);
   if (getenv("SGX_DEBUG_VS_NIR"))
      nir_print_shader(s, stderr);
   if (!translate(&c, s))
      goto out;
   /* the draw's four words for the position, after the uniforms, then two
    * for each varying it scales (M39) */
   c.vs->clip_sa = c.nuniforms;
   c.nuniforms += 4;
   c.vs->fix_sa = c.nuniforms;
   for (unsigned k = 0; k < nvaryings; k++)
      c.nuniforms += c.vary_fix[k] & SGX_FIX_SCALE ? 2 : 0;
   if (c.nuniforms > 128) {
      fail(&c, "uniforms past word 128");
      goto out;
   }
   out_at = util_dynarray_num_elements(&c.code, uint64_t);
   vertex_output(&c);
   if (c.failed)
      goto out;
   /* branches: skipinv clear but on the vertex's output, as a pixel
    * program's (M20, M22) */
   if (c.branches)
      for (unsigned i = 0; i < out_at; i++)
         *util_dynarray_element(&c.code, uint64_t, i) &= ~(1ull << 55);

   c.vs->ncode = util_dynarray_num_elements(&c.code, uint64_t);
   c.vs->code = MALLOC(c.vs->ncode * sizeof(uint64_t));
   if (!c.vs->code) {
      fail(&c, "out of memory");
      goto out;
   }
   memcpy(c.vs->code, util_dynarray_begin(&c.code), c.vs->ncode * sizeof(uint64_t));
   /* a vertex's registers, attributes and temporaries (pa): a pixel's
    * bound (M20) */
   if (4 * MAX2(c.nattrs, 1) + c.top > 64) {
      fail(&c, "%u registers a vertex", 4 * MAX2(c.nattrs, 1) + c.top);
      goto out;
   }
   c.vs->ntemps = c.top;
   c.vs->branches = c.branches;
   c.vs->nattrs = MAX2(c.nattrs, 1);
   c.vs->nuniforms = c.nuniforms;
   c.vs->nubuf = c.ubuf ? c.uwords : 0;
   c.vs->ubuf_sa = c.ubuf_sa;
   c.vs->ubo_mask = c.ubo_mask;
   for (unsigned k = 0; k < SGX_FRAME_MAX_UBOS; k++)
      c.vs->ubo_sa[k] = c.ubo_mask >> k & 1 ? c.ubo_sa[k] : 0;

out:
   util_dynarray_fini(&c.code);
   free(c.loc);
   free(c.owned);
   free(c.last_use);
   free(c.def_at);
   util_dynarray_fini(&c.loops);
   ralloc_free(s);
   if (c.failed) {
      sgx_vs_destroy(c.vs);
      return NULL;
   }
   return c.vs;
}

void
sgx_vs_destroy(struct sgx_vs *vs)
{
   if (!vs)
      return;
   FREE(vs->code);
   FREE(vs);
}

void
sgx_fs_destroy(struct sgx_fs *fs)
{
   if (!fs)
      return;
   FREE(fs->prog.code);
   FREE(fs);
}

/* Short of registers, the shader compiled again in NIR's own order: values
 * made just before their use keep fewer alive as a rule, not always
 * (dEQP's tmp_array.vec3_dynamic_write_dynamic_read_fragment came to 67
 * registers a pixel with them, 64 without -- M36) */
static bool
short_of_registers(const char *why)
{
   return strstr(why, "registers a pixel") || strstr(why, "temporaries hold");
}

struct sgx_fs *
sgx_compile_fs(const nir_shader *fs, const struct sgx_blend_key *blend, char *why,
               unsigned why_size)
{
   struct sgx_fs *f = compile_fs(fs, blend, why, why_size, false);

   if (!f && short_of_registers(why))
      f = compile_fs(fs, blend, why, why_size, true);
   return f;
}

struct sgx_vs *
sgx_compile_vs(const nir_shader *vs, const unsigned *varying_slot, unsigned nvaryings,
               const uint8_t *attr, const uint8_t *fix, char *why, unsigned why_size)
{
   struct sgx_vs *v = compile_vs(vs, varying_slot, nvaryings, attr, fix, why, why_size, false);

   if (!v && short_of_registers(why))
      v = compile_vs(vs, varying_slot, nvaryings, attr, fix, why, why_size, true);
   return v;
}
