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
/* dest = src0 * src1 + src2, F32, one lane; src0 a temporary or primary
 * attribute, absolute only */
uint64_t usse_fmad(struct usse_reg dest, struct usse_reg src0, struct usse_reg src1,
                   struct usse_reg src2);
/* dest = op(src), F32, one lane */
uint64_t usse_fcomp(enum usse_comp_op op, struct usse_reg dest, struct usse_reg src);
/* dest = src, F32, one lane (any lanes) */
uint64_t usse_fmov(struct usse_reg dest, struct usse_reg src);
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
#define USSE_WDF0             0xf920000000000000ull   /* wait for data return channel 0 */

/* dest, dest + 1 (F32, dest even) = channels chan, chan + 1 (0 or 2) of
 * src's four 8-bit channels, scaled to [0, 1] -- the colour the tile holds,
 * read out of o0 for blending.  An F32 VPCK writes one 64-bit register:
 * two of these make the four channels (gltex/glblend with
 * SGX_DEBUG_FBFETCH: a mask of xyzw left z and w unwritten, and the first
 * channel select reaches channel 2 but not 3). */
uint64_t usse_unpack_unorm8(struct usse_reg dest, struct usse_reg src, unsigned chan);

/* o<dest> = four F32 values packed to 8 bits each, scaled from [0, 1]:
 * src, src + 1 (a pair) and src + 2, src + 3; src even */
uint64_t usse_pack_unorm8(unsigned dest_o, struct usse_reg src);
/* the same into another register (an output or a primary attribute) */
uint64_t usse_pack_unorm8_to(struct usse_reg dest, struct usse_reg src);

/* VTST: predicate p<pdst> = (src != 0), src F32, one lane (src - #0 by
 * VSUB, the zero test "non-zero"; iOS's c00_discard has the same form) */
uint64_t usse_vtst_ne0(unsigned pdst, struct usse_reg src);
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

#endif
