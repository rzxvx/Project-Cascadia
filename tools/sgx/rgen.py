#!/usr/bin/env python3
"""rgen.py -- replay the captured GLES triangle (M4) under Linux.

Builds, at fixed GPU addresses, everything one render needs and writes it as
images for apple-sgx/mem plus a device script (rrun.sh):

  0x87900000  render target buffers, from tools/iosgpu/rtemu.py (the kext's
              own init + submit code): regions, tail pointers, details, state,
              and the 3D register block at +0xc000
  0x87a00000  parameter buffer: descriptor, page table (+0x10000), two blocks
              (+0x20000: 0x22 pages, +0x50000: 0x80 pages), each a header page
              then data pages; page numbers count from TA_REQ_BASE 0x87800000
  0x87b00000  the TA command (rkick copies it into the render CCB)
  0x98dbc000  the GL buffers from the capture, at their iOS GPU addresses
  0x98f00000  the VDM control stream (iOS had it at 0x90012000, which is our
              framebuffer; nothing in the stream points at itself)
  0x00001000  the GL shader code page (USE code base 3 = 0; overwrites the
              2D engine's programs until the next boot)

Usage: rgen.py CAPDIR RTDIR OUTDIR [--fb X Y]
  --fb    draw straight into the framebuffer (GPU 0x90000000, 768 px lines)
          at X, Y: the 3D pass's emit program (GPU 0x1d40) gets the linear
          PBE state instead of the twiddled 64x64 surface at 0x98ddd000
  CAPDIR  r_<cpu>.bin regions split from logs/ios/tq/gt_r_tri.bin
  RTDIR   rtemu.py output for 64 64 1 2 tri_payload.txt (rt_64x64_*.bin)
"""
import os, struct, sys

TA_BASE = 0x87800000
RT_VA, PB_VA, CMD_VA = 0x87900000, 0x87a00000, 0x87b00000
VDM_VA = 0x98f00000
CMD_SIZE = 0x120
NCORES = 2

def words(b):
    return list(struct.unpack('<%dI' % (len(b) // 4), b))

def pack(w):
    return struct.pack('<%dI' % len(w), *w)

def pb_image():
    """PB as the kext lays it out (0x80bfd51c, 0x80bfd9a0, 0x80bfdaa8)."""
    img = bytearray(0xd1000)
    blocks = [(PB_VA + 0x20000, 0x22000), (PB_VA + 0x50000, 0x80000)]
    pg = lambda va: (va - TA_BASE) >> 12
    for i, (va, size) in enumerate(blocks):
        nxt = blocks[i + 1][0] if i + 1 < len(blocks) else 0
        h = struct.pack('<IHHII', size >> 12, pg(va) + 1, pg(va + size), PB_VA, nxt)
        img[va - PB_VA:va - PB_VA + len(h)] = h
    total = sum(s >> 12 for _, s in blocks)
    end = pg(blocks[-1][0] + blocks[-1][1] + 0x1000)
    d = [0] * 20
    d[0] = 3
    d[1] = total
    d[2] = (end - 1) << 16 | (pg(blocks[0][0]) + 1)
    d[3] = (end - 2) << 16
    d[4] = PB_VA + 0x10000
    d[10] = total + 0x10
    d[11] = blocks[-1][1] >> 12
    d[12] = total
    d[13] = max((blocks[-1][1] >> 12) - 0x100, 0)
    d[14] = blocks[0][0]
    img[0:80] = pack(d)
    return img

def rt_image(rtdir):
    """rtemu.py's buffers (it places them from 0x87900000 in order) + 3D block."""
    img = bytearray(0xd000)
    off = 0
    for i in range(6):
        b = open(os.path.join(rtdir, 'rt_64x64_b%d.bin' % i), 'rb').read()
        img[off:off + len(b)] = b
        off += len(b)
    blk = open(os.path.join(rtdir, 'rt_64x64_blk3d.bin'), 'rb').read()
    img[0xc000:0xc000 + len(blk)] = blk
    return img

def gl_image(cap):
    """GL buffers at 0x98dbc000.. (one mapping), from the CPU regions."""
    img = bytearray(0x46000)
    for gpu, cpu in ((0x98dbc000, 0x8f2000), (0x98dd6000, 0x90a000),
                     (0x98ddb000, 0x91d000), (0x98df2000, 0x977000)):
        b = open(os.path.join(cap, 'r_%08x.bin' % cpu), 'rb').read()
        img[gpu - 0x98dbc000:gpu - 0x98dbc000 + len(b)] = b
    return img

def ta_cmd(rtdir):
    """The render CCB command as 0x80bfca20 builds it (one TA, first + last)."""
    p = words(open(os.path.join(rtdir, 'rt_64x64_payload.bin'), 'rb').read())
    w = lambda n: p[n - 2]
    c = [0] * (CMD_SIZE // 4)
    def put(off, v): c[off // 4] = v & 0xffffffff
    put(0x00, CMD_SIZE)
    put(0x04, 0x73)
    put(0x08, w(18)); put(0x0c, w(19)); put(0x10, w(20))
    put(0x14, 0x0c000000)
    put(0x20, 1)
    put(0x28, PB_VA)
    put(0x50, w(51)); put(0x54, w(52)); put(0x58, w(53))
    put(0x64, 1)
    put(0xbc, w(38))
    for i in range(4): put(0xc0 + 4 * i, w(4 + i))
    put(0xd0, 0x400000)
    put(0xd4, VDM_VA)
    put(0xd8, 0x1a2)
    for n in range(NCORES):
        put(0xdc + 4 * n, w(27) + n * w(28))
        put(0xec + 4 * n, w(8) + n * w(9))
    put(0xfc, 0x1e3ce508); put(0x100, 0x1e3ce508)
    put(0x104, w(11))
    put(0x10c, 0x7fffffff)
    put(0x110, TA_BASE)
    lim = 0x800 if w(39) == 0 else 0x400
    put(0x114, int(w(44) > lim or w(45) > lim))
    put(0x118, w(37))
    return pack(c)

MAPS = ((RT_VA, 0xd000), (PB_VA, 0xd1000), (CMD_VA, 0x1000),
        (0x98dbc000, 0x46000), (VDM_VA, 0x4000))

FB_VA, FB_STRIDE_PX = 0x90000000, 768

def emit_to_fb(code, x, y, w=64, h=64):
    """Rewrite the emit LIMMs r0..r5 (GPU 0x1d60..0x1d88): linear output."""
    from tqgen import limm
    regs = (0x00110000, FB_VA + (y * FB_STRIDE_PX + x) * 4, FB_STRIDE_PX // 2 - 1,
            0, 0, (h - 1) << 12 | (w - 1))
    for i, v in enumerate(regs):
        struct.pack_into('<Q', code, 0xd60 + 8 * i, limm(i, v))

def main():
    cap, rtdir, out = sys.argv[1:4]
    os.makedirs(out, exist_ok=True)
    code = bytearray(open(os.path.join(cap, 'r_00949000.bin'), 'rb').read()[:0x1000])
    if '--fb' in sys.argv:
        i = sys.argv.index('--fb')
        emit_to_fb(code, int(sys.argv[i + 1], 0), int(sys.argv[i + 2], 0))
    vdm = open(os.path.join(cap, 'r_00924000.bin'), 'rb').read()
    imgs = {RT_VA: rt_image(rtdir), PB_VA: pb_image(), CMD_VA: ta_cmd(rtdir),
            0x98dbc000: gl_image(cap), VDM_VA: vdm, 0x1000: code}
    for va, b in imgs.items():
        open(os.path.join(out, 'm_%08x.bin' % va), 'wb').write(b)
    sh = ['#!/bin/sh', '# rrun.sh -- generated by rgen.py: map, boot, load, render',
          'D=/sys/kernel/debug/apple-sgx; cd $(dirname $0)',
          'mount -t debugfs none /sys/kernel/debug 2>/dev/null']
    sh += ['echo "map 0x%x 0x%x" > $D/cmd 2>/dev/null' % m for m in MAPS]
    sh += ['echo 1 > $D/boot; sleep 0.2']
    sh += ['dd if=m_%08x.bin of=$D/mem bs=4096 seek=%d conv=notrunc 2>/dev/null' % (va, va >> 12)
           for va in imgs]
    sh += ['dmesg -c > /dev/null',
           'echo "rkick 0x%x 0x%x 0x%x ${1:-0}" > $D/cmd' % (PB_VA, RT_VA + 0x8000, CMD_VA),
           'sleep 0.5; dmesg | grep -A3 rkick',
           'dd if=$D/mem bs=4096 skip=%d count=20 2>/dev/null > /tmp/rout.bin' % (0x98ddd000 >> 12),
           'echo "out md5 $(md5sum /tmp/rout.bin | cut -c1-8)"; grep "r CCB" $D/regs']
    open(os.path.join(out, 'rrun.sh'), 'w').write('\n'.join(sh) + '\n')
    os.chmod(os.path.join(out, 'rrun.sh'), 0o755)

if __name__ == '__main__':
    main()
