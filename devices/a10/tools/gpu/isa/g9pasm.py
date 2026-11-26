# SPDX-License-Identifier: MIT
"""A minimal G9P (applegpu_g9p) assembler: the compute program frame around
a body of instructions (lists of 16-bit words), and encoders for what is
decoded so far (devices/a10/docs/gpu.md "G8.1"). Every encoder here is
checked on the GPU by isa-check.py.

    program(body, nregs, nbufs) -> code bytes (end word at a 64-byte block)
"""
import struct

END, PAD = 0x8000, 0xc000


def _jump(op_w0, offset):
    """`0900 W1 W2` (to the epilogue) / `0100 W1 W2` (to the end word): the
    target is `offset` bytes from the instruction's own start (jump())"""
    return jump(op_w0, offset)


def program(body, nregs, nbufs=2, count_half=None):
    """the frame of the compiler's compute kernels: prologue, register count,
    the jump to the epilogue for inactive threads (to check), body,
    epilogue, the jump to the end word"""
    # the lane test (r0, the local ID, against the threads per group in the
    # common store's half count_half), the mask stack's start in r[nregs]
    ch = count_half if count_half is not None else {1: 4, 2: 8}[nbufs]
    pro = [[0x1181, 0x8181 | 2 * ch, 0x4100, 0xacb7], push_init(nregs)]
    epi = [0x21c8, 0x0300 | 2 * nregs, 0x0180 | 4 * nregs, 0x46d2]
    nbody = sum(len(i) for i in body)
    words = [w for i in pro for w in i]
    words += _jump(0x0900, 2 * (3 + nbody))
    words += [w for i in body for w in i]
    words += epi
    jpos = 2 * len(words)
    end = (jpos + 6 + 63) // 64 * 64
    words += _jump(0x0100, end - jpos)
    words += [PAD] * ((end - 2 * len(words)) // 2) + [END]
    return struct.pack("<%dH" % len(words), *words)


# ---- encoders ---------------------------------------------------------------

def load(reg, slot, index, count=1):
    """device load of `count` (1-4) u32 from buffer slot `slot`, element `index`"""
    sz = {1: 4, 2: 5, 3: 6, 4: 7}[count]
    return _mem(sz, reg, slot, index, store=False)


def store(reg, slot, index, count=1):
    sz = {1: 4, 2: 5, 3: 6, 4: 7}[count]
    return _mem(sz, reg, slot, index, store=True)


def _mem(sz, reg, slot, index, store):
    w0 = 0x0312 | sz << 11
    w1 = 0x0100 | slot << 10 | (0x80 if store else 0) | reg << 1
    if index < 0x100:
        return [w0, w1, 0x8000 | index]
    return [w0, w1, 0x8100 | (index & 0xff), index >> 8]


def wait():
    return [0x004d]


# ALU operations: the last word's low byte, of opcode 0x99 (two sources) or
# 0x9a (three); bit 14 of the last word negates the second source (float)
OPS2 = {"iadd": 0xa1, "isub": 0xa5, "imul": 0xa2, "and": 0xc8, "or": 0xce, "xor": 0xc6,
        "fadd": 0x44, "fmul": 0x48, "fmul_sat": 0x4a}
OPS3 = {"ffma": 0x4c}
NEG_B = 0x4000
# compare conditions (cmp_reg / cmp_imm / sel)
COND = {"ult": 0xb6, "ugt": 0xb7, "eq": 0xbd, "slt": 0xbe, "sgt": 0xbf,
        "flt": 0x59, "fgt": 0x5d, "feq": 0x55}


def _srcs(a, b):
    """the source word: first source in bits 2-6, second in bits 10-14 (bits
    7 / 15: last-use flags, left clear), bit 8 = another word follows"""
    assert 0 <= a < 32 and 0 <= b < 32
    return 0x0100 | a << 2 | b << 10


# W0 bits 12 / 13: the second / first source from register bank 1 (the
# compute bank; W1 bit 9 the destination's); our programs use bank 1 only
BANKS = 0x3000


def dst(d):
    return 0x0300 | d << 1


def alu2(op, d, a, b, neg_b=False):
    """d = a OP b (isub: a - b)"""
    return [BANKS | 0x0199, dst(d), _srcs(a, b), OPS2[op] | (NEG_B if neg_b else 0)]


def alu3(op, d, a, b, c, neg_b=False):
    """d = a * b + c (ffma); c in the third word's bits 2-6"""
    return [BANKS | 0x0800 | 0x019a, dst(d), _srcs(a, b), 0x0100 | c << 2, OPS3[op] | (NEG_B if neg_b else 0)]


def cmp_reg(cond, a, b):
    """compare a with b for a following push / break / select"""
    return [BANKS | 0x0181, _srcs(a, b), (0x6000 if cond.startswith("f") else 0xa000) | COND[cond]]


def cmp_imm(cond, r, imm):
    """compare r with an immediate (< 1024)"""
    assert 0 <= imm < 1024 and r < 32
    return [0x1181 | ((imm >> 9) & 1) << 13, 0x0101 | (imm & 0x3f) << 1 | r << 10,
            0x0120 | ((imm >> 6) & 7) << 1, 0xa100 | COND[cond], 0x0070]


def sel(cond, d, a, b, c):
    """d = c in the lanes where (a COND b); movf() sets the others"""
    return [BANKS | 0x099e, dst(d), _srcs(a, b), 0x0180 | c << 2, 0x6000 | COND[cond]]


def movf(d, other):
    return [0x21f8, dst(d), 0x0180 | other << 2, 0x0154, 0x0038]


# --- the execution-mask stack, in register st (the one after the program's) -

def st_src(st):
    return 0x0100 | st << 2


def push_init(st):
    return [0x01c8, dst(st), 0x0101, 0x0120, 0x46d1]


def push_if(st, invert=False):
    """disable the lanes whose compare failed (invert: passed)"""
    return [0x21c8, dst(st), st_src(st), 0x46d1 | (0x2000 if invert else 0)]


def else_(st):
    return [0x21c8, dst(st), st_src(st), 0x46d4]


def pop(st, levels=1, last_read=True):
    return [0x21c8, dst(st), (0x0080 if last_read else 0) | st_src(st), 0x0600 | 0xd2 | levels << 14]


def loop_begin(st):
    return [0x21c8, dst(st), st_src(st), 0x86d0]


def loop_break(st, when_true):
    return [0x21c8, dst(st), st_src(st), 0x86d7 | (0x2000 if when_true else 0)]


def brk(st, n):
    """set the active lanes' stack level to n (an immediate, 0x98-style):
    break (n = enclosing ifs + 2) / continue (+ 1) inside a loop"""
    assert 0 < n < 64
    return [0x0188, dst(st), 0x0101 | n << 1, 0x0120, 0x86d6]


JMP_ANY, JMP_NONE = 0x0101, 0x0900


def jump(op, offset):
    """a jump `offset` bytes from its own start: two words of 16 offset bits,
    offset bit 8 (and 24) moved to bit 0 to keep bit 8 for the continuation"""
    o = offset & 0xffffffff
    return [op, (o & 0xfefe) | 0x0100 | (o >> 8 & 1), (o >> 16 & 0xfefe) | (o >> 24 & 1)]


# --- addresses: buffer base + register --------------------------------------

def addr(d, base_dword, idx, shift):
    """r[d]:r[d+1] = (the 64-bit common-store dwords base_dword, +1) + (r[idx] << shift)"""
    return [0x1189, dst(d), (1 | idx << 2) << 8 | 0x81 | 4 * (base_dword & 31),
            0x4100 | (base_dword >> 5) << 1, 0x01a1, 0x0600 | shift << 13]


def mem_at(store, size, reg, pair):
    """load / store `size` (SIZE) at the address r[pair]:r[pair+1]"""
    return [size << 11 | 0x0310, (1 | pair << 2) << 8 | (0x80 if store else 0) | reg << 1, 0x8800]


def tg_load(d, word, bank1=False):
    """r[d] = threadgroup-memory word `word` (words 0-2: the workgroup ID,
    written by the kernel's PDS program); follow with wait()"""
    return [0x2314, (0x0300 if bank1 else 0x0100) | d << 1, 0x8100 | (word & 0xff), 0x0200 | word >> 8]


# float unary operations (219c dd 0x01ss op) and conversions
FUNARY = {"floor": 0x40, "rcp": 0x80, "rsqrt": 0x81, "log2": 0x84, "exp2": 0x85}
CONV = {"f2u": 0x10, "f2i": 0x12, "u2f": 0x14, "i2f": 0x16}


def funary(op, d, a):
    return [0x219c, dst(d), 0x0100 | a << 2, FUNARY[op]]


def convert(op, d, a):
    c = CONV[op]
    return [0x1199, dst(d), 0x0101 | c | a << 10, 0x0120, 0x0468 if c & 4 else 0x0460]


def shift(d, a, b, right):
    """d = a << b / a >> b (logical), b a register"""
    return [0x199e, dst(d), 0x0101 | a << 10, 0x0120, 0x0100 | b << 2, 0x04f4 if right else 0x04f0]


def shift_imm(d, a, n, right):
    """d = a << n / a >> n (logical), n < 32"""
    return [0x519e if right else 0x119a, (0 if right else 0x8000) | dst(d), 0x0101 | a << 10, 0x0120,
            0x0101 | (n & 31) << 1, 0x0120, 0x14f4 if right else 0x14f0]


SIZE = {"u8": 1, "u16": 2, "u32": 4, "2x32": 5, "3x32": 6, "4x32": 7}

# the move immediate (opcode 0x98): value bit -> (word, bit)
_IMM = {0: (2, 1), 1: (2, 2), 2: (2, 3), 3: (2, 4), 4: (2, 5), 5: (2, 6), 6: (3, 1), 7: (3, 2),
        8: (3, 3), 9: (0, 13), 10: (3, 4), 11: (3, 12), 12: (3, 13), 13: (3, 14), 14: (3, 6),
        15: (3, 7), 24: (3, 15)}
_IMM.update({16 + i: (4, i) for i in range(8)})
_IMM.update({25 + i: (4, 9 + i) for i in range(7)})


def mov_imm(d, value):
    """d = value (u32), the compiler's form `0198 03dd W2 W3 [W4] 0154 0038`"""
    wide = value >> 16 != 0
    w = [0x0198, 0x0300 | d << 1, 0x0101, 0x0120 | (1 if wide else 0)] + ([0x0100] if wide else [])
    for vb, (i, b) in _IMM.items():
        if value >> vb & 1:
            w[i] |= 1 << b
    return w + [0x0154, 0x0038]


# --- vertex and fragment programs ------------------------------------------

def vs_output(reg, slot):
    """vertex output `slot` (0-3 the clip-space position, then the
    varyings' components) = r[reg]"""
    return [0x151e, 0x0100 | 2 * reg, slot]


def vertex_program(body, last_reg, last_slot):
    """a vertex shader: body, then the last output write in the end word's
    place at the start of a 64-byte block (after a jump and padding)"""
    words = [w for i in body for w in i]
    jpos = 2 * len(words)
    end = (jpos + 6 + 63) // 64 * 64
    words += _jump(0x0100, end - jpos)
    words += [PAD] * ((end - 2 * len(words)) // 2)
    words += [0x155e, 0xc100 | 2 * last_reg, last_slot]
    words += [0] * (-len(words) % 32)
    return struct.pack("<%dH" % len(words), *words)


def fragment_program(body):
    """a fragment shader: body, padding, the end word at a 64-byte block"""
    words = [w for i in body for w in i]
    words += [PAD] * ((-2 * (len(words) + 1)) % 64 // 2) + [END]
    return struct.pack("<%dH" % len(words), *words)


def fs_dst(d):
    """a fragment-bank destination (W1 bit 9 clear)"""
    return 0x0100 | d << 1


def fs_mov_imm(d, value):
    w = mov_imm(d, value)
    w[1] = fs_dst(d)
    return w


def iterate(first_half, comp, n, f16=True, persp=True):
    """the fragment's iterated varying: n components (1-4) from component
    index comp on, into registers from half first_half (f16) or from
    register first_half / 2 (f32)"""
    w0 = (0x000a if f16 else 0x0008) | (n % 4) << 11 | (0x0200 if persp else 0) | 0x0100
    return [w0, (1 + comp) << 9 | 0x0100 | first_half, 0x0080]


def emit_colour(first_half=0):
    """the colour output: four f16 halves from first_half on"""
    return [0x211c, 0x0800 | first_half]


def iterate_f32_coord(first_reg, comp, n=2):
    """f32 varying components for texture coordinates into r[first_reg]..
    (the compiler's form with W2 0x8000)"""
    return [0x0108 | (n % 4) << 11 | 0x0200, (1 + comp) << 9 | 0x0100 | 2 * first_reg, 0x8000]


def sample(first_half, coord_reg):
    """sample the 2D texture whose state the PDS put at common-store dword 0
    (sampler at dword 8) at the f32 coordinates r[coord_reg], r[coord_reg+1]
    into four f16 halves from first_half (the compiler's form; first_half /
    coord_reg 0 only, other fields to decode); follow with wait()"""
    assert first_half == 0 and coord_reg == 0
    return [0xf518, 0x0100, 0x8108, 0x0700, 0x0200]


def load_tile(first_half):
    """the tile's current colour (four f16 halves from first_half): the colour
    emit's form with W1 bit 13 -- what blending reads"""
    return [0x211c, 0x2800 | first_half]


OPS16 = {"hadd": 0x04, "hmul": 0x08}


def alu2h(op, d, a, b):
    """fragment-bank f16 two-source ALU (opcode 0x89): half d = half a OP half b"""
    return [0x0189, 0x0100 | d, 0x0100 | 0x80 | a << 1 | (0x40 | b) << 9, 0x0100 | OPS16[op], 0x8000]
