#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Register banks: a value written with the destination's W1 bit 9 clear
(bank 0) and read back with the source's W0 bank bit clear, against reading
it from bank 1. build/gpu/isa-check7.{bin,txt}."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, ".."))
import g9pasm as A  # noqa: E402
import g9pshaders  # noqa: E402


def mov_bank0(d, v):
    w = A.mov_imm(d, v)
    w[1] &= ~0x0200
    return w


def or_banks(d, a, b, a_bank1, b_bank1):
    """d = a | b, the first source from bank 1 if a_bank1 (W0 bit 13), the second if b_bank1 (bit 12)"""
    w = A.alu2("or", d, a, b)
    w[0] = (w[0] & ~0x3000) | (0x2000 if a_bank1 else 0) | (0x1000 if b_bank1 else 0)
    return w


tests = {
    # r4 = 0x5 in bank 1, r4 = 0xa0 in bank 0: the same register number, two values
    "b1_b1": ([A.mov_imm(4, 0x5), mov_bank0(4, 0xa0), A.mov_imm(3, 0), or_banks(5, 3, 4, True, True)], 0x5),
    "b1_b0": ([A.mov_imm(4, 0x5), mov_bank0(4, 0xa0), A.mov_imm(3, 0), or_banks(5, 3, 4, True, False)], 0xa0),
    "b0_b1": ([A.mov_imm(4, 0x5), mov_bank0(4, 0xa0), A.mov_imm(3, 0), or_banks(5, 4, 3, False, True)], 0xa0),
}
out, lines = {}, []
for name, (body, want) in tests.items():
    out[name] = A.program(body + [A.store(5, 0, 0)], nregs=8)
    lines.append("%s %08x" % (name, want))
g9pshaders.pack(out, os.path.join(HERE, "../../../build/gpu/isa-check7.bin"))
open(os.path.join(HERE, "../../../build/gpu/isa-check7.txt"), "w").write("\n".join(lines) + "\n")
print("temps=0x20 k0=0x4103")
