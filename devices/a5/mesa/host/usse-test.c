/*
 * Copyright 2026 Project Cascadia
 * SPDX-License-Identifier: MIT
 *
 * usse-test.c -- prints what the USSE encoder (sgx_usse.c) makes of each
 * case usse-test.py checks: "name word", one a line.
 */
#include <inttypes.h>
#include <stdio.h>

#include "sgx_usse.h"

static struct usse_reg
neg(struct usse_reg r)
{
   r.neg = true;
   return r;
}

#define T(n)  usse_reg(USSE_TEMP, n)
#define I(n)  usse_reg(USSE_IMM, n)
#define PA(n) usse_reg(USSE_PA, n)
#define SA(n) usse_reg(USSE_SA, n)
#define O(n)  usse_reg(USSE_OUTPUT, n)
#define P(name, w) printf("%s %016" PRIx64 "\n", name, (uint64_t)(w))

int
main(void)
{
   uint64_t w;

   P("pack_pa0", usse_pack_unorm8(0, PA(0)) | USSE_END);
   P("pack_r8", usse_pack_unorm8(0, T(8)) | USSE_END);
   P("rcp_pa10_pa5", usse_fcomp(USSE_COMP_RCP, PA(10), PA(5)));
   P("rcp_pa11_pa6", usse_fcomp(USSE_COMP_RCP, PA(11), PA(6)));
   P("rcp_pa12_pa7", usse_fcomp(USSE_COMP_RCP, PA(12), PA(7)));
   P("rcp_pa13_pa8", usse_fcomp(USSE_COMP_RCP, PA(13), PA(8)));
   P("rsq_r4_sa3", usse_fcomp(USSE_COMP_RSQ, T(4), SA(3)));
   P("exp_r5_r2", usse_fcomp(USSE_COMP_EXP2, T(5), T(2)));
   P("add_r4_r6_pa5", usse_fop(USSE_NMAD_ADD, T(4), T(6), PA(5)));
   P("mul_r5_nsa3_r8", usse_fop(USSE_NMAD_MUL, T(5), neg(SA(3)), T(8)));
   P("min_r10_pa1_sa0", usse_fop(USSE_NMAD_MIN, T(10), PA(1), SA(0)));
   P("max_o2_r3_r4", usse_fop(USSE_NMAD_MAX, O(2), T(3), T(4)));
   P("frc_r2_r3_r3", usse_fop(USSE_NMAD_FRC, T(2), T(3), T(3)));
   P("mad_r2_pa1_sa4_r7", usse_fmad(T(2), PA(1), SA(4), T(7)));
   P("mad_r3_r0_nr2_sa1", usse_fmad(T(3), T(0), neg(T(2)), SA(1)));
   P("mov_r3_pa4", usse_fmov(T(3), PA(4)));
   P("mov_r2_sa7", usse_fmov(T(2), SA(7)));
   usse_fmovc(&w, USSE_TEST_EQ0, T(2), T(4), T(6), T(8));
   P("movc_eq", w);
   usse_fmovc(&w, USSE_TEST_LT0, T(3), PA(5), SA(1), T(9));
   P("movc_lt", w);
   P("smp_ios", usse_smp2d(USSE_SMP_RAW, USSE_SMP_COORD_F16, PA(0), PA(0), SA(6), USSE_SMP_NONE,
                           T(0)));
   P("smp_f32", usse_smp2d(USSE_SMP_F32, USSE_SMP_COORD_F32, T(8), T(4), SA(12), USSE_SMP_NONE,
                           T(0)));
   P("smp_bias", usse_smp2d(USSE_SMP_F32, USSE_SMP_COORD_F32, T(8), T(4), SA(12), USSE_SMP_BIAS,
                            T(6)));
   P("wdf0", USSE_WDF0);
   P("unpack_r4_o0_xy", usse_unpack_unorm8(T(4), O(0), 0));
   P("unpack_r6_o0_zw", usse_unpack_unorm8(T(6), O(0), 2));
   P("limm_r5", usse_limm(T(5), 0x3f800000));
   P("limm_r100", usse_limm(T(100), 0xdeadbeef));
   P("vtst_r5", usse_vtst_bits(1, T(5), false));
   P("vtst_pa4", usse_vtst_bits(0, PA(4), true));
   P("kill_p1_end", usse_kill(2) | USSE_END);
   P("or_o0_sa14", usse_vbw_or(O(0), SA(14), 0) | USSE_END);
   P("mul_f16_pa0_pa2_pa0", usse_hop(USSE_NMAD_MUL, PA(0), PA(2), PA(0)));
   P("mul_f16_pa0_pa0_pa2", usse_hop(USSE_NMAD_MUL, PA(0), PA(0), PA(2)));
   P("mul_f16_pa0_pa0_sa6", usse_hop(USSE_NMAD_MUL, PA(0), PA(0), SA(6)));
   /* (M46) masks, src1's swizzle with its constants, negation, src2's table */
   P("mul_f16_r0xyz", usse_hop_ex(USSE_NMAD_MUL, T(0), 0x7, PA(0), 0 | 1 << 3 | 2 << 6 | 3 << 9,
                                  PA(2), 4));
   P("mul_f16_2221", usse_hop_ex(USSE_NMAD_MUL, T(2), 0xf, PA(0), 6 | 6 << 3 | 6 << 6 | 5 << 9,
                                 T(0), 4));
   P("max_f16_0000", usse_hop_ex(USSE_NMAD_MAX, T(2), 0xf, PA(0), 4 | 4 << 3 | 4 << 6 | 4 << 9,
                                 T(2), 4));
   P("add_f16_neg_sa4", usse_hop_ex(USSE_NMAD_ADD, T(4), 0xf,
                                    (struct usse_reg){ .bank = USSE_SA, .num = 4, .neg = true },
                                    0 | 1 << 3 | 2 << 6 | 3 << 9, T(2), 4));
   P("mul_f16_xxxx_src2", usse_hop_ex(USSE_NMAD_MUL, T(6), 0x8, T(4), 3 | 2 << 3 | 1 << 6 | 0 << 9,
                                      PA(2), 0));
   P("pack_f16_o0_pa0", usse_pack_unorm8_f16(O(0), PA(0)) | USSE_END);
   P("pack_f16_pa0_pa0", usse_pack_unorm8_f16(PA(0), PA(0)));
   P("xor_o0_pa0_o0", usse_vbw(USSE_VBW_XOR, O(0), PA(0), O(0), false) | USSE_END);
   P("and_o0_pa0_o0", usse_vbw(USSE_VBW_AND, O(0), PA(0), O(0), false) | USSE_END);
   P("or_o0_pa0_o0", usse_vbw(USSE_VBW_OR, O(0), PA(0), O(0), false) | USSE_END);
   P("not_o0", usse_vbw(USSE_VBW_AND, O(0), USSE_ALL_ONES, O(0), true) | USSE_END);
   P("not_pa0", usse_vbw(USSE_VBW_AND, O(0), USSE_ALL_ONES, PA(0), true) | USSE_END);
   P("and_r0_pa0_o0", usse_vbw(USSE_VBW_AND, T(0), PA(0), O(0), false));
   P("set_o0", usse_vbw(USSE_VBW_OR, O(0), O(0), O(0), true) | USSE_END);
   P("clear_o0", usse_vbw(USSE_VBW_XOR, O(0), O(0), O(0), false) | USSE_END);
   P("and_r2_r0", usse_vbw_and(T(2), T(0), 0x1f) & ~(1ull << 55));
   P("and_index1", usse_vbw_and(usse_reg(USSE_INDEX, 1), T(6), 0xffff));
   P("or_r3_idx1_sa2", usse_vbw_or(T(3), usse_reg(USSE_IDX1, 0x60 | 2), 0));
   /* M44: the integer instructions, as glldr --probe ran them */
   P("imad_r0_r2_r4_r2", usse_imad(T(0), T(2), T(4), T(2), true));
   P("isub_r0_r2_r4", usse_imad(T(0), T(2), I(1), neg(T(4)), true));
   P("ineg_r0_r2", usse_imad(T(0), T(2), neg(I(1)), I(0), true));
   P("shl_r0_r2_3", usse_vbw_imm(USSE_VBW_SHL, T(0), T(2), 3, 0));
   P("asr_r0_r2_r4", usse_vbw(USSE_VBW_ASR, T(0), T(2), T(4), false));
   P("xor_r0_r2_sign", usse_vbw_imm(USSE_VBW_XOR, T(0), T(2), 0x8000, 16));
   P("not_r0_r2", usse_vbw(USSE_VBW_XOR, T(0), T(2), USSE_ALL_ONES, false));
   usse_imovc(&w, USSE_TEST_LT0, T(0), T(2), T(4), T(6));
   P("imovc_lt", w);
   usse_imovc(&w, USSE_TEST_EQ0, T(0), T(2), USSE_ALL_ONES, USSE_ZERO);
   P("imovc_bool", w);
   P("vtst_slt", usse_vtst_int(0, T(2), T(4), true, false));
   P("vtst_ult", usse_vtst_int(0, T(2), T(4), false, false));
   P("vtst_ieq", usse_vtst_int(0, T(2), T(4), true, true));
   usse_vtst_f32(&w, 0, T(2), T(4), false);
   P("vtst_flt", w);
   usse_vtst_f32(&w, 0, T(3), T(5), false);
   P("vtst_flt_y", w);
   P("cvt_s16hi_f32", usse_cvt16_f32(T(0), T(2), 1, true));
   P("cvt_u16lo_f32", usse_cvt16_f32(T(0), T(2), 0, false));
   P("cvt_f32_s16lo", usse_cvt_f32_16(T(0), T(2), 0, true));
   P("cvt_f32_s16hi", usse_cvt_f32_16(T(0), T(2), 1, true));
   P("limm_p0", usse_limm_pred(T(0), 0xffffffff, 1));
   /* M47: the vector instructions and the internal registers, as
    * usse-probe.py ran them */
   {
      const unsigned xyzw = USSE_SW(0, 1, 2, 3);

      P("vdp_r0_r2_i0", usse_vdp(T(0), 1, true, T(2), xyzw, 0, USSE_SW2_XYZW));
      P("vdp_xyz1", usse_vdp(T(0), 1, true, T(2), xyzw, 0, USSE_SWG_XYZ1));
      P("vdp_i1_y", usse_vdp(usse_ireg(1, 1), 2, true, T(2), xyzw, 0, USSE_SW2_XYZW));
      P("vmad_i2", usse_vmad(usse_ireg(2, 0), 0xf, 0, USSE_SW2_XYZW, T(4), USSE_SW2_XYZW, 1,
                             USSE_SW2_XYZW, false));
      P("vmad_r0_ext", usse_vmad(T(0), 0xf, 0, USSE_SW2_X, T(4), USSE_SWX_ZWXY, 1, USSE_SWX_ZWXY,
                                 false));
      P("add_i0_r2_c0", usse_fop_ex(USSE_NMAD_ADD, usse_ireg(0, 0), 0xf, T(2), xyzw, USSE_ZERO,
                                    USSE_SW2_X));
      P("mul_r0_r2_i0z", usse_fop(USSE_NMAD_MUL, T(0), T(2), usse_ireg(0, 2)));
      P("rcp_r0_i0w", usse_fcomp(USSE_COMP_RCP, T(0), usse_ireg(0, 3)));
      P("mov_r0_i0z", usse_fmov(T(0), usse_ireg(0, 2)));
      P("mad_i1w_i0y", usse_fmad(usse_ireg(1, 3), usse_ireg(0, 1), SA(5), usse_ireg(0, 2)));
      P("vmad2_r0_r2_r4_r6", usse_fmad_ex(T(0), 3, T(2), 4, T(4), 4, T(6), 4));
      P("vmad2_bcast_sa", usse_fmad_ex(PA(20), 3, PA(5), 1, SA(4), 4, PA(16), 4));
      P("vmad2_i0zw", usse_fmad_ex(T(8), 3, usse_ireg(0, 0), 7, T(4), 4, T(6), 4));
      P("add2_zwzw", usse_fop_ex(USSE_NMAD_ADD, T(0), 3, T(2), USSE_SW(2, 3, 2, 3), T(4),
                                 USSE_SW2_ZWZW));
      P("tstmsk_lt", usse_vtstmsk_f32(T(0), T(2), T(4), USSE_CMP_LT));
      P("tstmsk_ge_c0", usse_vtstmsk_f32(PA(20), SA(70), USSE_ZERO, USSE_CMP_GE));
      w = usse_fop(USSE_NMAD_ADD, T(6), T(6), T(6));
      usse_nosched(&w);
      P("add_nosched", w);
      w = usse_vbw_or(T(6), T(6), 0);
      usse_nosched(&w);
      P("or_nosched", w);
   }
   return 0;
}
