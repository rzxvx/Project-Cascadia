#!/usr/bin/env python3
"""frame.py -- the template frame sgx2d builds on, without an iOS capture.

    frame.py OUTDIR [--check CAPDIR]

Writes the GL window (PDS programs, state, the index buffer), the VDM
stream and the render command (payload.txt, for rtemu.py) into OUTDIR, in
the form rgen.py reads, from
programs.py (USSE), pds.py (PDS) and build/firmware/gl-event.pds (the one
template that comes from the IPSW).  --check compares every object an sgx2d
frame reaches with a canonical capture (mkpack's build/sgx2d/capture).

The frame is the one iOS's GL driver set up for a full-screen textured
quad, texture x vertex colour, SRC_ALPHA / ONE_MINUS_SRC_ALPHA (gltrace
mod): the same objects at the same addresses, so the rest of the pack
(rgen.py, rpack.py, sgx2d) is unchanged.  Words whose meaning is not known
are named after where they are and kept at the values GL gives them.
"""
import os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import pds, programs

W, H = 768, 1024
GL_VA, GL_SIZE = 0x98956000, 0x6f000
PDS = GL_VA                       # the 3D pass's PDS block and the pixel programs
IDX = GL_VA + 0x11000             # the index buffer, 0..8191 (u16)
STATE = GL_VA + 0x5f000           # the TA's state and the vertex/state programs
TEX0, TEX1 = GL_VA + 0x16000, GL_VA + 0x18000   # where GL's two textures were
OUT_VA = GL_VA + 0x4f000          # GL's render target (payload w46)
FB_VA = 0x90000000

# the framebuffer, as the background object reads it back: linear, the 2D
# engine's format (rpack.FB_LOAD)
FB_LOAD = ((W // 4 - 2) << 16 | 0x0e90, 0xcc000000 | (W - 1) << 12 | (H - 1), FB_VA, 0x10000000)


def doutu(name, base=3):
    """the DOUTU word for a program on the code page: instruction index << 4
    | code base"""
    return programs.doutu_index(programs.LAYOUT[name]) << 4 | base


def tag5(va):
    """a PDS data pointer: (address >> 4) & 0x07ffffff under tag 1 (bit 31
    implied); sgx2d's p27()"""
    return 0x10000000 | (va >> 4) & 0x07ffffff


def f32(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]


def tiles():
    """last 32-pixel tile, x << 16 | y"""
    return ((W + 31) // 32 - 1) << 16 | ((H + 31) // 32 - 1)


# the constants the state programs DMA into the secondary attributes:
# 2/255, 1/255, 2/65535, 1/65535 -- normalisation of 8- and 16-bit values
NORM = [f32(2 / 255), f32(1 / 255), f32(2 / 65535), f32(1 / 65535)]

# the texture state of a pixel program (sgx2d replaces size and address):
# iterate control, then {format, 0x0c << 24 | log2 w << 16 | log2 h, address, 0}
TEX_ITERATE = 0x1fc01900
TEX_FORMAT = 0x03fe0000
BG_ITERATE = 0xf800
TEMPS_TEXTURED, TEMPS_BG = 0xe, 0xa


def full_state(texblock):
    """draw 0's whole state, 21 words, as the state_21 program emits them"""
    tx, ty = tiles() >> 16, tiles() & 0xffff
    return [
        0x0000dfc7,                 # ISP state A
        0x03d00300,                 # ISP state B: compare ALWAYS (24:22), no depth
                                    # write (20), blending (25)
        0x0000f000, 0x0e000000,     # (GL's)
        tag5(PDS + 0x1c0),          # the pixel program's secondary loader
        0x0803e000,                 # pixel PDS info
        tag5(texblock),             # the pixel program: texture block
        0x80000000 | tx, ty,        # tile clip: last tile
        f32(W / 2), f32(W / 2), f32(H / 2), f32(H / 2), f32(0.5), f32(0.5),   # viewport
        0,
        0x0a001000, f32(1e-5), 0x00088000, 0x00000039, 0x00000003,   # (GL's)
    ]


def delta_state(texblock):
    """a later draw's state: mask 0x40 (the pixel program only), then the
    same three words as full_state's 4..6"""
    return [0x40, tag5(PDS + 0x1c0), 0x0803e000, tag5(texblock)]


def gl_window(event_path):
    g = bytearray(GL_SIZE)

    def put(va, b):
        if isinstance(b, tuple):
            b = b[0]
        if isinstance(b, list):
            b = struct.pack('<%dI' % len(b), *b)
        assert g[va - GL_VA:va - GL_VA + len(b)] == bytes(len(b)), '%x overlaps' % va
        g[va - GL_VA:va - GL_VA + len(b)] = b

    # the 3D pass: event program (+0), background object (+0x100, +0x120),
    # the pixel programs (+0x160, +0x200) and their secondary loader (+0x1c0)
    put(PDS, pds.event_program(event_path, doutu('eor'), doutu('eot'), doutu('done')))
    put(PDS + 0x100, pds.nothing())
    put(PDS + 0x120, pds.background(doutu('bg_reload'), TEMPS_BG, BG_ITERATE, FB_LOAD))
    put(PDS + 0x160, pds.textured(doutu('pixel_blend'), TEMPS_TEXTURED, TEX_ITERATE,
                                  [TEX_FORMAT, 0x0c030003, TEX0, 0]))
    put(PDS + 0x1c0, pds.dma_then_usse(PDS + 0x1a4, 4, doutu('empty_c'), 2))
    put(PDS + 0x200, pds.textured(doutu('pixel_blend'), TEMPS_TEXTURED, TEX_ITERATE,
                                  [TEX_FORMAT, 0x0c020002, TEX1, 0]))
    put(IDX, struct.pack('<8192H', *range(8192)))

    # the TA side
    put(STATE, pds.state_header(0, tiles(), doutu('state_2'), 0))
    put(STATE + 0x60, pds.run_usse(doutu('empty_a'), 2))
    put(STATE + 0x78, NORM)
    put(STATE + 0xa0, pds.dma_then_usse(STATE + 0x78, 6, doutu('empty_b'), 2))
    put(STATE + 0xe0, full_state(PDS + 0x160))
    put(STATE + 0x140, pds.dma_then_usse(STATE + 0xe0, 20, doutu('state_21'), 0))
    # vertex fetch: [rgba f32 x4][uv f32 x2][xy f32 x2], 32 bytes; control =
    # first primary attribute << 8 | words - 1 (sgx2d points the addresses
    # at its own vertices)
    vb = STATE + 0x1e0
    put(STATE + 0x180, pds.vertex_fetch([(vb, 0x003), (vb + 0x10, 0x401), (vb + 0x18, 0x801)],
                                        32, doutu('vertex')))
    put(STATE + 0x360, NORM)
    put(STATE + 0x380, pds.dma_then_usse(STATE + 0x360, 6, doutu('empty_d'), 2))
    put(STATE + 0x3c0, delta_state(PDS + 0x200))
    put(STATE + 0x3e0, pds.dma_then_usse(STATE + 0x3c0, 3, doutu('state_4'), 0))
    return bytes(g)


def vdm4(tag, va):
    return tag << 28 | (va >> 4) & 0x0fffffff


def vdm_stream():
    """The VDM control stream of the template frame, in sgx2d's shape (which
    builds its own each frame; rpack takes the tail and the fetch word from
    this one): per draw the constants loader, the state program, the draw
    (6 indices), the vertex fetch; then the state header and its tail."""
    draws = ((STATE + 0xa0, STATE + 0x140, 0x12022206),     # draw 0: whole state
             (STATE + 0x380, STATE + 0x3e0, 0x12022201))    # later draws: delta
    s = []
    for consts, prog, info in draws:
        s += [vdm4(4, consts), 0x1000e102, vdm4(4, prog), info,
              0x81c00006, IDX, 0x70000000, 0x003fffff,
              vdm4(9, STATE + 0x180), 0x07800604]
    s += [vdm4(4, STATE + 0x60), 0x0800e100, vdm4(6, STATE), 0x1a022201, 0xc0000000]
    return s


def payload():
    """the render command's 55 words (the GL part; rtemu.py adds the
    kernel's).  docs/research/p105-gpu.md, M4."""
    w = [0] * 55
    w[0], w[1] = 1, 0xe                 # (GL's: command type, resource records)
    w[2] = 0x90012000                   # VDM stream (rgen moves it to its own)
    w[3] = 1                            # TA commands
    w[12] = w[13] = PDS                 # the 3D pass's pixel and event PDS program
    w[14] = w[15] = 5                   # its data segment, in rows
    w[16] = w[17] = 0x4000              # (its info word)
    w[18] = (PDS + 0x100 - 0x80000000) >> 4      # background object: PDS program
    w[19] = 0x0801e000                  # its info word
    w[20] = tag5(PDS + 0x120)           # its pixel program
    w[23], w[24] = 0x200, 3             # (GL's)
    w[36] = w[37] = 0x88                # (GL's; w37 -> TA register 0x250)
    w[44], w[45] = W, H                 # the render's size
    w[46] = OUT_VA                      # GL's render target
    w[47], w[48], w[49], w[50] = 0x100, 0x4000, 0x1f, 9   # (GL's)
    return w


# rgen.py's 'modblend' profile reads the frame as GL buffers named by the
# CPU addresses they had in the capture it was first built from
REGIONS = {'r_008a2000.bin': (PDS, 0x10000), 'r_008b4000.bin': (IDX, 0x4000),
           'r_00931000.bin': (STATE, 0x10000)}
VDM_REGION = 'r_008f3000.bin'


def write(out, event_path):
    """the frame as rgen.py takes it: regions and payload.txt"""
    os.makedirs(out, exist_ok=True)
    g = gl_window(event_path)
    for f, (va, n) in REGIONS.items():
        open(os.path.join(out, f), 'wb').write(g[va - GL_VA:va - GL_VA + n])
    v = vdm_stream()
    open(os.path.join(out, VDM_REGION), 'wb').write(struct.pack('<%dI' % len(v), *v).ljust(0x4000, b'\0'))
    open(os.path.join(out, 'payload.txt'), 'w').write(' '.join('%08x' % x for x in payload()) + '\n')


def main():
    out = sys.argv[1]
    ev = os.path.join(HERE, '..', '..', 'build', 'firmware', 'gl-event.pds')
    write(out, ev)
    if '--check' in sys.argv:
        check(sys.argv[sys.argv.index("--check") + 1])


def check(cap):
    """every object an sgx2d frame reaches (capmap.py), against the capture,
    with W, H as the capture had them (64 x 64)"""
    global W, H
    W0, H0 = W, H
    W, H = 64, 64
    g = gl_window(os.path.join(HERE, '..', '..', 'build', 'firmware', 'gl-event.pds'))
    p = payload()
    W, H = W0, H0
    regs = {}
    for f in os.listdir(cap):
        if f.startswith('r_'):
            regs[f] = open(os.path.join(cap, f), 'rb').read()
    # capture regions -> GL addresses (rgen's modblend profile)
    where = {'r_008a2000.bin': PDS, 'r_008b4000.bin': IDX, 'r_00931000.bin': STATE}
    objs = [(PDS, 0xb0), (PDS + 0x100, 4), (PDS + 0x120, 0x30), (PDS + 0x160, 0x30),
            (PDS + 0x1a0, 0x20), (PDS + 0x1c0, 0x2c), (PDS + 0x200, 0x30), (IDX, 0x4000),
            (STATE, 0x48), (STATE + 0x60, 0x40), (STATE + 0xa0, 0x2c), (STATE + 0xe0, 0x54),
            (STATE + 0x140, 0x2c), (STATE + 0x180, 0x58), (STATE + 0x360, 0x10),
            (STATE + 0x380, 0x2c), (STATE + 0x3c0, 0x10), (STATE + 0x3e0, 0x2c)]
    bad = 0
    for va, n in objs:
        for f, base in where.items():
            if base <= va < base + len(regs[f]):
                c = regs[f][va - base:va - base + n]
        ours = g[va - GL_VA:va - GL_VA + n]
        if va == PDS + 0x120:       # our framebuffer descriptor, not GL's surface
            c = c[:0x10] + struct.pack('<4I', *FB_LOAD) + c[0x20:]
        same = ours == c
        bad += not same
        print('%08x +%-5x %s' % (va, n, 'same' if same else 'DIFFERS'))
        if not same:
            for i in range(0, n, 4):
                a, b = ours[i:i + 4], c[i:i + 4]
                if a != b:
                    print('    +%03x ours %08x  capture %08x' % (i, struct.unpack('<I', a)[0], struct.unpack('<I', b)[0]))
    cp = [int(x, 16) for x in open(os.path.join(cap, 'payload.txt')).read().split()]
    for i, (a, b) in enumerate(zip(p, cp)):
        if a != b:
            bad += 1
            print('payload w%d ours %08x capture %08x' % (i, a, b))
    print('payload', 'same' if p == cp else 'DIFFERS')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
