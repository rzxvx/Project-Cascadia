/*
 * Copyright 2026 Project Cascadia
 * SPDX-License-Identifier: MIT
 *
 * The USSE encoder (sgx_usse.h).  Fields are set by their bit positions;
 * the names in the comments are what each field does, as iOS's programs
 * show it (docs/research/p105-mesa.md, M13c).
 */
#include "sgx_usse.h"

#include <assert.h>
#include <stdio.h>

/* v into bits hi..lo */
static uint64_t
bits(uint64_t v, unsigned hi, unsigned lo)
{
   if (v >> (hi - lo + 1)) {
      fprintf(stderr, "sgx_usse: %#llx does not fit bits %u:%u\n", (unsigned long long)v, hi, lo);
      assert(!"a USSE field overflows");
   }
   return v << lo;
}

#define SKIPINV  (1ull << 55)   /* set on every instruction iOS's driver writes */

/* A source in the two-bit bank selects most operands have, with the
 * extension bit beside them: temp, output, pa, sa; special in the
 * extended set. */
static void
src_bank(struct usse_reg r, unsigned *ext, unsigned *bank)
{
   switch (r.bank) {
   case USSE_TEMP:    *ext = 0; *bank = 0; break;
   case USSE_OUTPUT:  *ext = 0; *bank = 1; break;
   case USSE_PA:      *ext = 0; *bank = 2; break;
   case USSE_SA:      *ext = 0; *bank = 3; break;
   case USSE_SPECIAL: *ext = 1; *bank = 1; break;
   case USSE_IDX1:    *ext = 1; *bank = 0; break;
   default:           assert(0); *ext = *bank = 0; break;
   }
}

/* a destination: temp, output, pa; sa in the extended set */
static void
dest_bank(struct usse_reg r, unsigned *ext, unsigned *bank)
{
   switch (r.bank) {
   case USSE_TEMP:   *ext = 0; *bank = 0; break;
   case USSE_OUTPUT: *ext = 0; *bank = 1; break;
   case USSE_PA:     *ext = 0; *bank = 2; break;
   case USSE_SA:     *ext = 1; *bank = 0; break;
   case USSE_INDEX:  *ext = 1; *bank = 2; break;
   default:          assert(0); *ext = *bank = 0; break;
   }
}

/* the 64-bit register a float operand names, and its lane */
static unsigned
pair(struct usse_reg r)
{
   return r.bank == USSE_SPECIAL ? r.num : r.num >> 1;
}

static unsigned
lane(struct usse_reg r)
{
   return r.bank == USSE_SPECIAL ? 0 : r.num & 1;
}

static unsigned
modifier(struct usse_reg r)      /* none, -, |.|, -|.| */
{
   return (unsigned)r.neg | (unsigned)r.abs << 1;
}

uint64_t
usse_fop(enum usse_nmad_op op, struct usse_reg dest, struct usse_reg src1,
         struct usse_reg src2)
{
   unsigned dext, dbank, ext1, bank1, ext2, bank2;
   /* src1's swizzle: three bits a channel (x y z w 0 1 2 0.5), all four
    * the lane it is in; src2's from a table, xxxx or yyyy */
   unsigned sw1 = lane(src1) * 0x249, sw2 = lane(src2);

   dest_bank(dest, &dext, &dbank);
   src_bank(src1, &ext1, &bank1);
   src_bank(src2, &ext2, &bank2);
   assert(!src2.neg);
   return bits(0x01, 63, 59) | SKIPINV |
          bits(sw1 >> 10 & 3, 54, 53) | bits(dext, 51, 51) | bits(sw1 >> 9 & 1, 50, 50) |
          bits(ext1, 49, 49) | bits(ext2, 48, 48) | bits(sw2, 47, 44) |
          bits(1u << lane(dest), 42, 39) |                /* the dest mask */
          bits(modifier(src1), 38, 37) | bits(src2.abs, 36, 36) |
          bits(sw1 >> 7 & 3, 35, 34) | bits(dbank, 33, 32) | bits(bank1, 31, 30) |
          bits(bank2, 29, 28) | bits(pair(dest), 27, 22) | bits(sw1 & 0x7f, 21, 15) |
          bits(op, 14, 12) | bits(pair(src1), 11, 6) | bits(pair(src2), 5, 0);
}

uint64_t
usse_fmad(struct usse_reg dest, struct usse_reg src0, struct usse_reg src1,
          struct usse_reg src2)
{
   unsigned ext1, bank1, ext2, bank2;

   /* dest and src0 without an extended bank: temp, output, pa; temp, pa */
   assert(dest.bank == USSE_TEMP || dest.bank == USSE_OUTPUT || dest.bank == USSE_PA);
   assert((src0.bank == USSE_TEMP || src0.bank == USSE_PA) && !src0.neg);
   src_bank(src1, &ext1, &bank1);
   src_bank(src2, &ext2, &bank2);
   /* swizzles: xxxx or yyyy, from three tables that start the same way */
   return bits(0x00, 63, 59) | bits(0, 58, 58) /* F32 */ | SKIPINV |
          bits(0, 53, 53) | bits(src0.abs, 50, 50) | bits(ext1, 49, 49) | bits(ext2, 48, 48) |
          bits(lane(src2), 47, 45) | bits(0, 44, 44) | bits(1u << lane(dest), 42, 39) |
          bits(modifier(src1), 38, 37) | bits(modifier(src2), 36, 35) |
          bits(src0.bank == USSE_PA, 34, 34) | bits(dest.bank, 33, 32) |
          bits(bank1, 31, 30) | bits(bank2, 29, 28) | bits(pair(dest), 27, 22) |
          bits(lane(src1), 21, 20) | bits(lane(src0), 19, 18) | bits(pair(src0), 17, 12) |
          bits(pair(src1), 11, 6) | bits(pair(src2), 5, 0);
}

uint64_t
usse_fcomp(enum usse_comp_op op, struct usse_reg dest, struct usse_reg src)
{
   unsigned dext, dbank, ext, bank;

   dest_bank(dest, &dext, &dbank);
   src_bank(src, &ext, &bank);
   /* types 0 (F32) for dest (54:53) and source (40:39) */
   return bits(0x06, 63, 59) | SKIPINV | bits(dext, 51, 51) | bits(ext, 49, 49) |
          bits(op, 42, 41) | bits(modifier(src), 38, 37) | bits(lane(src), 36, 35) |
          bits(dbank, 33, 32) | bits(bank, 31, 30) | bits(pair(dest), 27, 21) |
          bits(pair(src), 13, 7) | bits(1u << lane(dest), 3, 0);
}

/* VMOV: kind 0 a move, 1 a conditional move; type 5 F32 */
static uint64_t
vmov(unsigned kind, unsigned test, struct usse_reg dest, struct usse_reg src0,
     struct usse_reg src1, struct usse_reg src2, unsigned swizzle)
{
   unsigned dext, dbank, ext1, bank1, ext2, bank2;
   /* src0: temp, pa; output, sa extended */
   unsigned ext0 = src0.bank == USSE_OUTPUT || src0.bank == USSE_SA;
   unsigned bank0 = src0.bank == USSE_PA || src0.bank == USSE_SA;

   dest_bank(dest, &dext, &dbank);
   src_bank(src1, &ext1, &bank1);
   src_bank(src2, &ext2, &bank2);
   return bits(0x07, 63, 59) | SKIPINV | bits(test >> 1, 54, 54) |
          bits(kind != 0, 53, 53) /* the swizzle on src0 too */ | bits(dext, 51, 51) |
          bits(ext0, 50, 50) | bits(ext1, 49, 49) | bits(ext2, 48, 48) | bits(kind, 47, 46) |
          bits(5, 42, 40) | bits(test & 1, 39, 39) | bits(swizzle, 38, 35) |
          bits(bank0, 34, 34) | bits(dbank, 33, 32) | bits(bank1, 31, 30) |
          bits(bank2, 29, 28) | bits(1u << lane(dest), 27, 24) | bits(pair(dest), 23, 18) |
          bits(pair(src0), 17, 12) | bits(pair(src1), 11, 6) | bits(pair(src2), 5, 0);
}

uint64_t
usse_fmov(struct usse_reg dest, struct usse_reg src)
{
   /* swizzle xxxx or yyyy: the source's lane, into the dest's */
   return vmov(0, 0, dest, usse_reg(USSE_TEMP, 0), src, usse_reg(USSE_TEMP, 0), lane(src));
}

bool
usse_fmovc(uint64_t *out, enum usse_test test, struct usse_reg dest, struct usse_reg src0,
           struct usse_reg src1, struct usse_reg src2)
{
   /* (src0 in sa reads 0, M31) */
   if (lane(src0) != lane(src1) || lane(src1) != lane(src2) ||
       (src0.bank != USSE_TEMP && src0.bank != USSE_PA && src0.bank != USSE_OUTPUT))
      return false;
   *out = vmov(1, test, dest, src0, src1, src2, lane(src1));
   return true;
}

uint64_t
usse_limm(struct usse_reg dest, uint32_t value)
{
   unsigned dext, dbank;

   dest_bank(dest, &dext, &dbank);
   assert(!dext);
   /* the immediate: 31:26 in 49:44, 25:21 in 40:36, 20:0 in 20:0 */
   return 0xfca0000000000000ull | bits(value >> 26, 49, 44) |
          bits(value >> 21 & 0x1f, 40, 36) | bits(dbank, 33, 32) | bits(dest.num, 27, 21) |
          bits(value & 0x1fffff, 20, 0);
}

uint64_t
usse_pack_unorm8(unsigned dest_o, struct usse_reg src)
{
   return usse_pack_unorm8_to(usse_reg(USSE_OUTPUT, dest_o), src);
}

uint64_t
usse_pack_unorm8_to(struct usse_reg dest, struct usse_reg src)
{
   unsigned ext, bank, dext, dbank;

   assert(!(src.num & 1));
   src_bank(src, &ext, &bank);
   dest_bank(dest, &dext, &dbank);
   /* VPCK: source format 6 (F32), dest 0 (U8), mask xyzw, scale; channels
    * x y z w from src (x y) and src + 2 (z w), the second source's bank
    * the first's */
   return bits(0x08, 63, 59) | SKIPINV | bits(dext, 51, 51) | bits(ext, 49, 49) |
          bits(ext, 48, 48) | bits(6, 43, 41) | bits(0, 40, 38) | bits(0xf, 37, 34) |
          bits(dbank, 33, 32) | bits(bank, 31, 30) | bits(bank, 29, 28) | bits(dest.num, 27, 21) |
          bits(3, 20, 19) | bits(1, 18, 18) | bits(1, 17, 16) | bits(2, 15, 14) |
          bits(src.num >> 1, 13, 8) | bits(0, 7, 7) | bits((src.num >> 1) + 1, 6, 1);
}

static uint64_t
smp(unsigned dim, enum usse_smp_out out, enum usse_smp_coord coord, struct usse_reg dest,
    struct usse_reg coords, struct usse_reg state, enum usse_smp_lod mode, struct usse_reg lod)
{
   unsigned ext1, bank1, ext2 = 1, bank2 = 2, lodn = 0;   /* no lod: immediate 0 */
   /* coords: temp, pa; output, sa extended */
   unsigned ext0 = coords.bank == USSE_OUTPUT || coords.bank == USSE_SA;
   unsigned bank0 = coords.bank == USSE_PA || coords.bank == USSE_SA;

   assert(dest.bank == USSE_TEMP || dest.bank == USSE_PA);
   assert(!(coords.num & 1) && !(state.num & 1));
   src_bank(state, &ext1, &bank1);
   if (mode != USSE_SMP_NONE) {
      src_bank(lod, &ext2, &bank2);
      lodn = pair(lod);
   }
   /* skipinv clear, as iOS's sampling instructions have it; dimension
    * (1 2D, 2 3D: t01, t04); data return channel 0 */
   return bits(0x1c, 63, 59) | bits(ext0, 50, 50) | bits(ext1, 49, 49) | bits(ext2, 48, 48) |
          bits(out, 47, 46) | bits(dim, 43, 42) | bits(mode, 41, 40) |
          bits(dest.bank == USSE_PA, 39, 39) | bits(coord, 36, 35) | bits(bank0, 34, 34) |
          bits(bank1, 31, 30) | bits(bank2, 29, 28) | bits(dest.num, 27, 21) |
          bits(coords.num >> 1, 20, 14) | bits(state.num >> 1, 13, 7) | bits(lodn, 6, 0);
}

uint64_t
usse_smp2d(enum usse_smp_out out, enum usse_smp_coord coord, struct usse_reg dest,
           struct usse_reg coords, struct usse_reg state, enum usse_smp_lod mode,
           struct usse_reg lod)
{
   return smp(1, out, coord, dest, coords, state, mode, lod);
}

uint64_t
usse_smp3d(enum usse_smp_out out, enum usse_smp_coord coord, struct usse_reg dest,
           struct usse_reg coords, struct usse_reg state, enum usse_smp_lod mode,
           struct usse_reg lod)
{
   return smp(2, out, coord, dest, coords, state, mode, lod);
}

uint64_t
usse_unpack_unorm8(struct usse_reg dest, struct usse_reg src, unsigned chan)
{
   unsigned dext, dbank, ext, bank;

   assert(!(dest.num & 1) && (chan == 0 || chan == 2));
   dest_bank(dest, &dext, &dbank);
   src_bank(src, &ext, &bank);
   /* VPCK: source format 0 (U8), dest 6 (F32), mask xy, scale.  A U8 source
    * is named in 32-bit units (its low bit at 7).  x reads channel chan --
    * the select's low bit at 0, its high bit the second source's low bit
    * (an immediate here, as iOS's pck.f16.u8 has it) -- y reads chan + 1 */
   return bits(0x08, 63, 59) | SKIPINV | bits(dext, 51, 51) | bits(ext, 49, 49) |
          bits(1, 48, 48) | bits(0, 43, 41) | bits(6, 40, 38) | bits(3, 37, 34) |
          bits(dbank, 33, 32) | bits(bank, 31, 30) | bits(2, 29, 28) | bits(dest.num, 27, 21) |
          bits(3, 20, 19) | bits(1, 18, 18) | bits(chan + 1, 17, 16) | bits(2, 15, 14) |
          bits(src.num >> 1, 13, 8) | bits(src.num & 1, 7, 7) | bits(chan >> 1, 6, 1);
}

uint64_t
usse_vtst_facing(unsigned pdst)
{
   /* iOS's word: bitwise ALU (3) op 0 (AND), src1 special 80, src2 the
    * immediate 1, the zero test non-zero */
   return (0x488b0281600c2801ull & ~bits(3, 35, 34)) | bits(pdst, 35, 34);
}

uint64_t
usse_limm_pred(struct usse_reg dest, uint32_t value, unsigned pred)
{
   return usse_limm(dest, value) | bits(pred, 43, 41);
}

uint64_t
usse_vtst_bits(unsigned pdst, struct usse_reg src, bool eq)
{
   unsigned ext, bank;

   src_bank(src, &ext, &bank);
   return (0x488b0281600c2801ull & ~(bits(3, 49, 48) | bits(3, 41, 40) | bits(3, 35, 34) |
                                     bits(0xf, 31, 28) | bits(0x3fff, 13, 0))) |
          bits(ext, 49, 49) | bits(ext, 48, 48) | bits(eq ? 1 : 2, 41, 40) |
          bits(pdst, 35, 34) | bits(bank, 31, 30) | bits(bank, 29, 28) |
          bits(src.num, 13, 7) | bits(src.num, 6, 0);
}

uint64_t
usse_kill(unsigned pred)
{
   /* iOS's word less its end bit and its predicate */
   return (0xf9340426c0000280ull & ~USSE_END & ~bits(3, 42, 41)) | bits(pred, 42, 41);
}

/* VBW's op 2 (and, or by bit 35), src2 the immediate (extended bank 2):
 * its bits 15:14 at 37:36, 13:7 at 20:14, 6:0 at 6:0 */
static uint64_t
vbw_logic(bool or_, struct usse_reg dest, struct usse_reg src1, unsigned imm)
{
   unsigned dext, dbank, ext, bank;

   dest_bank(dest, &dext, &dbank);
   src_bank(src1, &ext, &bank);
   return bits(1, 63, 62) | bits(2, 61, 59) | SKIPINV | bits(dext, 51, 51) | bits(ext, 49, 49) |
          bits(1, 48, 48) | bits(imm >> 14, 37, 36) | bits(or_, 35, 35) | bits(dbank, 33, 32) |
          bits(bank, 31, 30) | bits(2, 29, 28) | bits(dest.num, 27, 21) |
          bits(imm >> 7 & 0x7f, 20, 14) | bits(src1.num, 13, 7) | bits(imm & 0x7f, 6, 0);
}

uint64_t
usse_vbw_or_rot(struct usse_reg dest, struct usse_reg src1, unsigned imm, unsigned rot)
{
   /* (Vita3K's src2_rot, bits 42:38) */
   return vbw_logic(true, dest, src1, imm) | bits(rot, 42, 38);
}

/* VLDST: 111, op 1 (a load), no MOE expansion (with it, more than a word
 * loaded nothing), src0 sa (extended bank 1), src2 the immediate 0 (not
 * added), F32 words; src1 an immediate (extended bank 2) or a register */
static uint64_t
ldr(struct usse_reg dest, unsigned base, unsigned src1_ext, unsigned src1_bank, unsigned src1,
    unsigned count)
{
   assert(dest.bank == USSE_TEMP || dest.bank == USSE_PA);
   assert(count >= 1 && count <= 16 && base < 128);
   return bits(7, 63, 61) | bits(1, 60, 59) | SKIPINV | bits(1, 50, 50) | bits(src1_ext, 49, 49) |
          bits(1, 48, 48) | bits(count - 1, 47, 44) | bits(dest.bank == USSE_PA, 39, 39) |
          bits(1, 34, 34) | bits(src1_bank, 31, 30) | bits(2, 29, 28) | bits(dest.num, 27, 21) |
          bits(base, 20, 14) | bits(src1, 13, 7);
}

uint64_t
usse_ldr_imm(struct usse_reg dest, unsigned base, unsigned words, unsigned count)
{
   assert(words < 128);
   return ldr(dest, base, 1, 2, words, count);
}

uint64_t
usse_ldr_reg(struct usse_reg dest, unsigned base, struct usse_reg offset, unsigned count)
{
   unsigned ext, bank;

   src_bank(offset, &ext, &bank);
   return ldr(dest, base, ext, bank, offset.num, count);
}

uint64_t
usse_vbw_and(struct usse_reg dest, struct usse_reg src1, unsigned imm)
{
   return vbw_logic(false, dest, src1, imm);
}

uint64_t
usse_vbw_or(struct usse_reg dest, struct usse_reg src1, unsigned imm)
{
   return vbw_logic(true, dest, src1, imm);
}

uint64_t
usse_sop2(struct usse_reg dest, struct usse_reg src1, struct usse_reg src2,
          const struct usse_sop2 *f)
{
   unsigned dext, dbank, ext1, bank1, ext2, bank2;

   dest_bank(dest, &dext, &dbank);
   src_bank(src1, &ext1, &bank1);
   src_bank(src2, &ext2, &bank2);
   /* Vita3K's SOP2 (alu.cpp): 10000 pp c(cmod1) s n aa(asel1) d e r b
    * m(cmod2) ooo(count) f(amod1) ll(asel2) ggg(csel1) hhh(csel2) i(amod2)
    * tt kk jj qqqqqqq(dest) u(src1 mod) vv(cop) ww(aop) x y zzzzzzz(src1)
    * AAAAAAA(src2) -- register numbers in 32-bit words */
   return bits(0x10, 63, 59) | bits(f->cmod1, 56, 56) | SKIPINV | bits(f->asel1, 53, 52) |
          bits(dext, 51, 51) | bits(ext1, 49, 49) | bits(ext2, 48, 48) |
          bits(f->cmod2, 47, 47) | bits(f->amod1, 43, 43) | bits(f->asel2, 42, 41) |
          bits(f->csel1, 40, 38) | bits(f->csel2, 37, 35) | bits(f->amod2, 34, 34) |
          bits(dbank, 33, 32) | bits(bank1, 31, 30) | bits(bank2, 29, 28) |
          bits(dest.num, 27, 21) | bits(f->cop, 19, 18) | bits(f->aop, 17, 16) |
          bits(src1.num, 13, 7) | bits(src2.num, 6, 0);
}
