#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Control flow on 32 lanes: r0 = the lane (local ID), every lane stores its
result to out[lane] through an address built from buffer slot 0's base:
register-indexed stores, if / else on the mask stack, select, a loop whose
lanes leave at different iterations. build/gpu/isa-check4.{bin,txt}; probe
args printed (32 threads, the thread count at common-store dword 30)."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, ".."))
import g9pasm as A  # noqa: E402
import g9pshaders  # noqa: E402

LANES = 32
NREGS = 8
ST = NREGS          # the mask stack's register
CH = 60             # common-store half with the thread count (dword 30)


def store_lane(reg):
    """out[r0] = r[reg] (r6:r7 the address)"""
    return [A.addr(6, 0, 0, 2), A.mem_at(True, A.SIZE["u32"], reg, 6)]


def size(insns):
    return 2 * sum(len(i) for i in insns)


tests = {}
tests["lane3p1"] = ([A.mov_imm(1, 3), A.alu2("imul", 2, 0, 1), A.mov_imm(3, 1), A.alu2("iadd", 2, 2, 3)]
                    + store_lane(2), [3 * i + 1 for i in range(LANES)])
tests["ifelse"] = ([A.cmp_imm("ult", 0, 10), A.push_if(ST), A.mov_imm(2, 1), A.else_(ST), A.mov_imm(2, 2),
                    A.pop(ST)] + store_lane(2), [1 if i < 10 else 2 for i in range(LANES)])
tests["sel"] = ([A.mov_imm(1, 10), A.mov_imm(2, 7), A.mov_imm(3, 9), A.sel("ult", 4, 0, 1, 2), A.movf(4, 3)]
                + store_lane(4), [7 if i < 10 else 9 for i in range(LANES)])
loop_body = [A.cmp_reg("ult", 2, 0), A.loop_break(ST, False), A.alu2("iadd", 1, 1, 5), A.alu2("iadd", 2, 2, 3)]
tests["loop"] = ([A.mov_imm(1, 0), A.mov_imm(2, 0), A.mov_imm(3, 1), A.mov_imm(5, 2), A.loop_begin(ST)]
                 + loop_body + [A.jump(A.JMP_ANY, -size(loop_body)), A.pop(ST, 2)] + store_lane(1),
                 [2 * i for i in range(LANES)])

out, lines = {}, []
for name, (body, want) in tests.items():
    out[name] = A.program(body, nregs=NREGS, count_half=CH)
    lines.append(name + " " + " ".join("%08x" % w for w in want))
g9pshaders.pack(out, os.path.join(HERE, "../../../build/gpu/isa-check4.bin"))
open(os.path.join(HERE, "../../../build/gpu/isa-check4.txt"), "w").write("\n".join(lines) + "\n")
print("w6=1 w7=%d tab=30:0x%08x temps=0x20 k0=0x4103" % ((LANES - 1) << 20, LANES | LANES << 16))
