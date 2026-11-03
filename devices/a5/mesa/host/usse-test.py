#!/usr/bin/env python3
"""usse-test.py -- the USSE encoder (mesa/files/.../sgx/sgx_usse.c) against
the disassembler (tools/iosgpu/usse-dis.py) and against iOS's own words
where iOS wrote the same instruction (the M11 corpus).

    python3 mesa/host/usse-test.py      (needs a C compiler: cc)

Builds usse-test.c with the encoder, runs it, and checks each word: that
it decodes as the instruction meant, with the operands meant; and for the
cases iOS has, that it is iOS's word bit for bit.
"""
import importlib.util, os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
SGX = os.path.join(ROOT, 'mesa', 'files', 'src', 'gallium', 'drivers', 'sgx')
spec = importlib.util.spec_from_file_location('ud', os.path.join(ROOT, 'tools', 'iosgpu', 'usse-dis.py'))
ud = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ud)

# name: (mnemonic, operands as usse-dis.py prints them, iOS's word or None).
# The disassembler names a float operand by its 64-bit register (pa5 is
# pa4.y); iOS's VCOMP words are from a secondary program, where pa reads
# as sa (corpus p05_rcp_highp), so they are compared as words only.
CASES = {
    'pack_pa0': ('VPCK', 'pck.u8.f32 o0.xyzw, pa0.xyzw, pa2 scale', 0x40840c3da01d8002),
    'pack_r8': ('VPCK', 'pck.u8.f32 o0.xyzw, r8.xyzw, r10 scale', None),
    'rcp_pa10_pa5': ('VCOMP', None, 0x3080000a80a00101),
    'rcp_pa11_pa6': ('VCOMP', None, 0x3080000280a00182),
    'rcp_pa12_pa7': ('VCOMP', None, 0x3080000a80c00181),
    'rcp_pa13_pa8': ('VCOMP', None, 0x3080000280c00202),
    'rsq_r4_sa3': ('VCOMP', 'rsq.f32.f32 r4.x, sa2.y', None),
    'exp_r5_r2': ('VCOMP', 'exp.f32.f32 r4.y, r2.x', None),
    'add_r4_r6_pa5': ('V32NMAD', 'add.f32 r4.x, r6.xxxx, pa4.yyyy', None),
    'mul_r5_nsa3_r8': ('V32NMAD', 'mul.f32 r4.y, -sa2.yyyy, r8.xxxx', None),
    'min_r10_pa1_sa0': ('V32NMAD', 'min.f32 r10.x, pa0.yyyy, sa0.xxxx', None),
    'max_o2_r3_r4': ('V32NMAD', 'max.f32 o2.x, r2.yyyy, r4.xxxx', None),
    'frc_r2_r3_r3': ('V32NMAD', 'frc.f32 r2.x, r2.yyyy, r2.yyyy', None),
    'mad_r2_pa1_sa4_r7': ('VMAD2', 'mad.f32 r2.x, pa0.yyyy, sa4.xxxx, r6.yyyy', None),
    'mad_r3_r0_nr2_sa1': ('VMAD2', 'mad.f32 r2.y, r0.xxxx, -r2.xxxx, sa0.yyyy', None),
    'mov_r3_pa4': ('VMOV', 'mov.f32 r2.y, pa4.xxxx', None),
    'mov_r2_sa7': ('VMOV', 'mov.f32 r2.x, sa6.yyyy', None),
    'movc_eq': ('VMOV', 'movc.f32 r2.x, r4.xxxx == 0 ? r6.xxxx : r8.xxxx', None),
    'movc_lt': ('VMOV', 'movc.f32 r2.y, pa4.yyyy < 0 ? sa0.yyyy : r8.yyyy', None),
    'smp_ios': ('SMP', 'smp2d.raw.f16 pa0, pa0, state sa6 drc0', 0xe001048ce0000180),
    'smp_f32': ('SMP', 'smp2d.f32.f32 r8, r4, state sa12 drc0', None),
    'smp_bias': ('SMP', 'smp2d.f32.f32 r8, r4, state sa12, r6 drc0 bias', None),
    'wdf0': ('SPEC', '', 0xf920000000000000),
    'unpack_r4_o0_xy': ('VPCK', 'pck.f32.u8 r4.xy, o0.xyzw scale', None),
    'unpack_r6_o0_zw': ('VPCK', 'pck.f32.u8 r6.xy, o0.zwzw scale', None),
    'limm_r5': ('LIMM', 'r5 <- #0x3f800000', None),
    'limm_r100': ('LIMM', 'r100 <- #0xdeadbeef', None),
    'vtst_r5': ('VTST', 'tst p1.x = and(r5, r5) ne 0 (and)', None),
    'vtst_pa4': ('VTST', 'tst p0.x = and(pa4, pa4) eq 0 (and)', None),
    # iOS's c00_discard: `p1? KILL`, the end of its first phase
    'kill_p1_end': ('SPEC', '', 0xf9340426c0000280),
    # c03_loop's pixel program, c04_loop_break's (skipinv clear there)
    'or_o0_sa14': ('VBW', 'or o0, sa14, #0x0', 0x50850009e0000700),
    # M38: F16, four channels an instruction (iOS's s00, s03, s11)
    'mul_f16_pa0_pa2_pa0': ('V16NMAD', 'mul.f16 pa0.xyzw, pa2.xyzw, pa0.xyzw', 0x10a44786a0040040),
    'mul_f16_pa0_pa0_pa2': ('V16NMAD', 'mul.f16 pa0.xyzw, pa0.xyzw, pa2.xyzw', 0x10a44786a0040001),
    'mul_f16_pa0_pa0_sa6': ('V16NMAD', 'mul.f16 pa0.xyzw, pa0.xyzw, sa6.xyzw', 0x10a44786b0040003),
    'pack_f16_o0_pa0': ('VPCK', 'pck.u8.f16 o0.xyzw, pa0.xyzw scale', 0x40850a3da01d8000),
    'pack_f16_pa0_pa0': ('VPCK', 'pck.u8.f16 pa0.xyzw, pa0.xyzw scale', 0x40810a3ea01d8000),
    # M37: the logic ops of iOS's es1 cases e03..e10
    'xor_o0_pa0_o0': ('VBW', 'xor o0, pa0, o0', 0x5884000190000000),
    'and_o0_pa0_o0': ('VBW', 'and o0, pa0, o0', 0x5084000190000000),
    'or_o0_pa0_o0': ('VBW', 'or o0, pa0, o0', 0x5084000990000000),
    'not_o0': ('VBW', 'and o0, c56, ~o0', 0x5086080150001c00),
    'not_pa0': ('VBW', 'and o0, c56, ~pa0', 0x5086080160001c00),
    'and_r0_pa0_o0': ('VBW', 'and r0, pa0, o0', 0x5080000090000000),
    'set_o0': ('VBW', 'or o0, o0, ~o0', 0x5084080950000000),
    'clear_o0': ('VBW', 'xor o0, o0, o0', 0x5884000150000000),
    'and_r2_r0': ('VBW', 'and r2, r0, #0x1f', 0x500100002040001f),
    'and_index1': ('VBW', 'and index1, r6, #0xffff', None),
    'or_r3_idx1_sa2': ('VBW', 'or r3, idx198, #0x0', None),
    # M44: integers -- the words the device ran (glldr --probe), DEVICE below
    'imad_r0_r2_r4_r2': ('I32MAD2', None, None),
    'isub_r0_r2_r4': ('I32MAD2', None, None),
    'ineg_r0_r2': ('I32MAD2', None, None),
    'shl_r0_r2_3': ('VBW', 'shl r0, r2, #0x3', None),
    'asr_r0_r2_r4': ('VBW', 'asr r0, r2, r4', None),
    'xor_r0_r2_sign': ('VBW', 'xor r0, r2, #0x80000000', None),
    'not_r0_r2': ('VBW', 'xor r0, r2, c56', None),
    'imovc_lt': ('VMOV', 'movc.s32 r0.x, r2 < 0 ? r4 : r6', None),
    'imovc_bool': ('VMOV', 'movc.s32 r0.x, r2 == 0 ? c56 : c0', None),
    'vtst_slt': ('VTST', 'tst p0.x = sub.s32(r2, r4) lt 0 (and)', None),
    'vtst_ult': ('VTST', 'tst p0.x = sub.u32(r2, r4) lt 0 (and)', None),
    'vtst_ieq': ('VTST', 'tst p0.x = sub.s32(r2, r4) eq 0 (and)', None),
    'vtst_flt': ('VTST', 'tst p0.x = sub.f32(r2, r4) lt 0 (and)', None),
    'vtst_flt_y': ('VTST', 'tst p0.y = sub.f32(r2, r4) lt 0 (and)', None),
    'cvt_s16hi_f32': ('VPCK', 'pck.f32.s16 r0.x, r2.yyzw', None),
    'cvt_u16lo_f32': ('VPCK', 'pck.f32.u16 r0.x, r2.xyzw', None),
    'cvt_f32_s16lo': ('VPCK', 'pck.s16.f32 r0.x, r2.xxzw', None),
    'cvt_f32_s16hi': ('VPCK', 'pck.s16.f32 r0.y, r2.xxzw', None),
    'limm_p0': ('LIMM', 'r0 <- #0xffffffff', None),
}

# (M44) words the device ran through glldr --probe and gave the right answer
DEVICE = {
    'imad_r0_r2_r4_r2': 0xd000020000008202, 'isub_r0_r2_r4': 0xd002028080008084,
    'ineg_r0_r2': 0xd0030300a0008080, 'shl_r0_r2_3': 0x6081000020000103,
    'asr_r0_r2_r4': 0x6880000800000104, 'xor_r0_r2_sign': 0x5881042020000100,
    'not_r0_r2': 0x5881000010000138, 'imovc_lt': 0x38c0420001002106,
    'imovc_bool': 0x3883420051002e00, 'vtst_slt': 0x4880048000078104,
    'vtst_ult': 0x488004800007c104, 'vtst_ieq': 0x4880018000078104,
    'vtst_flt': 0x4880868000038082, 'vtst_flt_y': 0x4880869000038082,
    'cvt_s16hi_f32': 0x4081098420198101, 'cvt_u16lo_f32': 0x4081078420198100,
    'cvt_f32_s16lo': 0x40810d0420188100, 'cvt_f32_s16hi': 0x40810d0820188100,
    'limm_p0': 0xfca3f3f0001fffff,
}


def main():
    with tempfile.TemporaryDirectory() as d:
        exe = os.path.join(d, 'usse-test')
        subprocess.check_call(['cc', '-std=c11', '-Wall', '-Werror', '-I', SGX, '-o', exe,
                               os.path.join(HERE, 'usse-test.c'), os.path.join(SGX, 'sgx_usse.c')])
        out = subprocess.check_output([exe], text=True)
    bad = 0
    seen = set()
    for line in out.split('\n'):
        if not line:
            continue
        name, hexw = line.split()
        w = int(hexw, 16)
        mnem, text, ios = CASES[name]
        seen.add(name)
        got, _ = ud.decode(w)
        ops = ud.operands(got, w).strip() if got else ''
        problems = []
        if got != mnem:
            problems.append('decodes as %s' % got)
        if text is not None and ops != text:
            problems.append('reads "%s"' % ops)
        if ios is not None and w != ios:
            problems.append("is not iOS's %016x (xor %016x)" % (ios, w ^ ios))
        if name in DEVICE and w != DEVICE[name]:
            problems.append("is not the word the device ran, %016x" % DEVICE[name])
        bad += bool(problems)
        print('%-20s %016x %-8s %s%s' % (name, w, got, ops, '' if not problems else
                                          '   <-- ' + '; '.join(problems)))
    missing = set(CASES) - seen
    if missing:
        print('not printed by usse-test.c:', ', '.join(sorted(missing)))
        bad += len(missing)
    print('%d of %d right' % (len(CASES) - bad, len(CASES)))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
