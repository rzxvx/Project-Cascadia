#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Breaks and continues inside ifs in a loop (the 0x88 `86d6` form setting
the lanes' stack level to an immediate), long forward jumps. 32 lanes, r0 =
the lane, out[lane] = the result. build/gpu/isa-check8.{bin,txt}."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, ".."))
import g9pasm as A  # noqa: E402
import g9pshaders  # noqa: E402

LANES = 32
NREGS = 8
ST = NREGS
CH = 60


def store_lane(reg):
    return [A.addr(6, 0, 0, 2), A.mem_at(True, A.SIZE["u32"], reg, 6)]


def size(insns):
    return 2 * sum(len(i) for i in insns)


def loop(body, fixup=None):
    """loop_begin, body, [fixup], jump back while any lane is active, pop 2"""
    tail = fixup or []
    inner = body + tail
    return [A.loop_begin(ST)] + inner + [A.jump(A.JMP_ANY, -size(inner)), A.pop(ST, 2)]


tests = {}
# r1 = s, r2 = i, r3 = 1, r5 = 2: if (i == lane) break; s += 2; i++
init = [A.mov_imm(1, 0), A.mov_imm(2, 0), A.mov_imm(3, 1), A.mov_imm(5, 2)]
body = [A.cmp_reg("eq", 2, 0), A.push_if(ST), A.brk(ST, 3), A.pop(ST),
        A.alu2("iadd", 1, 1, 5), A.alu2("iadd", 2, 2, 3)]
tests["brk_if"] = (init + loop(body) + store_lane(1), [2 * i for i in range(LANES)])
# the same two ifs deep: if (i < 1000) { if (i == lane) break; s += 1 } s += 2
body = [A.mov_imm(4, 1000), A.cmp_reg("ult", 2, 4), A.push_if(ST),
        A.cmp_reg("eq", 2, 0), A.push_if(ST), A.brk(ST, 4), A.pop(ST), A.alu2("iadd", 1, 1, 3),
        A.pop(ST), A.alu2("iadd", 1, 1, 5), A.alu2("iadd", 2, 2, 3)]
tests["brk_if2"] = (init + loop(body) + store_lane(1), [3 * i for i in range(LANES)])
# continue: i++; if (i > lane) break; if (i & 1) continue; s += i
cont = [A.alu2("iadd", 2, 2, 3), A.cmp_reg("ugt", 2, 0), A.push_if(ST), A.brk(ST, 3), A.pop(ST),
        A.alu2("and", 4, 2, 3), A.cmp_reg("eq", 4, 3), A.push_if(ST), A.brk(ST, 2), A.pop(ST),
        A.alu2("iadd", 1, 1, 2)]
want_cont = [sum(k for k in range(1, i + 1) if k % 2 == 0) for i in range(LANES)]
# without a fixup the continued lanes stay at level 1: every lane continues
# at i = 1, none is left active, the loop ends with s = 0
tests["cont_nofix"] = (init + loop(cont) + store_lane(1), [0] * LANES)
# fixup: an always-true compare and the conditional while (86d7) bring the
# level-1 lanes back (the 0x88 form with level 0 does not)
tests["cont_fix_d7"] = (init + loop(cont, [A.cmp_reg("eq", 0, 0), A.loop_break(ST, False)]) + store_lane(1),
                        want_cont)
# a long forward jump: jump-none over 0x180 bytes of body (no lane active
# for lanes >= 16: they skip; the others run it)
pad = [A.alu2("iadd", 1, 1, 3)] * 48          # 48 x 8 bytes, s += 48
lj = [A.mov_imm(1, 0), A.mov_imm(3, 1), A.mov_imm(4, 16), A.cmp_reg("ult", 0, 4), A.push_if(ST)]
lj += [A.jump(A.JMP_NONE, 6 + size(pad))] + pad + [A.pop(ST)]
tests["longjump"] = (lj + store_lane(1), [48 if i < 16 else 0 for i in range(LANES)])
# the same with the whole group off: lanes >= 40 (none)
lj2 = [A.mov_imm(1, 7), A.mov_imm(3, 1), A.mov_imm(4, 40), A.cmp_reg("ugt", 0, 4), A.push_if(ST)]
lj2 += [A.jump(A.JMP_NONE, 6 + size(pad))] + pad + [A.pop(ST)]
tests["longjump_skip"] = (lj2 + store_lane(1), [7] * LANES)

out, lines = {}, []
for name, (b, want) in tests.items():
    out[name] = A.program(b, nregs=NREGS, count_half=CH)
    lines.append(name + " " + " ".join("%08x" % w for w in want))
g9pshaders.pack(out, os.path.join(HERE, "../../../build/gpu/isa-check8.bin"))
open(os.path.join(HERE, "../../../build/gpu/isa-check8.txt"), "w").write("\n".join(lines) + "\n")
print("w6=1 w7=%d tab=30:0x%08x temps=0x20 k0=0x4103" % ((LANES - 1) << 20, LANES | LANES << 16))
