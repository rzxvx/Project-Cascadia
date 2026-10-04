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
/* o<dest> = four F32 values packed to 8 bits each, scaled from [0, 1]:
 * src, src + 1 (a pair) and src + 2, src + 3; src even */
uint64_t usse_pack_unorm8(unsigned dest_o, struct usse_reg src);

#endif
