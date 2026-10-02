#!/usr/bin/env python3
"""capmap.py -- what of iOS's captured GL memory the pack actually uses.

    capmap.py PACKDIR [--strip OUTDIR]

Walks the pointers from everything that reaches the GPU in an sgx2d frame --
the template blocks sgx2d copies (tmpl.bin), the VDM words it writes
(consts, tail), the kernel's render data (render target buffers, 3D block,
TA command), our framebuffer descriptor -- into the GL window and the USSE
code page, and prints every object reached: its address, size, words and
who points at it.  Pointer encodings (docs/research/p105-gpu.md, M6): plain;
tag | addr >> 4 (VDM, some PDS data); PDS data tag5 | (addr >> 4) &
0x07ffffff with bit 31 implied; DOUTU idx << 4 | 3, a USSE program at code
base + idx * 8.  An object runs from where it is pointed at to the next 16
zero bytes (a PDS program's data and code are contiguous); a USSE program
from its PHAS to the first other instruction with bit 50, the end flag, set.

--strip writes a copy of the pack with everything not reached zeroed: if the
frames still draw from it, the map is the whole of what has to be replaced
by our own programs and state (M8, no iOS capture).
"""
import os, shutil, struct, sys

def words(b):
    return list(struct.unpack('<%dI' % (len(b) // 4), b[:len(b) // 4 * 4]))

def main():
    pack = sys.argv[1]
    P = {}
    for l in open(os.path.join(pack, 'pack.txt')):
        k, *v = l.split()
        P.setdefault(k, []).append(v)
    imgs = {int(va, 16): open(os.path.join(pack, f), 'rb').read() for va, f in P['img']}
    GL = 0x98956000
    gl = imgs[GL]
    CODE = 0x9a061000                     # code page; base 3 = CODE - 0x1000
    code = imgs[CODE]
    CB = CODE - 0x1000
    tmpl = open(os.path.join(pack, 'tmpl.bin'), 'rb').read()

    def region(a):
        if GL <= a < GL + len(gl):
            return 'gl'
        if CODE <= a < CODE + len(code):
            return 'code'
        return None

    def decode(w):
        """the GL-window or code addresses a word may encode"""
        out = []
        if region(w):
            out.append(('plain', w))
        a = (w & 0x0fffffff) << 4
        if region(a) and a != w:
            out.append(('tag', a))
        a = 0x80000000 | (w & 0x07ffffff) << 4
        if region(a) and a not in (w, (w & 0x0fffffff) << 4):
            out.append(('pds', a))
        if w & 0xf == 3 and 0x1000 <= w < 0x10000:
            a = CB + (w >> 4) * 8
            if region(a) == 'code':
                out.append(('doutu', a))
        return out

    def extent(a):
        """from a to the next 16 zero bytes (aligned), or for code to the
        program's end (a zero instruction)"""
        if region(a) == 'code':
            # to the first instruction other than PHAS with bit 50, the end
            # flag, set (NOP f804.., LIMM fca4.., VBW, SPEC, VMOV, VPCK alike)
            b, o = code, a - CODE
            e = o
            while e < len(b):
                w = struct.unpack_from('<Q', b, e)[0]
                e += 8
                if w >> 56 != 0xfa and w >> 50 & 1:
                    break
            return a, a + (e - o)
        b, o = gl, a - GL
        e = o
        while e < len(b):
            if b[e & ~0xf:(e & ~0xf) + 16] == bytes(16) and e > o:
                break
            e += 4
        return a, GL + e

    objs = {}       # start -> [end, set(refs)]
    todo = []
    def ref(a, why):
        s, e = extent(a)
        if s in objs:
            objs[s][1].add(why)
            return
        objs[s] = [e, {why}]
        todo.append(s)

    def scan(name, ws, base=None):
        for i, w in enumerate(ws):
            for kind, a in decode(w):
                ref(a, '%s+%x %s' % (name, i * 4, kind))

    # roots
    for name, off, size in [(k[5:], int(v[0][0]), int(v[0][1])) for k, v in P.items() if k.startswith('tmpl_')]:
        scan('tmpl_' + name, words(tmpl[off:off + size]))
        # the blocks themselves: where they were copied from is not known
        # here, but rpack takes them from the GL window -- find them there
        blk = tmpl[off:off + size]
        i = gl.find(blk)
        if i >= 0:
            ref(GL + i, 'tmpl_' + name + ' (copied)')
    for k in ('consts0', 'consts'):
        ref(int(P[k][0][0], 16), 'vdm ' + k)
    # the index buffer: u16 indices, every draw's
    ia, n = int(P['idx'][0][0], 16), int(P['idx'][0][1])
    objs[ia] = [ia + 2 * n, {'vdm draws (idx)'}]
    for i, w in enumerate(int(x, 16) for x in P['tail'][0]):
        for kind, a in decode(w):
            ref(a, 'vdm tail %d %s' % (i, kind))
    for va, b in imgs.items():
        if va in (GL, CODE, 0x98f00000):
            continue
        scan('img %x' % va, words(b))
    for v in P.get('blendprogs', [[]])[0]:
        for kind, a in decode(int(v, 16)):
            ref(a, 'blendprogs')

    # follow pointers inside what was reached
    while todo:
        s = todo.pop()
        e = objs[s][0]
        src = gl if region(s) == 'gl' else code
        base = GL if region(s) == 'gl' else CODE
        if region(s) == 'code':
            continue                    # USSE code points nowhere we map
        scan('%x' % s, words(src[s - base:e - base]))

    # merge overlaps and print
    keep = {'gl': bytearray(len(gl)), 'code': bytearray(len(code))}
    total = {'gl': 0, 'code': 0}
    for s in sorted(objs):
        e, why = objs[s]
        r = region(s)
        base = GL if r == 'gl' else CODE
        for o in range(s - base, e - base):
            keep[r][o] = 1
        n = (e - s) // 4
        print('%-4s %08x +%-4x %3d words  <- %s' % (r, s, e - s, n, ', '.join(sorted(why))[:150]))
    for r, src in (('gl', gl), ('code', code)):
        nz = sum(1 for i in range(0, len(src), 4) if src[i:i + 4] != bytes(4))
        kept = sum(1 for i in range(0, len(src), 4) if src[i:i + 4] != bytes(4) and keep[r][i])
        print('%s: %d of %d non-zero words reached' % (r, kept, nz))

    if '--strip' in sys.argv:
        out = sys.argv[sys.argv.index('--strip') + 1]
        if os.path.abspath(out) == os.path.abspath(pack):
            sys.exit('--strip needs another directory')
        shutil.copytree(pack, out, dirs_exist_ok=True)
        for va, r in ((GL, 'gl'), (CODE, 'code')):
            src = bytearray(imgs[va])
            for i in range(len(src)):
                if not keep[r][i]:
                    src[i] = 0
            open(os.path.join(out, 'm_%08x.bin' % va), 'wb').write(src)
        print('stripped pack:', out)

if __name__ == '__main__':
    main()
