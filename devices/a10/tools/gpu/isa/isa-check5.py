#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Workgroup IDs: 4 groups of 32 lanes (K3 = 3), every lane stores
wg * 1000 + lane to out[wg * 32 + lane]; the workgroup ID from threadgroup
memory word 0. build/gpu/isa-check5.{bin,txt}; probe args printed."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, ".."))
import g9pasm as A  # noqa: E402
import g9pshaders  # noqa: E402

LANES, GROUPS, NREGS = 32, 4, 8
want = [wg * 1000 + l for wg in range(GROUPS) for l in range(LANES)]


def body(bank1):
    return [A.tg_load(1, 0, bank1), A.wait(),
            A.mov_imm(4, LANES), A.alu2("imul", 2, 1, 4), A.alu2("iadd", 2, 2, 0),     # r2 = wg * 32 + lane
            A.mov_imm(4, 1000), A.alu2("imul", 3, 1, 4), A.alu2("iadd", 3, 3, 0),      # r3 = wg * 1000 + lane
            A.addr(6, 0, 2, 2), A.mem_at(True, A.SIZE["u32"], 3, 6)]


out = {"wg_b0": A.program(body(False), NREGS, count_half=60),
       "wg_b1": A.program(body(True), NREGS, count_half=60)}
g9pshaders.pack(out, os.path.join(HERE, "../../../build/gpu/isa-check5.bin"))
open(os.path.join(HERE, "../../../build/gpu/isa-check5.txt"), "w").write(
    "".join("%s %s\n" % (n, " ".join("%08x" % w for w in want)) for n in out))
print("x=%d w6=1 w7=%d tab=30:0x%08x temps=0x20 k0=0x4103" % (GROUPS - 1, (LANES - 1) << 20, LANES | LANES << 16))
