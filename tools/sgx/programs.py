#!/usr/bin/env python3
"""programs.py -- the USSE programs an sgx2d frame runs, as our own source.

    programs.py CODEPAGE.bin [--check CAPTURED.bin]

Writes the code page (4 KiB, at code base 3 + 0x1000) with every program at
its place in LAYOUT; --check compares each program with the same place in a
captured page (iOS's GL driver's, tools/sgx/capmap.py says which are used)
-- the oracle for the encodings.

What runs, and who starts it (each through a PDS program's DOUTU):

  vertex     the vertex shader: position and attributes to the tiler
  pixel_*    the fragment shader, one per blend mode: texture x vertex
             colour, blended with the tile
  bg_reload  the background object: each tile starts as the framebuffer
  eot        end of tile: the PBE writes the tile to the framebuffer
  eor, done  the 3D pass's two events
  state_N    VDM state programs: N state words, loaded by the PDS, to the
             tiler
  empty      programs with nothing to do, where the PDS needs one
"""
import os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import usse as u

FB_VA, FB_STRIDE_PX, W, H = 0x90000000, 768, 768, 1024
F1 = 0x3f800000                      # 1.0f

# where each program goes on the page (offsets in the page; the page is at
# code base + 0x1000, so a DOUTU names (0x1000 + offset) / 8).  For now the
# places iOS's capture had them, so the captured PDS blocks find them.
LAYOUT = {
    'bg_reload': 0x400, 'empty_a': 0x540, 'state_2': 0x600, 'state_4': 0x680,
    'state_21': 0xac0, 'done': 0xc00, 'eor': 0xc40, 'eot': 0xd40,
    'vertex': 0xdc0, 'empty_b': 0xe40, 'pixel_blend': 0xe80, 'empty_c': 0xec0,
    'empty_d': 0xf00, 'pixel_add': 0xf40, 'pixel_mod': 0xf80,
}


def doutu_index(off):
    return (0x1000 + off) // 8


def state(n):
    """The PDS has loaded n state words into pa0..; out through o0.. to the
    tiler.  (A repeat covers at most 16 registers.)"""
    p = [u.phas()] + u.dummy_load()
    for k in range(0, n, 16):
        p.append(u.mov_words(('o', k), ('pa', k), min(16, n - k)))
    return p + [u.EMIT_STATE]


def vertex(off):
    """Two phases.  The PDS has fetched the vertex -- rgba, uv, xy, 32 bytes
    -- into the primary attributes.  Phase 1 completes the position (z = 0,
    w = 1.0, in pa10/pa11); phase 2 moves position and attributes to the
    outputs, the second VMOV with its own register increments, and emits."""
    second = off + 0x30
    p1 = [u.phas(next_phase=doutu_index(second))] + u.dummy_load() + [
        u.limm(('pa', 11), F1),
        u.limm(('pa', 10), 0, end=True)]
    p2 = [u.phas(),
          u.smlsi(1, 1, 0xe, 1, src1_inc_mode=1),
          u.vmov_f32(('o', 0), ('pa', 2), repeat=3),
          u.smlsi(1, 1, 1, 1),
          u.vmov_f32(('o', 3), ('pa', 0), repeat=2),
          u.EMIT_VERTEX]
    assert len(p1) * 8 == 0x30
    return p1 + p2


def eot():
    """The PBE state for the tile: straight into the linear framebuffer
    (the 2D engine's format: 0x00110000, address, stride / 2 - 1, size)."""
    regs = (0x00110000, FB_VA, FB_STRIDE_PX // 2 - 1, 0, 0, (H - 1) << 12 | (W - 1))
    return [u.phas()] + u.dummy_load() + [u.limm(('r', i), v) for i, v in enumerate(regs)] + \
        [u.EMIT_PIXEL]


def pixel(mode):
    """colour = texture x vertex colour, blended with the tile's colour"""
    blend = {
        'blend': dict(csel1=u.SEL_SRC_ALPHA_C, csel2=u.SEL_SRC_ALPHA_C, cmod2=1,   # SRC_ALPHA,
                      asel1=u.SEL_SRC_ALPHA, asel2=u.SEL_SRC_ALPHA, amod2=1),      # 1-SRC_ALPHA
        'add': dict(csel1=u.SEL_SRC_ALPHA_C, csel2=u.SEL_ZERO, cmod2=1,            # SRC_ALPHA, ONE
                    asel1=u.SEL_SRC_ALPHA, asel2=u.SEL_ZERO, amod2=1),
        'mod': dict(csel1=u.SEL_DST, csel2=u.SEL_ZERO, cmod2=0,                    # DST_COLOR, ZERO
                    asel1=u.SEL_DST, asel2=u.SEL_ZERO, amod2=0),
    }[mode]
    return [u.phas(), u.sop2m_modulate(), u.sop2_blend(**blend)]


def programs():
    L = LAYOUT
    return {
        'bg_reload': [u.phas(), u.mov_words(('o', 0), ('pa', 0), end=True)],
        'state_2': state(2), 'state_4': state(4), 'state_21': state(21),
        'done': [u.phas(), u.EMIT_DONE],
        'eor': [u.phas(), u.limm(('r', 0), 0), u.limm(('r', 1), 0), u.EMIT_EVENT],
        'eot': eot(),
        'vertex': vertex(L['vertex']),
        'pixel_blend': pixel('blend'), 'pixel_add': pixel('add'), 'pixel_mod': pixel('mod'),
        **{k: [u.phas(), u.nop(end=True)] for k in L if k.startswith('empty')},
    }


def code_page():
    page = bytearray(0x1000)
    for name, prog in programs().items():
        off = LAYOUT[name]
        b = u.words(prog)
        assert page[off:off + len(b)] == bytes(len(b)), name + ' overlaps'
        page[off:off + len(b)] = b
    return bytes(page)


def main():
    page = code_page()
    open(sys.argv[1], 'wb').write(page)
    if '--check' in sys.argv:
        cap = open(sys.argv[sys.argv.index('--check') + 1], 'rb').read()
        bad = 0
        for name, prog in programs().items():
            off, n = LAYOUT[name], len(prog) * 8
            same = page[off:off + n] == cap[off:off + n]
            bad += not same
            print('%-12s +%03x %2d instrs  %s' % (name, off, len(prog), 'same' if same else 'DIFFERS'))
            if not same:
                for k in range(0, n, 8):
                    a, b = struct.unpack_from('<Q', page, off + k)[0], struct.unpack_from('<Q', cap, off + k)[0]
                    if a != b:
                        print('    +%03x ours %016x  captured %016x' % (off + k, a, b))
        sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
