#!/usr/bin/env python3
"""usse-probe.py -- USSE instructions run on the iPad's GPU, one program at a
time, to find what they do (docs/research/p105-mesa.md, M44).

Each probe is a few instructions, assembled from Vita3K's bitstrings (the
table tools/iosgpu/usse-dis.py decodes with) and run by the Mesa driver's
probe program (SGX_LDR_PROBE=word,... glldr --probe, sgx_compiler.c): the
words after its PHAS, the pixel's bytes printed.  End a probe with `out rN`
(o0 = rN as it is, and the program's end): the word comes back as it is.

    usse-probe.py batch FILE        a probe a line, instructions split by ';'
    usse-probe.py words LINE...     the words, disassembled

    limm r2 0x80000005; shl r0 r2 #3; out r0          -> 0x00000028

Instructions: limm D V [PRED], imad D S0 S1 S2 [signed= neg1= neg2=],
and/or/xor/shl/shr/asr/rol D S1 S2|#IMM [rot= inv=], vmov D S0 S1 S2 TEST
[typ= kind= mask= swz=], vpck D S1 S2 sfmt= dfmt= mask= sel=(a,b,c,d),
vtst P S1 S2 alu_sel= alu_op= sign= zero= comb= chan= prec=, vtstmsk D S1 S2
[alu_sel= alu_op= sign= zero= mtype= prec= wb=], out R,
word HEX.  Registers r, o, pa, sa, c (special), # (immediate).

DEVICE (root@10.55.0.2) is the iPad; MESA (/root/sgx-mesa-cur) the build
whose probe runs; SGX_LDR_PROBE_BRANCHES=1 in ENV runs the probe as a
program whose pixels each go their own way (PHAS mode 1).
"""
import importlib.util, os, re, struct, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
spec = importlib.util.spec_from_file_location('ud', os.path.join(ROOT, 'tools', 'iosgpu', 'usse-dis.py'))
ud = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ud)

DEVICE = os.environ.get('DEVICE', 'root@10.55.0.2')
MESA = os.environ.get('MESA', '/root/sgx-mesa-cur')
ENV = os.environ.get('ENV', '')


def enc(name, **f):
    """fill a bitstring's lettered fields (MSB first, as Vita3K writes them)"""
    bs = ud.bits_of(name)
    widths = {}
    for ch in bs:
        widths[ch] = widths.get(ch, 0) + 1
    w, pos = 0, {}
    for i, ch in enumerate(bs):
        b = 63 - i
        if ch == '1':
            w |= 1 << b
        elif ch in f:
            k = pos.get(ch, 0)
            pos[ch] = k + 1
            if f[ch] >> widths[ch]:
                raise ValueError('%s: field %s=%#x does not fit %d bits' % (name, ch, f[ch], widths[ch]))
            if (f[ch] >> (widths[ch] - 1 - k)) & 1:
                w |= 1 << b
    return w


def _reg(r, table):
    m = re.match(r'(r|o|pa|sa|c|#)(\d+)$', r)
    if not m or m.group(1) not in table:
        raise ValueError('register %s' % r)
    return table[m.group(1)] + (int(m.group(2)),)


SRC12 = {'r': (0, 0), 'o': (0, 1), 'pa': (0, 2), 'sa': (0, 3), 'c': (1, 1), '#': (1, 2)}
DEST = {'r': (0, 0), 'o': (0, 1), 'pa': (0, 2), 'sa': (1, 0)}
SRC0 = {'r': (0, 0), 'pa': (0, 1), 'o': (1, 0), 'sa': (1, 1)}
VBW_OPS = {'and': (2, 0), 'or': (2, 1), 'xor': (3, 0), 'shl': (4, 0), 'rol': (4, 1),
           'shr': (5, 0), 'asr': (5, 1)}


def limm(d, value, pred=0):
    e, b, n = _reg(d, DEST)
    v = value & 0xffffffff
    return enc('LIMM', s=1, i=v >> 26, p=pred, m=(v >> 21) & 0x1f, t=b, u=n, v=v & 0x1fffff)


def imad(d, s0, s1, s2, signed=1, neg1=0, neg2=0):
    de, db, dn = _reg(d, DEST)
    e0, b0, n0 = _reg(s0, SRC0)
    e1, b1, n1 = _reg(s1, SRC12)
    e2, b2, n2 = _reg(s2, SRC12)
    return enc('I32MAD2', d=de, r=e1, c=e2, b=e0, i=signed, g=neg1, a=neg2, k=b0, t=db,
               f=b1, h=b2, j=dn, l=n0, m=n1, q=n2)


def vbw(op, d, s1, s2, inv=0, rot=0, end=0, pred=0):
    o1, o2 = VBW_OPS[op]
    de, db, dn = _reg(d, DEST)
    e1, b1, n1 = _reg(s1, SRC12)
    if s2.startswith('#'):
        imm = int(s2[1:], 0)
        e2, b2, n2, sel, exth = 1, 2, imm & 0x7f, (imm >> 7) & 0x7f, imm >> 14
    else:
        (e2, b2, n2), sel, exth = _reg(s2, SRC12), 0, 0
    return enc('VBW', o=o1, p=pred, s=1, d=de, e=end, c=e1, x=e2, i=inv, t=rot, h=exth, b=o2,
               k=db, f=b1, g=b2, j=dn, l=sel, m=n1, q=n2)


def vmov(d, s0, s1, s2, test, typ=2, kind=1, mask=1, swz=0):
    """test 0 eq0, 1 ne0, 2 lt0, 3 le0; typ 2 INT32, 5 F32 (its registers
    64-bit ones)"""
    de, db, dn = _reg(d, DEST)
    e0, b0, n0 = _reg(s0, SRC0)
    e1, b1, n1 = _reg(s1, SRC12)
    e2, b2, n2 = _reg(s2, SRC12)
    return enc('VMOV', s=1, t=test >> 1, d=de, e=e0, c=e1, b=e2, m=kind, o=typ, i=test & 1,
               w=swz, k=b0, l=db, f=b1, g=b2, h=mask, j=dn, q=n0, u=n1, v=n2)


def vpck(d, s1, s2, sfmt, dfmt, mask, sel, scale=0):
    de, db, dn = _reg(d, DEST)
    e1, b1, n1 = _reg(s1, SRC12)
    e2, b2, n2 = _reg(s2, SRC12)
    return enc('VPCK', s=1, d=de, r=e1, c=e2, f=sfmt, t=dfmt, m=mask, b=db, k=b1, l=b2, g=dn,
               o=sel[3], h=scale, i=sel[1], j=sel[2], q=n1, v=sel[0] >> 1, w=n2, x=sel[0] & 1)


def vtst(pdst, s1, s2, alu_sel, alu_op, sign, zero, comb=1, chan=0, prec=0):
    e1, b1, n1 = _reg(s1, SRC12)
    e2, b2, n2 = _reg(s2, SRC12)
    return enc('VTST', s=1, c=e1, e=e2, a=prec, i=sign, z=zero, m=comb, h=chan, n=pdst,
               k=b1, f=b2, l=alu_sel, u=alu_op, j=n1, q=n2)


def vtstmsk(d, s1, s2, alu_sel=0, alu_op=14, sign=1, zero=0, mtype=0, prec=1, wb=1, neg1=0,
            v=0, o=0, y=0, m=0):
    """(M46) a test's outcome as a mask into D (mtype: what the mask is) --
    the F32 forms' registers 64-bit ones, as VTST's"""
    de, db, dn = _reg(d, DEST)
    e1, b1, n1 = _reg(s1, SRC12)
    e2, b2, n2 = _reg(s2, SRC12)
    return enc('VTSTMSK', s=1, o=o, y=y, d=de, t=neg1, r=e1, c=e2, e=prec, v=v, i=sign,
               z=zero, m=m, a=mtype, b=db, n=b1, k=b2, f=dn, w=wb, l=alu_sel, g=alu_op, h=n1,
               j=n2)


def _vreg(r, table, bits=6):
    """a float operand's register field (64-bit units: r2 is field 1), the
    internal registers i0..i3 the temporaries' top four (fields 60..63 of
    six bits, 124..127 of seven); the hardware's constants (c) and
    immediates as they are"""
    m = re.match(r'i([0-3])$', r)
    if m:
        return 0, 0, (1 << bits) - 4 + int(m.group(1))
    e, b, n = _reg(r, table)
    if r[0] in 'c#':
        return e, b, n
    if n & 1:
        raise ValueError('%s: a float operand names a 64-bit register (even)' % r)
    return e, b, n >> 1


def _sw(s, table):
    """a swizzle as its index in table (a tuple of names), or a number"""
    return table.index(s) if s in table else int(s, 0)


def _chan(s):
    """four channels, Vita3K's SwizzleChannel each (x y z w 0 1 2 h)"""
    return [ud.CHAN.index(ch) for ch in s]


def fmov(d, s, mask=0xf, swz='xyzw', rpt=0, nosched=0, typ=5):
    """(VMOV, a float move) d = s through swz (VEC4_STD), d's channels in
    mask -- F32 (typ 5) or F16 (4) registers 64-bit ones"""
    de, db, dn = _vreg(d, DEST)
    e1, b1, n1 = _vreg(s, SRC12)
    return enc('VMOV', s=1, d=de, c=e1, m=0, a=rpt, n=nosched, o=typ, w=_sw(swz, ud.VEC4_STD),
               l=db, f=b1, h=mask, j=dn, u=n1)


def vmad2(d, s0, s1, s2, f16=0, mask=0x3, sw0='xyzw', sw1='xyzw', sw2='xyzw', abs0=0, mod1=0,
          mod2=0, nosched=0, pred=0):
    """(VMAD2) d = s0 * s1 + s2, two F32 lanes (or four F16)"""
    de, db, dn = _vreg(d, DEST)
    e0, b0, n0 = _vreg(s0, SRC0)
    e1, b1, n1 = _vreg(s1, SRC12)
    e2, b2, n2 = _vreg(s2, SRC12)
    if de or e0:
        raise ValueError('vmad2: dest and src0 have no extended banks')
    k0, k1 = _sw(sw0, ud.MAD2_S0), _sw(sw1, ud.MAD2_S1)
    return enc('VMAD2', d=f16, p=pred, s=1, r=k0 >> 2, c=abs0, b=e1, a=e2, w=_sw(sw2, ud.MAD2_S2),
               i=k1 >> 2, n=nosched, e=mask, m=mod1, o=mod2, k=b0, t=db, f=b1, g=b2, h=dn,
               z=k1 & 3, j=k0 & 3, l=n0, q=n1, u=n2)


def nmad(op, d, s1, s2, f16=0, mask=0x1, sw1='xyzw', sw2='xyzw', mod1=0, abs2=0, nosched=0):
    """(V32NMAD / V16NMAD) d = op(s1, s2): src1 through four channels (x y z
    w 0 1 2 h), src2 through VEC4_STD"""
    de, db, dn = _vreg(d, DEST)
    e1, b1, n1 = _vreg(s1, SRC12)
    e2, b2, n2 = _vreg(s2, SRC12)
    sw = sum(c << 3 * k for k, c in enumerate(_chan(sw1)))
    return enc('V16NMAD' if f16 else 'V32NMAD', s=1, r=sw >> 10, d=de, c=sw >> 9 & 1, b=e1,
               a=e2, w=_sw(sw2, ud.VEC4_STD), n=nosched, e=mask, m=mod1, o=abs2, i=sw >> 7 & 3,
               t=db, k=b1, l=b2, f=dn, z=sw & 0x7f, g=ud.NMAD_OP.index(op), h=n1, j=n2)


def _v34(s, v4):
    """a VMAD/VDP swizzle: (index, extended table)"""
    std, ext = (ud.VEC4_STD, ud.VEC4_EXT) if v4 else (ud.VEC3_STD, ud.VEC3_EXT)
    if s in std:
        return std.index(s), 0
    if s in ext:
        return ext.index(s), 1
    return int(s, 0), 0


def vmad(d, g0, s1, g1, v4=1, mask=0xf, swg0='xyzw', sw1='xyzw', swg1='xyzw', neg0=0, abs0=0,
         neg1=0, abs1=0, negg1=0, absg1=0, rpt=0, inc=0, nosched=0, end=0):
    """(VMAD) d = g0 * s1 + g1, g0 and g1 internal registers (i0..i3), mad4
    (v4) or mad3"""
    de, db, dn = _vreg(d, DEST)
    e1, b1, n1 = _vreg(s1, SRC12)
    q, z = _v34(swg0, v4)
    a_, y = _v34(sw1, v4)
    u, g = _v34(swg1, v4)
    return enc('VMAD', s=1, g=g, o=v4, d=de, e=end, r=e1, a=inc, i=abs0, t=rpt, n=nosched, w=mask,
               c=neg1, b=abs1, f=negg1, h=absg1, z=z, k=db, j=b1, l=int(g0[1:]), m=dn, q=q, u=u,
               v=int(g1[1:]), x=neg0, y=y, A=a_, B=n1)


def vdp(d, s1, g0, v4=1, mask=0x1, sw1='xyzw', swg0='xyzw', neg1=0, abs1=0, absg0=0, clip=None,
        rpt=0, inc=0, nosched=0, end=0):
    """(VDP) d = dot(s1, g0), g0 an internal register: dp4 (v4) or dp3; src1
    through four channels of its own (x y z w 0 1 2 h)"""
    de, db, dn = _vreg(d, DEST)
    e1, b1, n1 = _vreg(s1, SRC12)
    ch = _chan(sw1.ljust(4, 'x'))
    z, zx = _v34(swg0, v4)
    if zx:
        raise ValueError('vdp: gpi0 takes the standard swizzles')
    return enc('VDP', s=1, c=clip is not None, o=v4, d=de, e=end, r=e1, a=inc, g=absg0, t=rpt,
               n=nosched, w=mask, b=neg1, f=abs1, l=clip or 0, k=db, h=b1, i=int(g0[1:]), j=dn,
               z=z, m=ch[3], q=ch[2], y=ch[1], x=ch[0], u=n1)


def vcomp(op, d, s, chan=0, mask=0x1, mod=0, rpt=0):
    """(VCOMP) d = op(s.chan), F32: op rcp rsq log exp"""
    de, db, dn = _vreg(d, DEST, 7)
    e1, b1, n1 = _vreg(s, SRC12, 7)
    return enc('VCOMP', s=1, d=0, e=de, r=e1, a=rpt, b=ud.COMP_OP.index(op), c=0, m=mod, f=chan,
               t=db, k=b1, g=dn, h=n1, w=mask)


def _kw(args, conv=lambda v: int(v, 0)):
    def val(k, v):
        if k == 'sel':
            return eval(v)
        if k.startswith('sw'):
            return v
        return conv(v)
    return {k: val(k, v) for k, v in (a.split('=') for a in args)}


def _f32(v):
    return struct.unpack('<I', struct.pack('<f', float(v)))[0]


def parse(line):
    """an instruction; tokens +bN set bit N of its word (a flag found by
    probing: +b43 is the vector ALU's nosched)"""
    t = line.split()
    extra = 0
    for tok in [x for x in t if re.match(r'\+b\d+$', x)]:
        extra |= 1 << int(tok[2:])
        t.remove(tok)
    return _parse(t) | extra


def _parse(t):
    op, a = t[0], t[1:]
    if op == 'limm':
        return limm(a[0], int(a[1], 0), int(a[2]) if len(a) > 2 else 0)
    if op == 'imad':
        return imad(a[0], a[1], a[2], a[3], **_kw(a[4:]))
    if op in VBW_OPS:
        return vbw(op, a[0], a[1], a[2], **_kw(a[3:]))
    if op == 'vmov':
        return vmov(a[0], a[1], a[2], a[3], int(a[4]), **_kw(a[5:]))
    if op == 'vpck':
        return vpck(a[0], a[1], a[2], **_kw(a[3:]))
    if op == 'vtst':
        return vtst(int(a[0]), a[1], a[2], **_kw(a[3:]))
    if op == 'vtstmsk':
        return vtstmsk(a[0], a[1], a[2], **_kw(a[3:]))
    if op == 'limmf':
        return limm(a[0], _f32(a[1]), int(a[2]) if len(a) > 2 else 0)
    if op == 'fmov':
        return fmov(a[0], a[1], **_kw(a[2:]))
    if op == 'vmad2':
        return vmad2(a[0], a[1], a[2], a[3], **_kw(a[4:]))
    if op in ud.NMAD_OP:
        return nmad(op, a[0], a[1], a[2], **_kw(a[3:]))
    if op == 'vmad':
        return vmad(a[0], a[1], a[2], a[3], **_kw(a[4:]))
    if op == 'vdp':
        return vdp(a[0], a[1], a[2], **_kw(a[3:]))
    if op in ud.COMP_OP:
        return vcomp(op, a[0], a[1], **_kw(a[2:]))
    if op == 'enc':
        return enc(a[0], **_kw(a[1:]))
    if op == 'out':
        return vbw('or', 'o0', a[0], '#0', end=1)
    if op == 'word':
        return int(a[0], 16)
    raise ValueError(op)


def batch(probes):
    """each probe a list of lines; one ssh for all: [(word or None, text)]"""
    script = []
    for i, lines in enumerate(probes):
        words = ','.join('%016x' % parse(l) for l in lines)
        script.append('echo "@%d $(%s SGX_MESA=%s SGX_LDR_PROBE=%s /root/sgx-dev glldr --probe '
                      '2>&1 | tr "\\n" " ")"' % (i, ENV, MESA, words))
    r = subprocess.run(['ssh', DEVICE, 'sh'], input='\n'.join(script) + '\n',
                       capture_output=True, text=True, timeout=600)
    res = [(None, '')] * len(probes)
    for line in r.stdout.splitlines():
        m = re.match(r'@(\d+) (.*)', line)
        if not m:
            continue
        p = re.search(r'pixels: ([0-9a-f]{2}) ([0-9a-f]{2}) ([0-9a-f]{2}) ([0-9a-f]{2})', m.group(2))
        if p:
            # a pixel's R G B A are o0's bytes 0..3
            R, G, B, A = (int(x, 16) for x in p.groups())
            res[int(m.group(1))] = (A << 24 | B << 16 | G << 8 | R, m.group(2))
        else:
            res[int(m.group(1))] = (None, m.group(2))
    return res


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ('batch', 'words'):
        print(__doc__)
        return 2
    if sys.argv[1] == 'words':
        for l in sys.argv[2:]:
            w = parse(l)
            name = ud.decode(w)[0]
            print('%016x  %-8s %s' % (w, name or '?', ud.operands(name, w) if name else ''))
        return 0
    probes = [[x.strip() for x in l.split(';') if x.strip()]
              for l in open(sys.argv[2]) if l.strip() and not l.startswith('#')]
    for lines, (v, txt) in zip(probes, batch(probes)):
        f = struct.unpack('<f', struct.pack('<I', v))[0] if v is not None else 0
        print('%-12s %-12s %s' % ('%#010x' % v if v is not None else 'ERR',
                                  '%.6g' % f if v is not None else '', ' ; '.join(lines)))
        if v is not None and os.environ.get('ALL'):
            print('    ', re.search(r'pixels: .*', txt).group(0))
        if v is None:
            print('    ', txt[:300])
    return 0


if __name__ == '__main__':
    sys.exit(main())
