#!/usr/bin/env python3
"""rpack.py -- the template pack sgx2d (tools/sgx/lib) builds its frames from.

Everything comes from the two-texture capture (rgen.py --profile twotex, see
rbatch.py for what each block is), at full screen, plus a window of our own
(EXT) for textures, per-texture blocks and vertices.  OUTDIR gets the memory
images (m_<va>.bin), tmpl.bin (the blocks sgx2d copies) and pack.txt:

  map VA SIZE        GPU memory to map before the microkernel boots
  img VA FILE        an image to load after the boot
  poke PHYS VALUE    a register to set after the boot (USE_CODE_BASE_3)
  kick PB DET CMD    the rkick arguments
  key VALUE          addresses and sizes (see below)

Usage: rpack.py CAPDIR RTDIR OUTDIR   (CAPDIR logs/ios/twotex/f, RTDIR
rtemu.py 768 1024 on the twotex payload with RT_GPU_BASE=0x87c00000)
"""
import os, struct, subprocess, sys

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
import rgen
from rbatch import GL, STATE, PDS, IDX, EXT_VA, EXT_SIZE, BGLOAD_VA, BGLOAD_SIZE, W, H

# tmpl.bin layout (offset: block, size)
TMPL = (('full', STATE + 0xe0, 0x50),      # draw 0's whole state; word 6 = texture block
        ('fullprog', STATE + 0x140, 0x2c), # its program; word 0 = data pointer
        ('delta', STATE + 0x300, 0x20),    # later draws' state; word 3 = texture block
        ('deltaprog', STATE + 0x320, 0x2c),
        ('fetch', STATE + 0x360, 0x44),    # vertex fetch; words 0, 4 = (u,v), (x,y) pointers
        ('tex', PDS + 0x200, 0x30))        # texture block; words 5, 6 = log2 size, address

def main():
    cap, rtdir, out = sys.argv[1:4]
    subprocess.check_call([sys.executable, os.path.join(here, 'rgen.py'), cap, rtdir, out,
                           '--profile', 'twotex', '--fb', '0', '0', '--size', '%dx%d' % (W, H),
                           '--codebase', '0x9a060000'])
    gpath = os.path.join(out, 'm_%08x.bin' % GL)
    gl = bytearray(open(gpath, 'rb').read())
    struct.pack_into('<I', gl, PDS + 0x138 - GL, BGLOAD_VA)     # see rbatch.py
    open(gpath, 'wb').write(gl)
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
    p = ['map %s %s' % tuple(m) for m in maps]
    p += ['img 0x%s %s' % (f.split('.')[0][2:], f) for f in imgs]
    p += ['poke 0x%s 0x%s' % tuple(x) for x in pokes]
    p += ['kick %s %s %s' % tuple(kick)]
    p += ['screen %d %d' % (W, H), 'fb 0x%x %d' % (rgen.FB_VA, rgen.FB_STRIDE_PX),
          'consts0 0x%x' % (STATE + 0xa0), 'consts 0x%x' % (STATE + 0x2c0),
          'idx 0x%x 8192' % IDX, 'vdm 0x%x 0x4000' % rgen.VDM_VA,
          'ext 0x%x 0x%x' % (EXT_VA, EXT_SIZE),
          'tail ' + ' '.join('0x%08x' % w for w in tail)]
    p += lines
    open(os.path.join(out, 'pack.txt'), 'w').write('\n'.join(p) + '\n')
    for f in ('rrun.sh',):
        os.remove(os.path.join(out, f))
    print(open(os.path.join(out, 'pack.txt')).read())

if __name__ == '__main__':
    main()
