#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check the move-immediate encoding on the GPU: the compiler's code for
o[0] = 1 (16-bit form) and o[0] = 0x10000 (32-bit form), the value field
rewritten by our encoder, packed for tools/gpu/g9p-probe (build/gpu/
mov-check.bin). Run: g9p-probe mNAME file=/run/mov-check.bin, expect the
value in buf 0 word 0."""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, ".."))
import g9pisa  # noqa: E402
import g9pshaders  # noqa: E402

# value bit -> (word, bit), measured by imm-map.py
IMM = {0: (2, 1), 1: (2, 2), 2: (2, 3), 3: (2, 4), 4: (2, 5), 5: (2, 6), 6: (3, 1), 7: (3, 2),
       8: (3, 3), 9: (0, 13), 10: (3, 4), 11: (3, 12), 12: (3, 13), 13: (3, 14), 14: (3, 6),
       15: (3, 7), 24: (3, 15)}
IMM.update({16 + i: (4, i) for i in range(8)})
IMM.update({25 + i: (4, 9 + i) for i in range(7)})


def set_imm(ins, value):
    """The value into a move; W3 bit 0 set = the 32-bit form (W4 = bits 16-23,
    25-31), else W4 on is the instruction's tail and bits 16-31 must be 0."""
    ins = list(ins)
    wide = ins[3] & 1
    for vb, (w, b) in IMM.items():
        if vb >= 16 and not wide:
            if value >> vb & 1:
                raise ValueError("value needs the 32-bit form")
            continue
        ins[w] = ins[w] & ~(1 << b) | ((value >> vb & 1) << b)
    return ins


def code_of(insns):
    return b"".join(struct.pack("<%dH" % len(i), *i) for i in insns)


raw = g9pisa.corpus_raw({
    "m16": "kernel void m16(device uint *o [[buffer(0)]]) { o[0] = 1u; }",
    "m32": "kernel void m32(device uint *o [[buffer(0)]]) { o[0] = 0x10000u; }"})
out = {}
tests = {"m16": [0xbeef, 0x1234, 0x8001, 0x0200], "m32": [0xdeadbeef, 0x12345678, 0x80000001, 0x01010101]}
for name, values in tests.items():
    code = raw[name]
    out[name + "_orig"] = code
    # the move's offset in the code: walk the instructions
    off = 0
    for ins in g9pisa.split(code):
        if ins[0] & 0xff == 0x98:
            break
        off += 2 * len(ins)
    for v in values:
        new = set_imm(ins, v)
        out["%s_%08x" % (name, v)] = code[:off] + struct.pack("<%dH" % len(new), *new) + code[off + 2 * len(new):]
g9pshaders.pack(out, os.path.join(HERE, "../../../build/gpu/mov-check.bin"))
for n in out:
    print(n)
