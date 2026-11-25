#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Float ALU: in[0..3] = 1.5, 2.0, 0.25, -3.0 -> r0..r3; one operation -> r4 ->
out[0]. build/gpu/isa-check3.{bin,txt} (name, expected bits)."""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, ".."))
import g9pasm  # noqa: E402
import g9pshaders  # noqa: E402

F = [1.5, 2.0, 0.25, -3.0]
fb = lambda f: struct.unpack("<I", struct.pack("<f", f))[0]


def alu2(op, a, b, d=4):
    return g9pasm.alu2({0x44: "fadd", 0x48: "fmul", 0x4a: "fmul_sat"}[op & 0xff], d, a, b, neg_a=bool(op & 0x4000))


def alu3(op, a, b, c, d=4):
    return g9pasm.alu3("ffma", d, a, b, c, neg_a=bool(op & 0x4000))


tests = {
    "fadd_1_2": (alu2(0x0044, 1, 2), F[1] + F[2]),
    "fmul_1_3": (alu2(0x0048, 1, 3), F[1] * F[3]),
    "fsub_negA": (alu2(0x4044, 1, 0), F[0] - F[1]),
    "fmul_negA": (alu2(0x4048, 0, 1), -F[0] * F[1]),
    "fsat_mul": (alu2(0x004a, 1, 0), min(max(F[1] * F[0], 0), 1)),
    "fsat_neg": (alu2(0x004a, 3, 0), 0.0),
    "fma_0_1_2": (alu3(0x004c, 0, 1, 2), F[0] * F[1] + F[2]),
    "fma_3_2_1": (alu3(0x004c, 3, 2, 1), F[3] * F[2] + F[1]),
}
out, lines = {}, []
for name, (ins, want) in tests.items():
    body = [g9pasm.load(0, 1, 0, 4), g9pasm.wait(), ins, g9pasm.store(4, 0, 0)]
    out[name] = g9pasm.program(body, nregs=5)
    lines.append("%s %08x" % (name, fb(want)))
g9pshaders.pack(out, os.path.join(HERE, "../../../build/gpu/isa-check3.bin"))
open(os.path.join(HERE, "../../../build/gpu/isa-check3.txt"), "w").write("\n".join(lines) + "\n")
print(" ".join("in=1:%d:0x%08x" % (i, fb(f)) for i, f in enumerate(F)))
