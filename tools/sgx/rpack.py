#!/usr/bin/env python3
"""rpack.py -- the template pack sgx2d (tools/sgx/lib) builds its frames from.

From the texture x vertex colour capture (logs/ios/mod, rgen.py --profile
modblend), at full screen, plus a window of our own (EXT) for textures,
per-texture blocks and vertices.  A frame is: draw 0, the background, with
the whole state; then one draw per run of quads with the same texture and
blend mode, each with a 4-word state delta pointing at that texture's 3D PDS
block.  The blend mode lives in the small USSE program that block runs
(programs.py: pixel_blend, pixel_add, pixel_mod -- SRC_ALPHA/
ONE_MINUS_SRC_ALPHA, SRC_ALPHA/ONE and DST_COLOR/ZERO).  The code page is
ours (programs.py); the state and PDS blocks are still the capture's.

OUTDIR gets the memory images (m_<va>.bin), tmpl.bin (the blocks sgx2d
copies) and pack.txt:

  map VA SIZE        GPU memory to map before the microkernel boots
  img VA FILE        an image to load after the boot
  poke PHYS VALUE    a register to set after the boot (USE_CODE_BASE_3)
  kick PB DET CMD    the rkick arguments
  key VALUE...       addresses and sizes (see the end of main()); the Mesa
                     driver reads the same file (mesa/files/.../sgx_frame.c)

Usage: rpack.py CAPDIR RTDIR OUTDIR   (CAPDIR logs/ios/mod/blend, RTDIR
rtemu.py 768 1024 on mod/blend_payload.txt, W/H patched, RT_GPU_BASE=0x87c00000)
"""
import os, struct, subprocess, sys

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
import programs, rgen

W, H = 768, 1024
GL = 0x98956000
STATE, PDS = 0x989b5000, 0x98956000
IDX = 0x98967000
# code base 3: the page is at CB + 0x1000.  The kernel points USE_CODE_BASE_3
# and _5 here itself (apple_sgx.h, SGX_CODE_BASE); sgx2d checks they agree.
CB = 0x9a000000
EXT_VA, EXT_SIZE = 0x9b400000, 0x400000
# The background object reloads every tile from the output descriptor (3D
# PDS block +0x120): with blending on it cannot know a tile is covered, and
# after a partial render (the parameter buffer ran out mid-frame) it must
# bring back what the first pass drew.  So it reads the framebuffer itself,
# linear (the 2D engine's format: stride field, size, 0x10000000).
FB_LOAD = ((768 // 4 - 2) << 16 | 0x0e90, 0xcc000000 | (768 - 1) << 12 | (1024 - 1),
           rgen.FB_VA, 0x10000000)
TEXHEAP_VA, TEXHEAP_CHUNK, TEXHEAP_N = 0x9d000000, 0x4000000, 1  # texels, 64 MiB

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
    os.makedirs(out, exist_ok=True)
    page = os.path.join(out, 'code.bin')
    open(page, 'wb').write(programs.code_page())
    subprocess.check_call([sys.executable, os.path.join(here, 'rgen.py'), cap, rtdir, out,
                           '--profile', 'modblend', '--fb', '0', '0', '--size', '%dx%d' % (W, H),
                           '--codebase', '0x%x' % CB, '--code', page])
    os.remove(page)
    gpath = os.path.join(out, 'm_%08x.bin' % GL)
    gl = bytearray(open(gpath, 'rb').read())
    struct.pack_into('<4I', gl, PDS + 0x130 - GL, *FB_LOAD)
    open(gpath, 'wb').write(gl)
    doutu = [programs.doutu_index(programs.LAYOUT['pixel_' + m]) << 4 | 3
             for m in ('blend', 'add', 'mod')]
    t = bytearray()
    lines = []
    for name, va, size in TMPL:
        off = (len(t) + 0x7f) & ~0x7f
        t += b'\0' * (off - len(t)) + gl[va - GL:va - GL + size]
        lines.append('tmpl_%s %d %d' % (name, off, size))
    open(os.path.join(out, 'tmpl.bin'), 'wb').write(t)

    rrun = open(os.path.join(out, 'rrun.sh')).read().splitlines()
    maps = [l.split('"')[1].split()[1:] for l in rrun if l.startswith('echo "map ')]
    maps += [['0x%x' % EXT_VA, '0x%x' % EXT_SIZE]]
    maps += [['0x%x' % (TEXHEAP_VA + i * TEXHEAP_CHUNK), '0x%x' % TEXHEAP_CHUNK]
             for i in range(TEXHEAP_N)]
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
          'pds 0x%x' % PDS,     # the 3D PDS block: Mesa's clears point it at their target
          'ext 0x%x 0x%x' % (EXT_VA, EXT_SIZE),
          'texheap 0x%x 0x%x' % (TEXHEAP_VA, TEXHEAP_CHUNK * TEXHEAP_N),
          'fetch 9 0x%08x' % fetchw,
          'blendprogs ' + ' '.join('0x%x' % d for d in doutu),
          'tail ' + ' '.join('0x%08x' % w for w in tail)]
    p += lines
    open(os.path.join(out, 'pack.txt'), 'w').write('\n'.join(p) + '\n')
    os.remove(os.path.join(out, 'rrun.sh'))
    print(open(os.path.join(out, 'pack.txt')).read())

if __name__ == '__main__':
    main()
