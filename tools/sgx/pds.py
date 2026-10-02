#!/usr/bin/env python3
"""pds.py -- the PDS programs an sgx2d frame runs, built by shape.

The PDS is the SGX's small data mover: a program is a data segment (16-byte
rows) followed by 32-bit instructions, and it ends by starting a USSE
program (DOUTU).  iOS's GL driver does not compile these: it writes a data
segment of a fixed shape and a fixed run of instruction words for each
kind (seen in its code as movw/movt constants with a field or two ORed in);
the builders below do the same.  What the instruction words mean, as far as
it is known (docs/research/p105-gpu.md, M8):

    bits 31:27 opcode, 26:24 predicate (7 = always)
    0x07......  DOUT -- hand data to the rest of the GPU; the low 12 bits
                say what (USSE start, DMA, iterate, texture, attribute) and
                bits 23:12 where the operands are in the data segment: row
                (word index >> 2) in 23:18, the second half of a row (DS1,
                word index bit 1) adding one to it, and 17:12
    0xaf000000  end of program

The 3D pass's event program is not built here: it is a template in the GL
driver itself, and ./cascadia firmware takes it out of the IPSW like the
microkernel (event_program()).
"""
import struct

END = 0xaf000000

# DOUTU: start a USSE program; the operand is the data row holding the
# DOUTU word (code base index | instruction index << 4) and the temporaries.
DOUTU_ONLY = 0x185          # the program's only DOUT
DOUTU_AFTER = 0x1a5         # after a DMA or attribute DOUTs
DOUTU_TEX = 0x1b5           # before the iterate and texture DOUTs that feed it
DOUTU_VERTEX = 0x1f5        # vertex programs (predicate 3)


def doutu(row, kind, pred=7):
    return pred << 24 | row << 18 | kind


DMA_ROW0 = 0x07018113       # DMA: address and control word from data row 0
ITERATE_W3 = 0x07040c12     # iterate the texture coordinates, control: word 3
ITERATE_BG = 0x07000c02     # the background object's iteration
TEXTURE_ROW1 = 0x07041004   # texture fetch, state: the four words of row 1
ATTR_ROW0 = 0x070181a6      # attribute DOUTs of the state header program
ATTR_ROW1 = 0x070581a6
FETCH_INDEX = 0x67800072    # vertex fetch: the index


def fetch_attr(n):
    """vertex fetch: attribute n, address and control word in row n"""
    return 0x2f0091a3 | (4 * n + 1) << 16


def rows(words):
    words = list(words)
    return words + [0] * (-len(words) % 4)


def program(data, code):
    """data segment (padded to rows) then code, as bytes; the data size in
    rows is what a PDS pointer's size word gives"""
    d = rows(data)
    return struct.pack('<%dI' % (len(d) + len(code)), *(d + code)), len(d) // 4


# --- the shapes -----------------------------------------------------------

def run_usse(doutu_word, temps, extra=0):
    """start a USSE program, nothing else (the VDM's tail)"""
    return program([doutu_word, temps, extra, 0], [doutu(0, DOUTU_ONLY), END])


def dma_then_usse(src, ctl, doutu_word, temps):
    """DMA words from SRC into the USSE's inputs (CTL: count - 1 and where),
    then start the USSE program -- state and constant loaders"""
    return program([src, ctl, 0, 0, doutu_word, temps],
                   [DMA_ROW0, doutu(1, DOUTU_AFTER), END])


def state_header(x, tiles, doutu_word, temps):
    """the frame's first state: the tile counts, then the USSE program that
    emits them"""
    return program([0x2000, 0, x, 0, tiles, 0x100, 0, 0, doutu_word, temps],
                   [ATTR_ROW0, ATTR_ROW1, ATTR_ROW1, ATTR_ROW1, doutu(2, DOUTU_AFTER), END])


def textured(doutu_word, temps, iterate, tex):
    """a pixel program's PDS: start the USSE program, iterate the texture
    coordinates (control word ITERATE), fetch the texture (four state words
    TEX: format and size, address)"""
    return program([doutu_word, temps, 0, iterate] + list(tex),
                   [doutu(0, DOUTU_TEX), ITERATE_W3, TEXTURE_ROW1, END])


def background(doutu_word, temps, iterate, tex):
    """the background object: every tile starts as what TEX (the
    framebuffer, linear) holds"""
    return program([doutu_word, temps, 0, iterate] + list(tex),
                   [doutu(0, DOUTU_ONLY), ITERATE_BG, TEXTURE_ROW1, END])


def vertex_fetch(attrs, stride, doutu_word):
    """fetch each vertex's attributes, ATTRS = [(address, control)], the
    first row also holding the vertex stride; then the vertex shader"""
    data = []
    for i, (addr, ctl) in enumerate(attrs):
        data += [addr, ctl, stride if i == 0 else 0, 0]
    data += [doutu_word, 0, 0, 0]
    code = [FETCH_INDEX] + [fetch_attr(i) for i in range(len(attrs))] + \
        [doutu(len(attrs), DOUTU_VERTEX, pred=3), END]
    return program(data, code)


EVENT_SHA256 = 'a1b855d41993968b40005e886f7ac11c8c3abb42aa0987e005eec4e6cb2a2285'


def event_program(path, eor, eot, done):
    """The 3D pass's pixel/event PDS program: iOS's GL driver keeps it as a
    template (build/firmware/gl-event.pds, from the IPSW), data segment of
    five rows then 22 instructions, and fills in the three USSE programs it
    picks from -- end of render, end of tile, and the rest -- and word 16."""
    import hashlib
    t = bytearray(open(path, 'rb').read())
    if hashlib.sha256(t).hexdigest() != EVENT_SHA256:
        raise SystemExit('%s is not the 12H321 GL driver\'s event program' % path)
    for i, v in ((0, eor), (2, eot), (4, done), (16, 0x30000)):
        struct.pack_into('<I', t, 4 * i, v)
    return bytes(t), 5


def nothing():
    """a PDS program that only ends (the TA's background object needs one)"""
    return program([], [END])


def _check(gl_path, gl_va):
    """rebuild the captured PDS programs from their data words: the oracle"""
    gl = open(gl_path, 'rb').read()
    at = lambda va, n: gl[va - gl_va:va - gl_va + n]
    w = lambda va: struct.unpack_from('<I', gl, va - gl_va)[0]
    FB = [w(0x98956130 + 4 * i) for i in range(4)]
    cases = {
        0x98956100: nothing(),
        0x98956120: background(w(0x98956120), w(0x98956124), w(0x9895612c), FB),
        0x98956160: textured(w(0x98956160), w(0x98956164), w(0x9895616c), [w(0x98956170 + 4 * i) for i in range(4)]),
        0x98956200: textured(w(0x98956200), w(0x98956204), w(0x9895620c), [w(0x98956210 + 4 * i) for i in range(4)]),
        0x989561c0: dma_then_usse(w(0x989561c0), w(0x989561c4), w(0x989561d0), w(0x989561d4)),
        0x989b5000: state_header(w(0x989b5008), w(0x989b5010), w(0x989b5020), w(0x989b5024)),
        0x989b5060: run_usse(w(0x989b5060), w(0x989b5064)),
        0x989b50a0: dma_then_usse(w(0x989b50a0), w(0x989b50a4), w(0x989b50b0), w(0x989b50b4)),
        0x989b5140: dma_then_usse(w(0x989b5140), w(0x989b5144), w(0x989b5150), w(0x989b5154)),
        0x989b5180: vertex_fetch([(w(0x989b5180), w(0x989b5184)), (w(0x989b5190), w(0x989b5194)),
                                  (w(0x989b51a0), w(0x989b51a4))], w(0x989b5188), w(0x989b51b0)),
        0x989b5380: dma_then_usse(w(0x989b5380), w(0x989b5384), w(0x989b5390), w(0x989b5394)),
        0x989b53e0: dma_then_usse(w(0x989b53e0), w(0x989b53e4), w(0x989b53f0), w(0x989b53f4)),
    }
    bad = 0
    for va, (b, nrows) in sorted(cases.items()):
        same = at(va, len(b)) == b
        bad += not same
        print('%08x %2d words, %d data rows  %s' % (va, len(b) // 4, nrows, 'same' if same else 'DIFFERS'))
        if not same:
            print('   ours', b.hex()); print('   capt', at(va, len(b)).hex())
    return bad


if __name__ == '__main__':
    import sys
    sys.exit(1 if _check(sys.argv[1], int(sys.argv[2], 0)) else 0)
