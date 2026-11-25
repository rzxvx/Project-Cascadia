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
    """`0900 01oo 0000` (to the epilogue) / `0100 01pp 0000` (to the end word):
    the target is `offset` bytes from the instruction's own start"""
    assert 0 <= offset < 0x100, "longer jumps: to decode"
    return [op_w0, 0x0100 | offset, 0x0000]


def program(body, nregs, nbufs=2):
    """the frame of the compiler's compute kernels: prologue, register count,
    the jump to the epilogue for inactive threads (to check), body,
    epilogue, the jump to the end word"""
    pro = [[0x1181, {1: 0x8189, 2: 0x8191}[nbufs], 0x4100, 0xacb7],
           [0x01c8, 0x0300 | 2 * nregs, 0x0101, 0x0120, 0x46d1]]
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
# 0x9a (three); bit 14 of the last word negates source A (float)
OPS2 = {"iadd": 0xa1, "isub": 0xa5, "imul": 0xa2, "and": 0xc8, "or": 0xce, "xor": 0xc6,
        "fadd": 0x44, "fmul": 0x48, "fmul_sat": 0x4a}
OPS3 = {"ffma": 0x4c}
NEG_A = 0x4000


def _srcs(a, b):
    """W2: source A in bits 10-14 (bit 15: a flag, set like the compiler),
    source B in bits 2-6 (bit 7: a flag); bit 8 = another word follows"""
    assert 0 <= a < 32 and 0 <= b < 32
    return ((0x40 | a << 1) << 9) | 0x100 | 0x80 | b << 2


def alu2(op, d, a, b, neg_a=False):
    """d = a OP b (isub: b - a). W0 bits 12-13 set: both sources from the
    register file (clear: garbage -- an operand cache, to check)"""
    return [0x3199, 0x0300 | d << 1, _srcs(a, b), OPS2[op] | (NEG_A if neg_a else 0)]


def alu3(op, d, a, b, c, neg_a=False):
    """d = a * b + c (ffma); source C in W3 bits 2-6"""
    return [0x399a, 0x0300 | d << 1, _srcs(a, b), 0x0180 | c << 2, OPS3[op] | (NEG_A if neg_a else 0)]


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
