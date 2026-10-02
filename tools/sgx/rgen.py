#!/usr/bin/env python3
"""rgen.py -- replay the captured GLES triangle (M4) under Linux.

Builds, at fixed GPU addresses, everything one render needs and writes it as
images for apple-sgx/mem plus a device script (rrun.sh):

  0x87900000  render target buffers, from tools/iosgpu/rtemu.py (the kext's
              own init + submit code): regions, tail pointers, details, state,
              and the 3D register block at +0xc000
  0x88000000  parameter buffer: descriptor, page table (+0x10000), two blocks
              (+0x20000: 0x22 pages, +0x50000: 0x800 pages), each a header page
              then data pages; page numbers count from TA_REQ_BASE 0x87800000
  0x87b00000  the TA command (rkick copies it into the render CCB)
  0x98dbc000  the GL buffers from the capture, at their iOS GPU addresses
  0x98f00000  the VDM control stream (iOS had it at 0x90012000, which is our
              framebuffer; nothing in the stream points at itself)
  0x00001000  the GL shader code page (USE code base 3 = 0; overwrites the
              2D engine's programs until the next boot)

Usage: rgen.py CAPDIR RTDIR OUTDIR [--profile tri|depth] [--fb X Y] [--reloc BASE]
  --reloc the triangle's GL buffers and VDM stream moved to BASE.. (tri only)
  --teximg PNG  (profile tex) the quad, stretched over the frame, shows PNG
          as a 64x64 texture at 0x98900000
  --size W[xH]  (profiles tex, twotex) a W x H frame; RTDIR must come from rtemu.py W H
  --codebase CB  shader code page at CB + 0x1000, USE_CODE_BASE_3 = CB (the
          2D engine's programs at 0x1000 stay)
  --fb    draw straight into the framebuffer (GPU 0x90000000, 768 px lines)
          at X, Y: the 3D pass's emit program (GPU 0x1d40) gets the linear
          PBE state instead of the twiddled 64x64 surface at 0x98ddd000
  CAPDIR  r_<cpu>.bin regions split from logs/ios/tq/gt_r_tri.bin
  RTDIR   rtemu.py output for 64 64 1 2 tri_payload.txt (rt_64x64_*.bin)
"""
import os, struct, sys

TA_BASE = 0x87800000
RT_VA, PB_VA, CMD_VA = 0x87900000, 0x88000000, 0x87b00000
# the parameter buffer: descriptor, page table (+0x10000), then blocks of a
# header page and data pages (offset, bytes) -- iOS grows PB0 up to 8 MiB;
# too small a buffer forces partial renders mid-frame
PB_BLOCKS = ((0x20000, 0x22000), (0x50000, 0x800000))
PB_SIZE = PB_BLOCKS[-1][0] + PB_BLOCKS[-1][1] + 0x1000
VDM_VA = 0x98f00000
CMD_SIZE = 0x120
NCORES = 2

def words(b):
    return list(struct.unpack('<%dI' % (len(b) // 4), b))

def pack(w):
    return struct.pack('<%dI' % len(w), *w)

def pb_image():
    """PB as the kext lays it out (0x80bfd51c, 0x80bfd9a0, 0x80bfdaa8)."""
    img = bytearray(PB_SIZE)
    blocks = [(PB_VA + off, size) for off, size in PB_BLOCKS]
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

def rt_name(rtdir, part):
    import glob
    return glob.glob(os.path.join(rtdir, 'rt_*x*_%s.bin' % part))[0]

def rt_layout(rtdir):
    """rtemu.py's buffers at the GPU addresses it gave them (from RT_VA, in
    allocation order), then the 3D block (payload w51)."""
    import glob, json
    j = json.load(open(glob.glob(os.path.join(rtdir, 'rt_*x*.json'))[0]))
    blk_va = words(open(rt_name(rtdir, 'payload'), 'rb').read())[51 - 2]
    return j['bufs'], blk_va

def rt_image(rtdir):
    bufs, blk_va = rt_layout(rtdir)
    va = bufs[0]['gpu']               # RT_VA unless rtemu.py ran with RT_GPU_BASE
    img = bytearray(blk_va + 0x1000 - va)
    for i, b in enumerate(bufs):
        data = open(rt_name(rtdir, 'b%d' % i), 'rb').read()
        img[b['gpu'] - va:b['gpu'] - va + len(data)] = data
    blk = open(rt_name(rtdir, 'blk3d'), 'rb').read()
    img[blk_va - va:blk_va - va + len(blk)] = blk
    return img

# Per capture: the GL buffers' window (GPU base, size), {GPU: CPU region},
# and the CPU regions of the shader code page and the VDM control stream.
PROFILES = {
    # gt_r_tri.bin: one triangle
    'tri': dict(gl=(0x98dbc000, 0x46000),
                bufs={0x98dbc000: 0x8f2000, 0x98dd6000: 0x90a000,
                      0x98ddb000: 0x91d000, 0x98df2000: 0x977000},
                code=0x949000, vdm=0x924000),
    # gt_d_on.bin: two triangles, depth test on (the window also covers the
    # render target 0x98ddd000 and the depth buffer, ZLS base 0x98d00000 +
    # 0xe4000; both GPU-only, left zero)
    'depth': dict(gl=(0x98dab000, 0xc7000),
                  bufs={0x98dab000: 0x852000, 0x98dd6000: 0x87a000,
                        0x98e62000: 0x907000},
                  code=0x8bd000, vdm=0x8b9000),
    # gt_t_tex.bin: a 4x4 texture on a quad (logs/ios/tmpl); texture
    # 0x9889d000 (twiddled), output 0x98951000; programs through code base 5
    'tex': dict(gl=(0x9889d000, 0x14b000),
                bufs={0x98940000: 0x902000, 0x989d8000: 0x9ba000,
                      0x989c2000: 0x92d000, 0x9889d000: 0x941000},
                code=0x96d000, vdm=0x948000, cbase=(5,)),
    # gt_t_texblend.bin: the same quad with SRC_ALPHA, ONE_MINUS_SRC_ALPHA
    'texblend': dict(gl=(0x9889d000, 0x14b000),
                     bufs={0x989c7000: 0x9aa000, 0x98995000: 0x912000,
                           0x989c2000: 0x92d000, 0x9889d000: 0x941000},
                     code=0x96d000, vdm=0x974000, cbase=(3, 5)),
    # gt_2t.bin (logs/ios/twotex): two quads, two textures (8x8 at
    # 0x980cd000, 4x4 at 0x980cf000), blending on; output 0x980d1000
    'twotex': dict(gl=(0x980ac000, 0x4a000),
                   bufs={0x980ac000: 0x91b000, 0x980c6000: 0x935000,
                         0x980cd000: 0x969000, 0x980cf000: 0x96a000,
                         0x980e6000: 0x971000},
                   code=0x985000, vdm=0x981000, cbase=(3,)),
    # gt_m_blend.bin (logs/ios/mod): texture x vertex colour, two quads, two
    # textures, SRC_ALPHA/ONE_MINUS_SRC_ALPHA; vertices [rgba][uv][xy], 32 B
    'modblend': dict(gl=(0x98956000, 0x6f000),
                     bufs={0x98956000: 0x8a2000, 0x98967000: 0x8b4000,
                           0x989b5000: 0x931000},
                     code=0x8f7000, vdm=0x8f3000, cbase=(3,)),
    # gt_t_vcolor.bin: a triangle with a colour per vertex
    'vcolor': dict(gl=(0x9889d000, 0x14b000),
                   bufs={0x989c7000: 0x9aa000, 0x98995000: 0x912000,
                         0x989c2000: 0x92d000},
                   code=0x96d000, vdm=0x948000, cbase=(5,)),
}

def gl_image(cap, prof):
    """The GL buffers, at their iOS GPU addresses, as one mapping."""
    base, size = prof['gl']
    img = bytearray(size)
    for gpu, cpu in prof['bufs'].items():
        b = open(os.path.join(cap, 'r_%08x.bin' % cpu), 'rb').read()
        img[gpu - base:gpu - base + len(b)] = b
    return img

def ta_cmd(rtdir):
    """The render CCB command as 0x80bfca20 builds it (one TA, first + last)."""
    p = words(open(rt_name(rtdir, 'payload'), 'rb').read())
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

MAPS = ((PB_VA, PB_SIZE), (CMD_VA, 0x1000), (VDM_VA, 0x4000))

FB_VA, FB_STRIDE_PX = 0x90000000, 768
SGX_REGS = 0x35100000            # broadcast register bank (physical)

def emit_to_fb(code, x, y, w=64, h=64):
    """Rewrite the emit LIMMs r0..r5 (GPU 0x1d60..0x1d88): linear output."""
    from tqgen import limm
    regs = (0x00110000, FB_VA + (y * FB_STRIDE_PX + x) * 4, FB_STRIDE_PX // 2 - 1,
            0, 0, (h - 1) << 12 | (w - 1))
    for i, v in enumerate(regs):
        struct.pack_into('<Q', code, 0xd60 + 8 * i, limm(i, v))

# --reloc: the triangle's GL buffers moved to our own addresses.  Pointers
# appear as plain addresses, as (address >> 4) under a 4-bit tag, and in PDS
# data words as bits 30:4 under a 5-bit tag (bit 31 implied).
RELOC_TRI = ((0x98df2000, 0x10000, 0x00000), (0x98dbc000, 0x10000, 0x10000),
             (0x98dd6000, 0x4000, 0x20000), (0x98ddb000, 0x1000, 0x24000),
             (0x98ddd000, 0x14000, 0x30000), (VDM_VA, 0x4000, 0x50000))

def reloc_addr(a, base):
    for old, size, new in RELOC_TRI:
        if old <= a < old + size:
            return base + new + (a - old)
    return None

def reloc_word(x, base):
    r = reloc_addr(x, base)
    if r is not None:
        return r
    r = reloc_addr((x & 0x0fffffff) << 4, base)
    if r is not None and x & 0x0fffffff:
        return (x & 0xf0000000) | r >> 4
    r = reloc_addr((x & 0x07ffffff) << 4 | 0x80000000, base)
    if r is not None and x & 0x07ffffff:
        return (x & 0xf8000000) | (r >> 4) & 0x07ffffff
    return x

def reloc_buf(b, base):
    w = [reloc_word(x, base) for x in words(bytes(b))]
    return bytearray(pack(w))

def twiddle(rgba, w, h):
    """RGBA rows -> the GPU's texture order: Morton, y in the even bits."""
    out = bytearray(w * h * 4)
    for y in range(h):
        for x in range(w):
            i = 0
            for b in range(max(w, h).bit_length()):
                i |= ((y >> b) & 1) << (2 * b) | ((x >> b) & 1) << (2 * b + 1)
            out[i * 4:i * 4 + 4] = rgba[(y * w + x) * 4:(y * w + x) * 4 + 4]
    return out

TEX_VA = 0x98900000

def own_texture(gl, path):
    """--teximg (profile tex): the quad shows PATH (64x64) over the whole frame."""
    from PIL import Image
    base = PROFILES['tex']['gl'][0]
    img = Image.open(path).convert('RGBA').resize((64, 64))
    t = twiddle(img.tobytes(), 64, 64)
    gl[TEX_VA - base:TEX_VA - base + len(t)] = t
    desc = 0x989d8000 + 0x1e0 - base           # the sampler's descriptor
    struct.pack_into('<II', gl, desc + 0x14, 0x0c060006, TEX_VA)
    verts = 0x98940000 + 0x2b0 - base          # six (u, v, x, y) vertices
    for i in range(6):
        u, v, x, y = struct.unpack_from('<4f', gl, verts + 16 * i)
        struct.pack_into('<2f', gl, verts + 16 * i + 8, 1.0 if x > 0 else -1.0,
                         1.0 if y > 0 else -1.0)

# Where each profile keeps the frame size (found with gltrace tmplsz): the
# state buffer, its header (+0x10: last 32-pixel tile x << 16 | y), the tile
# clip pairs (0x80000000 | last x, last y), the clear program's constants
# (2W, 2H), the viewport (W/2, W/2, H/2, H/2), and the 3D PDS block with the
# output descriptor (+0x134: log2 w/h).
SIZE_FIELDS = {
    'tex': dict(state=0x98940000, clips=(0x90, 0x1dc), clear=(0xf8, 0x108),
                viewport=0x1e4, pds=0x989d8000),
    'twotex': dict(state=0x980e6000, clips=(0xfc,), clear=(), viewport=0x104,
                   pds=0x980ac000),
    'modblend': dict(state=0x989b5000, clips=(0xfc,), clear=(), viewport=0x104,
                     pds=0x98956000),
}

def resize(gl, prof_name, w, h):
    """--size WxH: a W x H frame (RTDIR from rtemu.py W H)."""
    f = SIZE_FIELDS[prof_name]
    base = PROFILES[prof_name]['gl'][0]
    st = f['state'] - base
    tx, ty = (w + 31) // 32 - 1, (h + 31) // 32 - 1
    struct.pack_into('<I', gl, st + 0x10, tx << 16 | ty)
    for c in f['clips']:
        struct.pack_into('<II', gl, st + c, 0x80000000 | tx, ty)
    if f['clear']:
        struct.pack_into('<f', gl, st + f['clear'][0], 2.0 * w)
        struct.pack_into('<f', gl, st + f['clear'][1], 2.0 * h)
    struct.pack_into('<4f', gl, st + f['viewport'], w / 2, w / 2, h / 2, h / 2)
    lw, lh = (w - 1).bit_length(), (h - 1).bit_length()
    struct.pack_into('<I', gl, f['pds'] + 0x134 - base, 0x0c000000 | lw << 16 | lh)

def main():
    cap, rtdir, out = sys.argv[1:4]
    os.makedirs(out, exist_ok=True)
    prof = PROFILES['tri']
    if '--profile' in sys.argv:
        prof = PROFILES[sys.argv[sys.argv.index('--profile') + 1]]
    maps = ((rt_layout(rtdir)[0][0]['gpu'], len(rt_image(rtdir))),) + MAPS + (prof['gl'],)
    code = bytearray(open(os.path.join(cap, 'r_%08x.bin' % prof['code']), 'rb').read()[:0x1000])
    if '--fb' in sys.argv:
        i = sys.argv.index('--fb')
        fbxy = (int(sys.argv[i + 1], 0), int(sys.argv[i + 2], 0))
    vdm = open(os.path.join(cap, 'r_%08x.bin' % prof['vdm']), 'rb').read()
    fsize = (64, 64)
    if '--size' in sys.argv:
        a = sys.argv[sys.argv.index('--size') + 1].split('x')
        fsize = (int(a[0], 0), int(a[-1], 0))
    if '--fb' in sys.argv:
        emit_to_fb(code, fbxy[0], fbxy[1], *fsize)
    rt_va = rt_layout(rtdir)[0][0]['gpu']
    imgs = {rt_va: rt_image(rtdir), PB_VA: pb_image(), CMD_VA: ta_cmd(rtdir),
            prof['gl'][0]: gl_image(cap, prof), VDM_VA: vdm, 0x1000: code}
    if '--size' in sys.argv:
        pname = sys.argv[sys.argv.index('--profile') + 1] if '--profile' in sys.argv else 'tri'
        resize(imgs[prof['gl'][0]], pname, *fsize)
    if '--teximg' in sys.argv:
        own_texture(imgs[prof['gl'][0]], sys.argv[sys.argv.index('--teximg') + 1])
    if '--reloc' in sys.argv:
        base = int(sys.argv[sys.argv.index('--reloc') + 1], 0)
        gl = imgs.pop(prof['gl'][0])
        vdm = imgs.pop(VDM_VA)
        win = bytearray(0x54000)
        for old, size, new in RELOC_TRI:
            part = vdm if old == VDM_VA else gl[old - prof['gl'][0]:old - prof['gl'][0] + size]
            data = old in (0x98dd6000, 0x98ddb000, 0x98ddd000)  # indices, colours, output
            win[new:new + len(part)] = part if data else reloc_buf(part, base)
        imgs[base] = win
        imgs[CMD_VA] = reloc_buf(imgs[CMD_VA], base)
        rt = imgs[rt_va]
        blk = rt_layout(rtdir)[1] - rt_va
        rt[blk:blk + 0x1000] = reloc_buf(rt[blk:blk + 0x1000], base)
        if '--fb' not in sys.argv:
            from tqgen import limm
            struct.pack_into('<Q', code, 0xd68, limm(1, base + 0x30000))
        maps = maps[:3] + ((base, 0x54000),)
    post = []
    if '--codebase' in sys.argv:
        # GL's programs use USE_CODE_BASE_3 or _5 (0 on iOS, code at 0x1000..):
        # point the base at CB and put the page at CB + 0x1000 instead
        cb = int(sys.argv[sys.argv.index('--codebase') + 1], 0)
        imgs[cb + 0x1000] = imgs.pop(0x1000)
        maps = maps + ((cb + 0x1000, 0x1000),)
        for n in prof.get('cbase', (3,)):
            post.append('peek w %x %x > /dev/null' % (SGX_REGS + 0xa0c + 4 * n, cb >> 6))
    out_va = base + 0x30000 if '--reloc' in sys.argv else 0x98ddd000
    for va, b in imgs.items():
        open(os.path.join(out, 'm_%08x.bin' % va), 'wb').write(b)
    sh = ['#!/bin/sh', '# rrun.sh -- generated by rgen.py: map, boot, load, render',
          'D=/sys/kernel/debug/apple-sgx; cd $(dirname $0)',
          'mount -t debugfs none /sys/kernel/debug 2>/dev/null']
    sh += ['echo "map 0x%x 0x%x" > $D/cmd 2>/dev/null' % m for m in maps]
    sh += ['echo 1 > $D/boot; sleep 0.2']
    sh += ['dd if=m_%08x.bin of=$D/mem bs=4096 seek=%d conv=notrunc 2>/dev/null' % (va, va >> 12)
           for va in imgs]
    sh += post
    sh += ['dmesg -c > /dev/null',
           'echo "rkick 0x%x 0x%x 0x%x ${1:-0}" > $D/cmd' % (PB_VA, rt_layout(rtdir)[0][4]['gpu'],
                                                            CMD_VA),
           'sleep 0.5; dmesg | grep -A3 rkick',
           'dd if=$D/mem bs=4096 skip=%d count=20 2>/dev/null > /tmp/rout.bin' % (out_va >> 12),
           'echo "out md5 $(md5sum /tmp/rout.bin | cut -c1-8)"; grep "r CCB" $D/regs']
    open(os.path.join(out, 'rrun.sh'), 'w').write('\n'.join(sh) + '\n')
    os.chmod(os.path.join(out, 'rrun.sh'), 0o755)

if __name__ == '__main__':
    main()
