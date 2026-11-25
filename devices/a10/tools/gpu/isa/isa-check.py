#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Integer ALU, register fields, move immediate -- hand-assembled programs
(g9pasm) for g9p-probe: in[0..3] = 1, 0x10, 0x100, 0x1000 (buffer slot 1 =
probe buf 1) -> r0..r3, the instruction under test -> r4 -> out[0] (slot 0).
The one-hot inputs make the result name the registers read.
build/gpu/isa-check.{bin,txt}; run: isa-run.sh isa-check IN-ARGS (printed)."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, ".."))
import g9pasm  # noqa: E402
import g9pshaders  # noqa: E402

IN = [0x1, 0x10, 0x100, 0x1000]
tests = {}
for a in range(4):
    for b in range(4):
        tests["xor_r%d_r%d" % (a, b)] = (g9pasm.alu2("xor", 4, a, b), IN[a] ^ IN[b])
tests["iadd_r1_r2"] = (g9pasm.alu2("iadd", 4, 1, 2), IN[1] + IN[2])
tests["isub_r3_r1"] = (g9pasm.alu2("isub", 4, 3, 1), (IN[3] - IN[1]) & 0xffffffff)
tests["imul_r1_r2"] = (g9pasm.alu2("imul", 4, 1, 2), IN[1] * IN[2])
tests["and_r1_r1"] = (g9pasm.alu2("and", 4, 1, 1), IN[1])
tests["or_r0_r3"] = (g9pasm.alu2("or", 4, 0, 3), IN[0] | IN[3])
for v in (0, 1, 0xbeef, 0x10000, 0xdeadbeef, 0x80000000):
    tests["mov_%x" % v] = (g9pasm.mov_imm(4, v), v)

out, lines = {}, []
for name, (ins, want) in tests.items():
    body = [g9pasm.load(0, 1, 0, 4), g9pasm.wait(), ins, g9pasm.store(4, 0, 0)]
    out[name] = g9pasm.program(body, nregs=5)
    lines.append("%s %08x" % (name, want))
g9pshaders.pack(out, os.path.join(HERE, "../../../build/gpu/isa-check.bin"))
open(os.path.join(HERE, "../../../build/gpu/isa-check.txt"), "w").write("\n".join(lines) + "\n")
print(" ".join("in=1:%d:0x%x" % (i, v) for i, v in enumerate(IN)))
