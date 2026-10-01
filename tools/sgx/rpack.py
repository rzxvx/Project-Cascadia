#!/usr/bin/env python3
"""rpack.py -- the template pack sgx2d (tools/sgx/lib) builds its frames from.

From the texture x vertex colour capture (logs/ios/mod, rgen.py --profile
modblend), at full screen, plus a window of our own (EXT) for textures,
per-texture blocks and vertices.  A frame is: draw 0, the background, with
the whole state; then one draw per run of quads with the same texture and
blend mode, each with a 4-word state delta pointing at that texture's 3D PDS
block.  The blend mode lives in the small USSE program that block runs
(GPU 0x1e80: PHAS, SOP2M, SOP2 -- only the SOP2 differs between SRC_ALPHA/
ONE_MINUS_SRC_ALPHA, SRC_ALPHA/ONE and DST_COLOR/ZERO); the ADD and MOD
copies go into free space on the code page.

OUTDIR gets the memory images (m_<va>.bin), tmpl.bin (the blocks sgx2d
copies) and pack.txt:

  map VA SIZE        GPU memory to map before the microkernel boots
  img VA FILE        an image to load after the boot
  poke PHYS VALUE    a register to set after the boot (USE_CODE_BASE_3)
  kick PB DET CMD    the rkick arguments
  key VALUE...       addresses and sizes (see the end of main())

Usage: rpack.py CAPDIR RTDIR OUTDIR   (CAPDIR logs/ios/mod/blend, RTDIR
rtemu.py 768 1024 on mod/blend_payload.txt, W/H patched, RT_GPU_BASE=0x87c00000)
"""
import os, struct, subprocess, sys

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
import rgen

W, H = 768, 1024
GL = 0x98956000
STATE, PDS = 0x989b5000, 0x98956000
IDX = 0x98967000
CB = 0x9a060000                        # code base 3: the page is at CB + 0x1000
EXT_VA, EXT_SIZE = 0x9b100000, 0x200000
BGLOAD_VA, BGLOAD_SIZE = 0x9c000000, 0x400000    # see rbatch.py

# the blend program (GPU 0x1e80) and its copies: (code page offset, SOP2)
BLEND_PROGS = ((0xe80, None),                       # SRC_ALPHA, ONE_MINUS_SRC_ALPHA
               (0xf40, 0x809480c590000000),         # SRC_ALPHA, ONE
               (0xf80, 0x80a4008190000000))         # DST_COLOR, ZERO

# tmpl.bin: (name, address, size)
TMPL = (('full', STATE + 0xe0, 0x50),      # draw 0's whole state; word 6 = texture block
        ('fullprog', STATE + 0x140, 0x2c), # its program; word 0 = data pointer
        ('delta', STATE + 0x3c0, 0x20),    # later draws' state; word 3 = texture block
        ('deltaprog', STATE + 0x3e0, 0x2c),
        ('fetch', STATE + 0x180, 0x58),    # vertex fetch; words 0, 4, 8 = rgba, uv, xy
        ('tex', PDS + 0x160, 0x30))        # texture block: word 0 blend program,
                                           # 5 log2 size, 6 address

def main():
    cap, rtdir, out = sys.argv[1:4]
    subprocess.check_call([sys.executable, os.path.join(here, 'rgen.py'), cap, rtdir, out,
                           '--profile', 'modblend', '--fb', '0', '0', '--size', '%dx%d' % (W, H),
                           '--codebase', '0x%x' % CB])
    gpath = os.path.join(out, 'm_%08x.bin' % GL)
    gl = bytearray(open(gpath, 'rb').read())
    struct.pack_into('<I', gl, PDS + 0x138 - GL, BGLOAD_VA)
    open(gpath, 'wb').write(gl)
    cpath = os.path.join(out, 'm_%08x.bin' % (CB + 0x1000))
    code = bytearray(open(cpath, 'rb').read())
    doutu = []
    for off, sop2 in BLEND_PROGS:
        if sop2 is not None:
            assert code[off:off + 0x18] == bytes(0x18)
            code[off:off + 0x18] = code[0xe80:0xe98]
            struct.pack_into('<Q', code, off + 0x10, sop2)
        doutu.append(((0x1000 + off) // 8) << 4 | 3)
    open(cpath, 'wb').write(code)
    t = bytearray()
    lines = []
    for name, va, size in TMPL:
        off = (len(t) + 0x7f) & ~0x7f
        t += b'\0' * (off - len(t)) + gl[va - GL:va - GL + size]
        lines.append('tmpl_%s %d %d' % (name, off, size))
    open(os.path.join(out, 'tmpl.bin'), 'wb').write(t)

    rrun = open(os.path.join(out, 'rrun.sh')).read().splitlines()
    maps = [l.split('"')[1].split()[1:] for l in rrun if l.startswith('echo "map ')]
    maps += [['0x%x' % EXT_VA, '0x%x' % EXT_SIZE], ['0x%x' % BGLOAD_VA, '0x%x' % BGLOAD_SIZE]]
    imgs = [l.split()[1][3:] for l in rrun if l.startswith('dd if=m_')]
    pokes = [l.split()[2:4] for l in rrun if l.startswith('peek w ')]
    kick = [l for l in rrun if 'rkick' in l and l.startswith('echo')][0].split('"')[1].split()[1:4]
    vdm = rgen.words(open(os.path.join(out, 'm_%08x.bin' % rgen.VDM_VA), 'rb').read())
    tail = vdm[vdm.index(0xc0000000) - 4:vdm.index(0xc0000000) + 1]
    # the vertex fetch word follows each draw's index-list words
    fetchw = [vdm[i + 1] for i in range(1, len(vdm)) if vdm[i - 1] == 0x003fffff][0]
    p = ['map %s %s' % tuple(m) for m in maps]
    p += ['img 0x%s %s' % (f.split('.')[0][2:], f) for f in imgs]
    p += ['poke 0x%s 0x%s' % tuple(x) for x in pokes]
    p += ['kick %s %s %s' % tuple(kick)]
    p += ['screen %d %d' % (W, H),
          'consts0 0x%x' % (STATE + 0xa0), 'consts 0x%x' % (STATE + 0x380),
          'idx 0x%x 8192' % IDX, 'vdm 0x%x 0x4000' % rgen.VDM_VA,
          'ext 0x%x 0x%x' % (EXT_VA, EXT_SIZE),
          'fetch 9 0x%08x' % fetchw,
          'blendprogs ' + ' '.join('0x%x' % d for d in doutu),
          'tail ' + ' '.join('0x%08x' % w for w in tail)]
    p += lines
    open(os.path.join(out, 'pack.txt'), 'w').write('\n'.join(p) + '\n')
    os.remove(os.path.join(out, 'rrun.sh'))
    print(open(os.path.join(out, 'pack.txt')).read())

if __name__ == '__main__':
    main()
