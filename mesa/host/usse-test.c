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
   return 0;
}
