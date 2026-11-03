#!/usr/bin/env python3
"""rbatch.py -- one full-screen frame of sprite batches, one texture each.

Built from the two-texture capture (logs/ios/twotex, rgen.py --profile
twotex).  Its first draw carries the whole state; every later draw only
(a) re-runs the constants program (state +0x2c0, shared), (b) a 4-word state
delta -- mask 0x40, the pixel program, 0x0803e000, and the 3D PDS block that
loads its texture descriptor -- and (c) a vertex fetch block pointing at its
vertices.  This builds the VDM stream from those pieces:

  draw 0  background: a full-screen opaque quad (clears the frame)
  draw k  the sprites of texture k

Textures (power-of-two, twiddled), the per-draw blocks and the vertex buffer
live in a window of our own at EXT_VA.

Usage: rbatch.py CAPDIR RTDIR OUTDIR BG.png SPRITE.png [SPRITE.png ...] [-n N] [-f FRAMES]
  CAPDIR logs/ios/twotex/f, RTDIR rtemu.py 768 1024 on the twotex payload
"""
import os, random, struct, subprocess, sys

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
import rgen

W, H = 768, 1024
GL = 0x980ac000                     # the twotex window
STATE, PDS = 0x980e6000, 0x980ac000
EXT_VA, EXT_SIZE = 0x9b100000, 0x200000
# the background object loads the previous frame from the output descriptor
# (3D PDS block +0x120) for every tile it cannot prove covered -- with
# blending on, all of them; give it a full-size surface of its own
BGLOAD_VA, BGLOAD_SIZE = 0x9c000000, 0x400000
VB_OFF = 0x100000                   # vertex buffer inside EXT
IDX = 0x980c6000                    # captured index buffer: 0..8191

def p27(a):
    """PDS data pointer: bits 30:4 under the 5-bit tag 0x10."""
    return 0x10000000 | (a >> 4) & 0x07ffffff

def vdm4(tag, a):
    return tag << 28 | a >> 4

class Ext:
    def __init__(self):
        self.img = bytearray(EXT_SIZE)
        self.top = 0
    def alloc(self, data, align=0x40):
        self.top = (self.top + align - 1) & ~(align - 1)
        va = EXT_VA + self.top
        self.img[self.top:self.top + len(data)] = data
        self.top += len(data)
        assert self.top <= VB_OFF
        return va

def texture(path):
    from PIL import Image
    im = Image.open(path).convert('RGBA')
    w = 1 << max(2, (im.width - 1).bit_length())
    h = 1 << max(2, (im.height - 1).bit_length())
    im = im.resize((w, h))
    return rgen.twiddle(im.tobytes(), w, h), w, h

def quad(x, y, w, h):
    nx = lambda px: px / W * 2 - 1
    ny = lambda py: py / H * 2 - 1
    x0, y0, x1, y1 = nx(x), ny(y), nx(x + w), ny(y + h)
    v = ((0, 0, x0, y0), (1, 0, x1, y0), (1, 1, x1, y1),
         (0, 0, x0, y0), (1, 1, x1, y1), (0, 1, x0, y1))
    return b''.join(struct.pack('<4f', *p) for p in v)

def main():
    args = [a for a in sys.argv[1:]]
    n = int(args.pop(args.index('-n') + 1)) if '-n' in args else 40
    if '-n' in args: args.remove('-n')
    frames = int(args.pop(args.index('-f') + 1)) if '-f' in args else 300
    if '-f' in args: args.remove('-f')
    cap, rtdir, out, bg = args[:4]
    sprites = args[4:]
    subprocess.check_call([sys.executable, os.path.join(here, 'rgen.py'), cap, rtdir, out,
                           '--profile', 'twotex', '--fb', '0', '0', '--size', '%dx%d' % (W, H),
                           '--codebase', '0x9a060000'])
    gpath = os.path.join(out, 'm_%08x.bin' % GL)
    gl = bytearray(open(gpath, 'rb').read())
    ext = Ext()
    rd = lambda va: struct.unpack_from('<I', gl, va - GL)[0]
    blk = lambda va, n: bytes(gl[va - GL:va - GL + n])

    def tex_block(path):
        """A copy of the second texture's 3D PDS block with our texture."""
        t, w, h = texture(path)
        tva = ext.alloc(t, 0x1000)
        b = bytearray(blk(PDS + 0x200, 0x30))
        struct.pack_into('<II', b, 0x14, 0x0c000000 | (w.bit_length() - 1) << 16 |
                         (h.bit_length() - 1), tva)
        return ext.alloc(b)

    def fetch_block(vb):
        """A copy of a vertex fetch block (state +0x360) reading vb."""
        b = bytearray(blk(STATE + 0x360, 0x44))
        struct.pack_into('<I', b, 0x0, vb)
        struct.pack_into('<I', b, 0x10, vb + 8)
        return ext.alloc(b)

    struct.pack_into('<I', gl, PDS + 0x138 - GL, BGLOAD_VA)
    # draw 0: the background, through the first draw's own blocks
    bgtex = tex_block(bg)
    if os.environ.get('RB_EXT0'):                          # experiment: copies in EXT
        b = bytearray(blk(STATE + 0xe0, 0x50))             # full state data
        struct.pack_into('<I', b, 0x18, p27(bgtex))        # its texture block
        data0 = ext.alloc(b, 0x20)
        st0 = bytearray(blk(STATE + 0x140, 0x2c))          # state program
        struct.pack_into('<I', st0, 0, data0)
        st0 = ext.alloc(st0)
    else:                                                  # in place, in the window
        struct.pack_into('<I', gl, STATE + 0xe0 + 0x18 - GL, p27(bgtex))
        st0 = STATE + 0x140
    vb = EXT_VA + VB_OFF
    stream = [vdm4(4, STATE + 0xa0), 0x1000e102, vdm4(4, st0), 0x12022206,
              0x81c00006, IDX, 0x70000000, 0x003fffff,
              vdm4(15, fetch_block(vb)), 0x05800403]
    vb += 6 * 16
    for path in sprites:                                   # draws 1..: one per texture
        tb = tex_block(path)
        d = bytearray(blk(STATE + 0x300, 0x20))
        struct.pack_into('<I', d, 0xc, p27(tb))
        dva = ext.alloc(d, 0x20)
        p = bytearray(blk(STATE + 0x320, 0x2c))
        struct.pack_into('<I', p, 0, dva)
        pva = ext.alloc(p)
        stream += [vdm4(4, STATE + 0x2c0), 0x1000e102, vdm4(4, pva), 0x12022201,
                   0x81c00000 | 6 * n, IDX, 0x70000000, 0x003fffff,
                   vdm4(15, fetch_block(vb)), 0x05800403]
        vb += 6 * 16 * n
    stream += [vdm4(4, STATE + 0x60), 0x0800e100, vdm4(6, STATE), 0x1a022201, 0xc0000000]
    open(gpath, 'wb').write(gl)
    open(os.path.join(out, 'm_%08x.bin' % rgen.VDM_VA), 'wb').write(rgen.pack(stream).ljust(0x4000, b'\0'))

    # the vertex buffer: background quad, then n sprites per texture, per frame
    rnd = random.Random(2)
    sp = [[rnd.uniform(0, W - 128), rnd.uniform(0, H - 128), rnd.choice((48, 64, 96, 128)),
           rnd.uniform(-5, 5), rnd.uniform(-5, 5)] for _ in range(n * len(sprites))]
    fr = bytearray()
    for f in range(frames):
        fr += quad(0, 0, W, H)
        for s in sp:
            s[0] += s[3]; s[1] += s[4]
            if not 0 <= s[0] <= W - s[2]: s[3] = -s[3]; s[0] = min(max(s[0], 0), W - s[2])
            if not 0 <= s[1] <= H - s[2]: s[4] = -s[4]; s[1] = min(max(s[1], 0), H - s[2])
            fr += quad(s[0], s[1], s[2], s[2])
    fsize = 96 * (1 + n * len(sprites))
    ext.img[VB_OFF:VB_OFF + fsize] = fr[:fsize]
    open(os.path.join(out, 'm_%08x.bin' % EXT_VA), 'wb').write(ext.img)
    open(os.path.join(out, 'frames.bin'), 'wb').write(fr)
    rrun = open(os.path.join(out, 'rrun.sh')).read()
    rrun = rrun.replace('echo 1 > $D/boot', 'echo "map 0x%x 0x%x" > $D/cmd 2>/dev/null\n'
                        'echo "map 0x%x 0x%x" > $D/cmd 2>/dev/null\n'
                        'echo 1 > $D/boot' % (EXT_VA, EXT_SIZE, BGLOAD_VA, BGLOAD_SIZE))
    rrun = rrun.replace('dmesg -c > /dev/null', 'dd if=m_%08x.bin of=$D/mem bs=4096 seek=%d '
                        'conv=notrunc 2>/dev/null\ndmesg -c > /dev/null' % (EXT_VA, EXT_VA >> 12))
    open(os.path.join(out, 'rrun.sh'), 'w').write(rrun)
    det = rrun.split('rkick 0x87a00000 ')[1].split()[0]
    sh = """#!/bin/sh
# anim.sh -- generated by rbatch.py: %d textures x %d sprites, %d frames
D=/sys/kernel/debug/apple-sgx; cd $(dirname $0)
i=0; while [ $i -lt %d ]; do
  dd if=frames.bin of=/tmp/f.bin bs=%d skip=$i count=1 2>/dev/null
  dd if=/tmp/f.bin of=$D/mem bs=4096 seek=%d conv=notrunc 2>/dev/null
  echo "rkick 0x87a00000 %s 0x87b00000" > $D/cmd || { echo "frame $i failed"; exit 1; }
  i=$((i+1))
done
echo "%d frames"; grep "r CCB" $D/regs
""" % (len(sprites), n, frames, frames, fsize, (EXT_VA + VB_OFF) >> 12, det, frames)
    open(os.path.join(out, 'anim.sh'), 'w').write(sh)
    os.chmod(os.path.join(out, 'anim.sh'), 0o755)
    print('textures %d, sprites %d, ext used 0x%x, frame %d bytes' %
          (1 + len(sprites), n * len(sprites), ext.top, fsize))

if __name__ == '__main__':
    main()
