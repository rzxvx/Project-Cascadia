#!/usr/bin/env python3
"""tqgen.py -- build an SGX543 transfer (2D blit) from parameters, no iOS data.

Pieces, at the GPU addresses the replay used (the driver will choose later):
  code   @0x1000      USSE programs (pixel copy, end-of-tile, PBE emit)
  block  @0x980f3000  PDS programs + their data segments
  param  @0x8c006000  objects + region header
  cmd    TQ command (0x140 bytes) for the kernel CCB
"""
import struct

def limm(reg, imm, bank_pa=False, end=False, bank=None):
    """LIMM rN/paN <- imm (encoding checked against the GL programs);
    bank: 0 temp, 1 output, 2 primary attribute (bits 33:32)"""
    w = 0xfca0000000000000 | (0x0000000200000000 if bank_pa else 0)
    if bank is not None:
        w = 0xfca0000000000000 | (bank & 3) << 32
    w |= (reg & 0x7f) << 21
    w |= ((imm >> 26) & 0x3f) << 44 | ((imm >> 21) & 0x1f) << 36 | (imm & 0x1fffff)
    if end:
        w |= 1 << 50
    return w

PHAS = 0xfa44070000000000
# ---- USSE programs ------------------------------------------------------
FILL = None                 # set to an ARGB colour to make the pixel program a fill

def prog_copy():            # 0x1400: o0 = pa0 (the sampled texel), end
    if FILL is not None:    # fill: o0 = constant
        return [PHAS, limm(0, FILL, bank=1, end=True)]
    return [PHAS, 0x50850009a0000000]
def prog_eot0():            # 0x1c00
    return [PHAS, 0xf834800000000000]
def prog_eot1():            # 0x1c40, then its second phase at 0x1c80
    return [PHAS, limm(0, 0), limm(1, 0), 0xfb26000081200000]
def prog_eot1b():           # 0x1c80
    return [PHAS, limm(1, 0, True), limm(2, 1, True), limm(4, 2, True), limm(8, 3, True, end=True)]
def prog_emit(pbe):         # 0x1d40: load the six PBE state words, emit
    return [PHAS, 0x488b0281a00c0000, 0xe9a30084a0000000, 0xf920000000000000] + \
           [limm(i, pbe[i]) for i in range(6)] + [0xfb24000003200082]

def code_page(pbe, size=0x7000):
    b = bytearray(size)
    for va, prog in ((0x1400, prog_copy()), (0x1c00, prog_eot0()), (0x1c40, prog_eot1()),
                     (0x1c80, prog_eot1b()), (0x1d40, prog_emit(pbe))):
        for i, w in enumerate(prog):
            struct.pack_into('<Q', b, va - 0x1000 + 8 * i, w)
    return bytes(b)

# ---- PDS programs and data segments (the level block) -------------------
# Main end-of-tile PDS program: its data segment (5 x 16 bytes) holds the DOUTU
# words of the three USSE programs and constants; the code follows.
PDS_MAIN = [0x9380000c, 0xcf800630, 0xcf800870, 0xf7700070, 0xcf086070, 0xc7606030,
            0xcf800a70, 0xf7700070, 0xcf0a6070, 0xc7606030, 0x07600b05, 0xaf000000,
            0x94800014, 0xcf0c8270, 0x8700c100, 0xcf800c70, 0xc70e6070, 0x100010e0,
            0x07018185, 0xaf000000, 0x07041185, 0xaf000000]
PDS_MAIN_CONST = [0xffff0000, 0x000000ff, 0x0000ff00, 0x000000ff, 0x0000ff00,
                  0x00000100, 0x00020000, 0xfffeffff, 0x00000000, 0x00030000]
# Per-object state program: DOUTU (start the pixel shader) + DOUTT (texture
# state from the data segment), end.
PDS_TEX = [0x07000185, 0x07000c02, 0x07041004, 0xaf000000]
PDS_END = [0xaf000000]

def doutu(va):              # USSE program address word: (instr idx << 4) | code base 10
    return ((va // 8) << 4) | 0xa

def tex_words(addr, log2w, log2h, fmt):
    """texture state for a source/destination: format word, size word, address"""
    return [fmt, 0x0c000000 | log2w << 16 | log2h, addr, 0]

def tex_words_linear(addr, w, h, stride_px):
    """a linear BGRA source (from iOS sampling an IOSurface): stride field in
    format bits 16-23, (w-1, h-1) in the size word, 0x10000000 marks linear"""
    return [(stride_px // 4 - 2) << 16 | 0x0e90, 0xcc000000 | (w - 1) << 12 | (h - 1), addr, 0x10000000]

def block(src, dst):
    """src/dst = (addr, log2w, log2h, fmtword), or ('lin', addr, w, h, stride_px)
    for a linear source.  Returns 0x1c0 bytes."""
    w = [0] * (0x1c0 // 4)
    w[0x00 // 4] = doutu(0x1c40); w[0x08 // 4] = doutu(0x1d40); w[0x10 // 4] = doutu(0x1c00)
    w[0x1c // 4: 0x1c // 4 + len(PDS_MAIN_CONST)] = PDS_MAIN_CONST
    w[0x50 // 4: 0x50 // 4 + len(PDS_MAIN)] = PDS_MAIN
    for base, t in ((0x100, dst), (0x160, src)):
        w[base // 4] = PDS_END[0]
        d = base + 0x20
        w[d // 4: d // 4 + 4] = [doutu(0x1400), 0xa, 0, 0xf800]
        w[d // 4 + 4: d // 4 + 8] = tex_words_linear(*t[1:]) if t[0] == 'lin' else tex_words(t[0], t[1], t[2], t[3])
        w[(d + 0x20) // 4:(d + 0x20) // 4 + 4] = PDS_TEX
    return struct.pack('<%dI' % len(w), *w)

# ---- parameter page: background object, quad object, region header ------
def half(x):
    return struct.unpack('<H', struct.pack('<e', x))[0]

def xy(x, y):
    return half(y) << 16 | half(x)

def pos(px):
    """a vertex coordinate: 16-bit fixed point, 4 fraction bits, +1024 px guard
    band (measured: 0x4100 -> 16 px, 0x4500 -> 80 px; it only looks like a half)"""
    return 0x4000 + int(round(px * 16))

def xy_px(x, y):
    return pos(x) << 16 | pos(y)          # x in the high half (100x37 test)

def pds_ptr(va, flag=0):
    return flag << 28 | ((va - 0x80000000) >> 4)

def param_page(block_va, x0, y0, x1, y1, bx1=None, by1=None):
    """quad corners in transfer units (1.0 = 32 px, origin 2.0); the background
    triangle spans the render box (2,2)..(bx1,by1), default the quad's far corner"""
    bx1 = x1 if bx1 is None else bx1
    by1 = y1 if by1 is None else by1
    w = [0] * (0x2000 // 4)
    bg = [pds_ptr(block_va + 0x100), 0x0801e000, pds_ptr(block_va + 0x120, 1), 0, 0, 0x3f800000, 0, 0,
          0x3f800000, 2, 0, xy(2, 2), 0x3f800000, xy(2, by1), 0x3f800000, xy(bx1, 2), 0x3f800000]
    q = [pds_ptr(block_va + 0x160), 0x0801e000, pds_ptr(block_va + 0x180, 1), 0, 0, 0, 0x3f800000,
         0x3f800000, 0x3f800000, 0x3f800000, 0, 0x01d80000, 0x4e504a30, 2, 0,
         xy(x0, y0), 0x3f800000, xy(x1, y0), 0x3f800000, xy(x1, y1), 0x3f800000, xy(x0, y1), 0x3f800000]
    w[0:len(bg)] = bg
    w[0x40:0x40 + len(q)] = q
    w[0x80:0x84] = [0x80000000, 0x46400f03, 0x0c000000 | ((0x6100 >> 2) & 0xffffff) | 0xb, 0xc0000000]
    return struct.pack('<%dI' % len(w), *w)

# ---- the TQ command ------------------------------------------------------
def command(block_va, rgn_base, bg_off, box_x, box_y, completion_va):
    c = [0] * (0x140 // 4)
    fixed = {0x04: 0x300, 0x08: 0x3f800000, 0x0c: 2, 0x14: 0x100, 0x18: 0x8c000000, 0x1c: 0,
             0x20: 0x88, 0x38: 0x80}          # what the iOS kernel writes into every GL transfer
    for o, v in fixed.items():
        c[o // 4] = v
    c[0x00 // 4] = 0x02000000 | (bg_off >> 4)
    c[0x10 // 4] = rgn_base
    c[0x24 // 4: 0x30 // 4] = [block_va, 5, 0x4000]
    c[0x34 // 4] = box_x << 16 | box_y    # last region index, x in the high half
    c[0xa0 // 4] = 0x140; c[0xa4 // 4] = 0; c[0xac // 4] = completion_va; c[0xb0 // 4] = 1
    c[0x108 // 4] = 0x01800000
    return struct.pack('<%dI' % len(c), *c)

def build(outdir, src, dst, pbe, quad, box, block_va=0x980f3000, completion_va=0x80157000):
    import os
    open(os.path.join(outdir, 'g_code.bin'), 'wb').write(code_page(pbe))
    open(os.path.join(outdir, 'g_block.bin'), 'wb').write(block(src, dst) + bytes(0x10000 - 0x1c0))
    open(os.path.join(outdir, 'g_param.bin'), 'wb').write(param_page(block_va, *quad))
    open(os.path.join(outdir, 'g_cmd.bin'), 'wb').write(command(block_va, 0x6200, 0x6000, box[0], box[1], completion_va))

def blit(outdir, src, dst_addr, dst_stride_px, w, h, block_va=0x980f3000, completion_va=0x80157000):
    """copy/scale src onto a w x h rectangle at dst_addr of a linear BGRA surface"""
    import os
    r5 = (h - 1) << 12 | (w - 1)
    pbe = [0x00110000, dst_addr, dst_stride_px // 2 - 1, 0, 0, r5]
    dst = ('lin', dst_addr, w, h, dst_stride_px)
    open(os.path.join(outdir, 'g_code.bin'), 'wb').write(code_page(pbe))
    open(os.path.join(outdir, 'g_block.bin'), 'wb').write(block(src, dst) + bytes(0x10000 - 0x1c0))
    page = bytearray(param_page(block_va, 2, 2, 3, 3))
    # the background triangle covers the box, the quad the rectangle (pixel coordinates)
    struct.pack_into('<3I', page, 0x2c, xy_px(0, 0), 0x3f800000, xy_px(0, 2 * h))
    struct.pack_into('<I', page, 0x3c, xy_px(2 * w, 0))
    for o, (x, y) in zip((0x13c, 0x144, 0x14c, 0x154), ((0, 0), (0, h), (w, h), (w, 0))):
        struct.pack_into('<I', page, o, xy_px(x, y))
    open(os.path.join(outdir, 'g_param.bin'), 'wb').write(bytes(page))
    open(os.path.join(outdir, 'g_cmd.bin'), 'wb').write(
        command(block_va, 0x6200, 0x6000, (w - 1) // 32, (h - 1) // 32, completion_va))
