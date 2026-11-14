/*
 * Copyright 2026 Project Cascadia
 * SPDX-License-Identifier: MIT
 *
 * An encoder for the SGX543's USSE instructions, the ones the compiler
 * emits (docs/research/p105-mesa.md, M13c).  Each instruction is 64 bits;
 * the field positions were read off iOS's own programs (the M11 corpus) and
 * are checked against them by mesa/host/usse-test.py.
 *
 * Registers are named in 32-bit units, as the PDS and VBW count them.  The
 * float instructions address 64-bit registers (a pair) and pick a lane in
 * it -- x the even register, y the odd one -- through their masks and
 * swizzles; the builders below take 32-bit numbers and work that out.
 *
 * No Mesa headers here, so the encoder can be built and tested on its own.
 */
#ifndef SGX_USSE_H
#define SGX_USSE_H

#include <stdbool.h>
#include <stdint.h>

enum usse_bank {
   USSE_TEMP,        /* r: temporaries */
   USSE_OUTPUT,      /* o: outputs */
   USSE_PA,          /* pa: primary attributes (what the PDS iterated) */
   USSE_SA,          /* sa: secondary attributes (uniforms) */
   USSE_SPECIAL,     /* c: the hardware's constants (sources only) */
   USSE_INDEX,       /* index1 (num 1), index2 (num 2): destinations only */
   USSE_IDX1,        /* a source through index1: num = bank << 5 | offset (bank 0
                      * temp, 1 output, 2 pa, 3 sa), 32-bit words */
   USSE_IMM,         /* (M44) an integer instruction's immediate source, num
                      * 0..127 (I32MAD2's src1 and src2) */
   USSE_INTERNAL,    /* (M47) an internal register's lane: num = 4 * register +
                      * lane, registers i0..i2 (F32 vec4s; the float
                      * instructions' fields name them as their bank's top
                      * temporaries).  Shared by the tasks on a pipe: alive
                      * only across instructions with usse_nosched() */
};

struct usse_reg {
   uint8_t bank;     /* enum usse_bank */
   uint8_t num;      /* 32-bit register number */
   bool neg, abs;    /* source modifiers, where the instruction has them */
};

static inline struct usse_reg
usse_reg(enum usse_bank bank, unsigned num)
{
   struct usse_reg r = { (uint8_t)bank, (uint8_t)num, false, false };
   return r;
}

#define USSE_NINTERNAL 3     /* i3's field writes nothing and reads i2 (M47) */

static inline struct usse_reg
usse_ireg(unsigned k, unsigned lane)
{
   struct usse_reg r = { (uint8_t)USSE_INTERNAL, (uint8_t)(4 * k + lane), false, false };

   return r;
}

#define USSE_END              (1ull << 50)  /* ends the program (not on PHAS) */
#define USSE_PHAS             0xfa44070000000000ull
#define USSE_NOP_END          0xf804014000000000ull

/* V32NMAD's second operation */
enum usse_nmad_op {
   USSE_NMAD_MUL, USSE_NMAD_ADD, USSE_NMAD_FRC, USSE_NMAD_DSX,
   USSE_NMAD_DSY, USSE_NMAD_MIN, USSE_NMAD_MAX, USSE_NMAD_DP,
};

/* VCOMP's operation */
enum usse_comp_op { USSE_COMP_RCP, USSE_COMP_RSQ, USSE_COMP_LOG2, USSE_COMP_EXP2 };

/* VMOV's conditional move: dest = src0 TEST 0 ? src1 : src2 */
enum usse_test { USSE_TEST_EQ0, USSE_TEST_NE0, USSE_TEST_LT0, USSE_TEST_LE0 };

/* One F32 lane each: dest = op(src1, src2) (src1 may be negated and
 * absolute, src2 absolute only).  dest, src1, src2 any lane. */
uint64_t usse_fop(enum usse_nmad_op op, struct usse_reg dest, struct usse_reg src1,
                  struct usse_reg src2);
/* (M46) src1 through a swizzle of its own: three bits a channel, x y z w
 * or the constants below (Vita3K's SwizzleChannel) */
#define USSE_K_0    4
#define USSE_K_1    5
#define USSE_K_2    6
#define USSE_K_HALF 7
#define USSE_SW_CONST(k) ((k) * 0x249)
uint64_t usse_fop_sw(enum usse_nmad_op op, struct usse_reg dest, struct usse_reg src1,
                     unsigned sw1, struct usse_reg src2);
/* (M47) V32NMAD in full: dest's lanes in mask (bit i lane i: an internal
 * register takes all four, a 64-bit register x and y), src1 through sw1
 * (three bits a channel, as usse_fop_sw's: the four channels read src1's
 * register and the three after it, an internal register's lanes), src2
 * through the swizzle table's sw2 (USSE_SW2_*: of a 64-bit register only x
 * and y read anything, z and w read 0; an internal register's all four) */
#define USSE_SW2_X    0       /* xxxx */
#define USSE_SW2_XYZW 4
#define USSE_SW2_ZWZW 11
#define USSE_SW(x, y, z, w) ((x) | (y) << 3 | (z) << 6 | (w) << 9)
uint64_t usse_fop_ex(enum usse_nmad_op op, struct usse_reg dest, unsigned mask,
                     struct usse_reg src1, unsigned sw1, struct usse_reg src2, unsigned sw2);
/* dest = src0 * src1 + src2, F32, one lane; src0 a temporary or primary
 * attribute, absolute only */
uint64_t usse_fmad(struct usse_reg dest, struct usse_reg src0, struct usse_reg src1,
                   struct usse_reg src2);
/* (M47) VMAD2 in full: dest = src0 * src1 + src2 on the lanes of mask (x and
 * y of a 64-bit register, an internal register's any), each source through
 * its own table's swizzle -- src0: xxxx yyyy zzzz wwww xyzw yzxw xyww zwxy,
 * src1: xxxx yyyy zzzz wwww xyzw xyyz yyww wyzw, src2: xxxx yyyy zzzz wwww
 * xyzw xzww xxyz xyzz (indices 0..7) */
uint64_t usse_fmad_ex(struct usse_reg dest, unsigned mask, struct usse_reg src0, unsigned sw0,
                      struct usse_reg src1, unsigned sw1, struct usse_reg src2, unsigned sw2);
/* (M47) VDP: dest = dot(src1, i<gpi0>) of three (dp3) or four (dp4)
 * channels, into the lanes of mask (an internal register's, or x or y of a
 * 64-bit register) -- src1 through sw1 (usse_fop_sw's channels, the
 * constants 0, 1, 2, 0.5 among them; its register and the three after),
 * with its negation and absolute value; the internal register through the
 * swizzle table's swg0 (dp4: USSE_SW2_*, 15 xyz1; dp3: 4 xyz) */
#define USSE_SWG_XYZ1 15
uint64_t usse_vdp(struct usse_reg dest, unsigned mask, bool dp4, struct usse_reg src1,
                  unsigned sw1, unsigned gpi0, unsigned swg0);
/* (M47) VMAD: dest = i<gpi0> * src1 + i<gpi1>, four channels (mad4), each
 * operand through the swizzle table (USSE_SW2_*; +16: the extended one --
 * 17 zwxy), dest's lanes in mask (an internal register's four, x and y of
 * a 64-bit register); gpi1 negated with neg1 */
#define USSE_SWX_ZWXY 17
uint64_t usse_vmad(struct usse_reg dest, unsigned mask, unsigned gpi0, unsigned swg0,
                   struct usse_reg src1, unsigned sw1, unsigned gpi1, unsigned swg1, bool neg1);
/* (M47) VTSTMSK: dest = src1 CMP src2 ? ~0 : 0 (mask type 1), F32, for each
 * lane of the 64-bit registers -- both of dest's lanes written; src1 may
 * be negated.  The comparisons IEEE's: -0 == 0, NaN unordered (only NE
 * true).  usse-probe.py found them (docs M47): the test is the
 * subtraction's sign test and zero test, both (bit 39) or either */
enum usse_cmp { USSE_CMP_LT, USSE_CMP_LE, USSE_CMP_EQ, USSE_CMP_NE, USSE_CMP_GE, USSE_CMP_GT };
uint64_t usse_vtstmsk_f32(struct usse_reg dest, struct usse_reg src1, struct usse_reg src2,
                          enum usse_cmp cmp);
/* (M47) w with its nosched flag set: no other task runs on the pipe between
 * this instruction and the next -- the internal registers, the pipe's,
 * keep what this task put there (bit 43 of the vector instructions, 54 of
 * VPCK, VTST, VBW, I32MAD2 and LIMM: each probed, docs M47).  False when
 * the instruction's flag is not known */
bool usse_nosched(uint64_t *w);
/* dest = op(src), F32, one lane */
uint64_t usse_fcomp(enum usse_comp_op op, struct usse_reg dest, struct usse_reg src);
/* dest = src, F32, one lane (any lanes) */
uint64_t usse_fmov(struct usse_reg dest, struct usse_reg src);
/* (M47) dest's pair = src's pair, F32, both lanes (dest, src even) */
uint64_t usse_fmov2(struct usse_reg dest, struct usse_reg src);
/* dest = src0 TEST 0 ? src1 : src2, F32, one lane; all four in the same
 * lane (one swizzle serves them) -- false if they are not */
bool usse_fmovc(uint64_t *out, enum usse_test test, struct usse_reg dest,
                struct usse_reg src0, struct usse_reg src1, struct usse_reg src2);
/* dest = a 32-bit immediate (any bank but special) */
uint64_t usse_limm(struct usse_reg dest, uint32_t value);
/* Texture sampling: SMP, then (before the result is read) WDF on the same
 * data return channel.  The texel lands in dest..: four registers as F32,
 * two as F16, one raw.  coords is a 64-bit register (F32: x the even
 * register, y the odd one), state the texture's four state words (in sa,
 * even), lod the bias or level for those modes (a register of its own). */
enum usse_smp_out { USSE_SMP_RAW = 0, USSE_SMP_F16 = 2, USSE_SMP_F32 = 3 };
enum usse_smp_coord { USSE_SMP_COORD_F32 = 0, USSE_SMP_COORD_F16 = 1 };
enum usse_smp_lod { USSE_SMP_NONE, USSE_SMP_BIAS, USSE_SMP_LOD };
uint64_t usse_smp2d(enum usse_smp_out out, enum usse_smp_coord coord, struct usse_reg dest,
                    struct usse_reg coords, struct usse_reg state, enum usse_smp_lod mode,
                    struct usse_reg lod);
/* the same with three coordinates (a cube map's direction: iOS's
 * textureCube is smp3d, the corpus's t04), F32 in coords..coords + 2 */
uint64_t usse_smp3d(enum usse_smp_out out, enum usse_smp_coord coord, struct usse_reg dest,
                    struct usse_reg coords, struct usse_reg state, enum usse_smp_lod mode,
                    struct usse_reg lod);
#define USSE_WDF0             0xf920000000000000ull   /* wait for data return channel 0 */

/* dest, dest + 1 (F32, dest even) = channels chan, chan + 1 (0 or 2) of
 * src's four 8-bit channels, scaled to [0, 1] -- the colour the tile holds,
 * read out of o0 for blending.  An F32 VPCK writes one 64-bit register:
 * two of these make the four channels (gltex/glblend with
 * SGX_DEBUG_FBFETCH: a mask of xyzw left z and w unwritten, and the first
 * channel select reaches channel 2 but not 3). */
uint64_t usse_unpack_unorm8(struct usse_reg dest, struct usse_reg src, unsigned chan);

/* (M43) F32 channels (src, src + 1 and src + 2, src + 3; src even) as F16
 * into dest.. -- mask 0xf: four, dest and dest + 1 (a target of half
 * floats' o0, o1: R G, B A); 0x3: two, dest.  One instruction: o1 written
 * by an instruction of its own never reached the target */
uint64_t usse_pack_f16(struct usse_reg dest, struct usse_reg src, unsigned mask);
/* (M43) F32 channels x, y of the pair src into dest, dest + 1 at once (a
 * VPCK F32 to F32): a target of two F32's o0, o1 */
uint64_t usse_pack_f32x2(struct usse_reg dest, struct usse_reg src);
/* dest, dest + 1 (F32, dest even) = F16 channels chan, chan + 1 (0 or 2)
 * of the four in src, src + 1 (src even): a target of half floats' tile,
 * o0 and o1, read for blending */
uint64_t usse_unpack_f16(struct usse_reg dest, struct usse_reg src, unsigned chan);

/* (M38) four F16 at once, each a register pair (four F16): src1 op src2,
 * channel for channel */
uint64_t usse_hop(enum usse_nmad_op op, struct usse_reg dest, struct usse_reg src1,
                  struct usse_reg src2);
/* (M46) the same in full: dest's channels in mask, src1 through sw1 (three
 * bits a channel: 0..3 x y z w, 4..7 the constants 0, 1, 2, 0.5) with its
 * negation and absolute value, src2 through the table's swizzle sw2 (4:
 * xyzw) */
uint64_t usse_hop_ex(enum usse_nmad_op op, struct usse_reg dest, unsigned mask,
                     struct usse_reg src1, unsigned sw1, struct usse_reg src2, unsigned sw2);
/* dest = the four F16 of the pair src packed to 8 bits each, scaled from
 * [0, 1] */
uint64_t usse_pack_unorm8_f16(struct usse_reg dest, struct usse_reg src);

/* o<dest> = four F32 values packed to 8 bits each, scaled from [0, 1]:
 * src, src + 1 (a pair) and src + 2, src + 3; src even */
uint64_t usse_pack_unorm8(unsigned dest_o, struct usse_reg src);
/* the same into another register (an output or a primary attribute) */
uint64_t usse_pack_unorm8_to(struct usse_reg dest, struct usse_reg src);

/* VTST: p<pdst> = (src & src) != 0 (eq: == 0) -- the bits of a 32-bit
 * register, in the form of iOS's facing test (`and(g16, #1) ne 0`): a
 * boolean (0.0 or 1.0) tested.  (An F32 test, src - #0 by VSUB, gave the
 * wrong answers: M20.) */
uint64_t usse_vtst_bits(unsigned pdst, struct usse_reg src, bool eq);
/* VBW, 32 bits: dest = src1 & imm or src1 | imm (imm 16 bits) -- moves of
 * raw words (`or o0, sa14, #0`), the index registers (`shr index1, r0, #5`,
 * iOS's c04_loop_break) */
uint64_t usse_vbw_and(struct usse_reg dest, struct usse_reg src1, unsigned imm);
uint64_t usse_vbw_or(struct usse_reg dest, struct usse_reg src1, unsigned imm);
/* dest = src1 | (imm rotated left by rot): bits past an immediate's 16
 * (M32: an LDR offset's multiplier, 4 << 16) */
uint64_t usse_vbw_or_rot(struct usse_reg dest, struct usse_reg src1, unsigned imm, unsigned rot);
/* VBW between two registers (M37, iOS's logic ops, es1 e03..e10): dest =
 * src1 op src2, src2 inverted first with not2 (`and o0, c56, ~o0`: c56 is
 * all ones).  (M44) And the shifts, Vita3K's op1 4 and 5: by src2's value
 * as it is -- 32 or more shifts every bit out (not taken modulo 32) */
enum usse_vbw_op {
   USSE_VBW_AND, USSE_VBW_OR, USSE_VBW_XOR, USSE_VBW_SHL, USSE_VBW_SHR, USSE_VBW_ASR,
};
uint64_t usse_vbw(enum usse_vbw_op op, struct usse_reg dest, struct usse_reg src1,
                  struct usse_reg src2, bool not2);
/* (M44) the same with src2 an immediate: 16 bits rotated left by rot */
uint64_t usse_vbw_imm(enum usse_vbw_op op, struct usse_reg dest, struct usse_reg src1,
                      unsigned imm, unsigned rot);
/* c56: the constant whose bits are all set; c0 is 0 (both as 32-bit
 * words: VBW's, VMOV's integer moves) */
#define USSE_ALL_ONES usse_reg(USSE_SPECIAL, 56)
#define USSE_ZERO     usse_reg(USSE_SPECIAL, 0)

/* (M44) an instruction run under an extended predicate (VBW, VMOV, VPCK,
 * VTST, I32MAD2 -- bits 58:56; not LIMM's, usse_limm_pred()): 1 p0, 2 p1,
 * 5 !p0, 6 !p1 */
uint64_t usse_pred(uint64_t w, unsigned pred);

/* (M44) I32MAD2, 32-bit integers: dest = src0 * src1 + src2, src1 and
 * src2 negated by their neg, each an immediate (USSE_IMM, unsigned 0..127)
 * or a register -- of src1 only the low 16 bits are multiplied, unsigned
 * (0x12345 * 0x10001 is 0x2468a less 0x12345); src0 temp, pa, output or sa */
uint64_t usse_imad(struct usse_reg dest, struct usse_reg src0, struct usse_reg src1,
                   struct usse_reg src2, bool is_signed);
/* (M44) VMOV of 32-bit integers (type INT32), conditional: dest = src0
 * TEST 0 ? src1 : src2, src0 tested as a signed integer (0x80000000 is
 * below 0).  Registers named in 32-bit units, 0..63 -- false (and no
 * word) when one is past that, or src0 is not a temporary, pa or output */
bool usse_imovc(uint64_t *out, enum usse_test test, struct usse_reg dest,
                struct usse_reg src0, struct usse_reg src1, struct usse_reg src2);
/* (M44) VTST: p<pdst> = src1 - src2 < 0 (lt) or == 0 (eq), as 32-bit
 * integers, signed or not -- the hardware's comparison, right where the
 * difference overflows (0x80000000 < 1, 0x7fffffff > -1).  src2 may be an
 * immediate, seven bits sign-extended (#64 is -64); src1 may not (#4 < 5
 * came out false) */
uint64_t usse_vtst_int(unsigned pdst, struct usse_reg src1, struct usse_reg src2,
                       bool is_signed, bool eq);
/* (M44) VTST on F32 values: p<pdst> = src1 - src2 < 0 or == 0 (-0 == 0);
 * src1 and src2 in the same lane (false and no word if not) */
bool usse_vtst_f32(uint64_t *out, unsigned pdst, struct usse_reg src1, struct usse_reg src2,
                   bool eq);
/* (M44) VPCK of a 16-bit half (half 0 the low one) of src, signed or not,
 * to F32 in dest (any lane) */
uint64_t usse_cvt16_f32(struct usse_reg dest, struct usse_reg src, unsigned half, bool is_signed);
/* (M44) VPCK of the F32 value src (any lane) into half `half` of dest, the
 * other half kept: S16 or U16, rounded to the nearest (2.5 to 2) and
 * saturated */
uint64_t usse_cvt_f32_16(struct usse_reg dest, struct usse_reg src, unsigned half, bool is_signed);

/* Memory loads (VLDST's, Vita3K's vldst; M32): count words in a row, from
 * the address in sa word base plus an offset plus 4 bytes, into dest..
 * (temporaries or pa).  The offset is an immediate in words (0..127) or a
 * temporary whose high half times its low half is bytes -- (4 << 16) |
 * word.  WDF0 before the words are read. */
uint64_t usse_ldr_imm(struct usse_reg dest, unsigned base, unsigned words, unsigned count);
uint64_t usse_ldr_reg(struct usse_reg dest, unsigned base, struct usse_reg offset, unsigned count);
/* KILL under a short predicate (0 always, 1 p0, 2 p1, 3 !p0): the
 * instruction that ends the first phase of iOS's c00_discard, with the
 * punch-through pass type in ISP state A.  Not used yet: here it kills
 * nothing (docs/research/p105-mesa.md, M19). */
uint64_t usse_kill(unsigned pred);
/* VTST: p<pdst> = the pixel's facing bit (bit 0 of special register g16,
 * iOS's v09_frontfacing: `and(g16, #1) ne 0`) */
uint64_t usse_vtst_facing(unsigned pdst);
/* LIMM under an extended predicate (0 none, 1 p0, 2 p1, 5 !p0, 6 !p1) */
uint64_t usse_limm_pred(struct usse_reg dest, uint32_t value, unsigned pred);

/* SOP2: two sources of four 8-bit channels (32-bit registers),
 * colour = cop(sel1 * src1.rgb, sel2 * src2.rgb), alpha = aop(asel1 *
 * src1.a, asel2 * src2.a), each factor 1 - itself with its mod -- GL's
 * blending, as iOS's pixel programs do it (M8; selects from Vita3K) */
enum {
   USSE_SOP2_ZERO,          /* colour selects */
   USSE_SOP2_SRC1,
   USSE_SOP2_SRC2,
   USSE_SOP2_SRC1_ALPHA,
   USSE_SOP2_SRC2_ALPHA,
   USSE_SOP2_SRC_ALPHA_SAT,
};
enum { USSE_SOP2_A_ZERO, USSE_SOP2_A_SRC1, USSE_SOP2_A_SRC2 };   /* alpha selects */
enum { USSE_SOP2_ADD, USSE_SOP2_SUB, USSE_SOP2_MIN, USSE_SOP2_MAX };
struct usse_sop2 {
   uint8_t csel1, cmod1, csel2, cmod2;
   uint8_t asel1, amod1, asel2, amod2;
   uint8_t cop, aop;
};
uint64_t usse_sop2(struct usse_reg dest, struct usse_reg src1, struct usse_reg src2,
                   const struct usse_sop2 *f);

#endif
