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
    'vtst_r5': ('VTST', 'tst p1.y = sub.f32(r4, #0.x) ne 0', None),
    'vtst_pa4': ('VTST', 'tst p0.x = sub.f32(pa4, #0.x) ne 0', None),
    # iOS's c00_discard: `p1? KILL`, the end of its first phase
    'kill_p1_end': ('SPEC', '', 0xf9340426c0000280),
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
