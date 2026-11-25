#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Register fields above r3, and the 0x9d form: in[0..3] loaded to r(base)..,
xor of two of them -> r(dest) -> out[0]. Writes build/gpu/isa-check2.{bin,txt}
(name, expected)."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, ".."))
import g9pasm  # noqa: E402
import g9pshaders  # noqa: E402

IN = [0x1, 0x10, 0x100, 0x1000]
NREGS = 32


def alu2(op, a, b, d, opc=0x99, aflag=0x40, bflag=0x80):
    w2 = ((aflag | a << 1) << 9) | 0x100 | bflag | b << 2
    return [0x3100 | opc, 0x0300 | d << 1, w2, op]


tests = {}
for base in (4, 8, 12, 16, 20, 24, 28):
    for d in (base + 4 if base + 4 < NREGS else 0,):
        tests["x_b%d_d%d" % (base, d)] = (base, alu2(0x00c6, base + 1, base + 2, d), d, IN[1] ^ IN[2])
# the 0x9d form and the flag bits
tests["x9d_flags"] = (0, alu2(0x00c6, 1, 2, 4, opc=0x9d), 4, IN[1] ^ IN[2])
tests["x9d_noB"] = (0, alu2(0x00c6, 1, 2, 4, opc=0x9d, bflag=0), 4, IN[1] ^ IN[2])
tests["x99_noB"] = (0, alu2(0x00c6, 1, 2, 4, bflag=0), 4, IN[1] ^ IN[2])
tests["x99_noA"] = (0, alu2(0x00c6, 1, 2, 4, aflag=0), 4, IN[1] ^ IN[2])
tests["x9d_noA"] = (0, alu2(0x00c6, 1, 2, 4, opc=0x9d, aflag=0), 4, IN[1] ^ IN[2])

out, lines = {}, []
for name, (base, ins, d, want) in tests.items():
    body = [g9pasm.load(base, 1, 0, 4), g9pasm.wait(), ins, g9pasm.store(d, 0, 0)]
    out[name] = g9pasm.program(body, nregs=NREGS)
    lines.append("%s %08x" % (name, want))
g9pshaders.pack(out, os.path.join(HERE, "../../../build/gpu/isa-check2.bin"))
open(os.path.join(HERE, "../../../build/gpu/isa-check2.txt"), "w").write("\n".join(lines) + "\n")
