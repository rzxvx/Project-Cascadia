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
vtst P S1 S2 alu_sel= alu_op= sign= zero= comb= chan= prec=, out R,
word HEX.  Registers r, o, pa, sa, c (special), # (immediate).

DEVICE (root@10.55.0.2) is the iPad; MESA (/root/sgx-mesa-cur) the build
whose probe runs; SGX_LDR_PROBE_BRANCHES=1 in ENV runs the probe as a
program whose pixels each go their own way (PHAS mode 1).
"""
import importlib.util, os, re, subprocess, sys

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


def _kw(args, conv=lambda v: int(v, 0)):
    return {k: (eval(v) if k == 'sel' else conv(v)) for k, v in (a.split('=') for a in args)}


def parse(line):
    t = line.split()
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
        print('%-12s %s' % ('%#010x' % v if v is not None else 'ERR', ' ; '.join(lines)))
        if v is None:
            print('    ', txt[:300])
    return 0


if __name__ == '__main__':
    sys.exit(main())
