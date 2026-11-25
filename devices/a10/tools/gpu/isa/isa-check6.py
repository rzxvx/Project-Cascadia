#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Unary float operations, conversions, shifts: a source from mov_imm, the
operation into r5, stored to out[0]. build/gpu/isa-check6.{bin,txt}."""
import math
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, ".."))
import g9pasm as A  # noqa: E402
import g9pshaders  # noqa: E402

fb = lambda f: struct.unpack("<I", struct.pack("<f", f))[0]
u32 = lambda v: v & 0xffffffff
tests = {
    "rcp_2": ([A.mov_imm(4, fb(2.0)), A.funary("rcp", 5, 4)], fb(0.5)),
    "rsqrt_4": ([A.mov_imm(4, fb(4.0)), A.funary("rsqrt", 5, 4)], fb(0.5)),
    "log2_8": ([A.mov_imm(4, fb(8.0)), A.funary("log2", 5, 4)], fb(3.0)),
    "exp2_3": ([A.mov_imm(4, fb(3.0)), A.funary("exp2", 5, 4)], fb(8.0)),
    "floor_m25": ([A.mov_imm(4, fb(-2.5)), A.funary("floor", 5, 4)], fb(-3.0)),
    "u2f_7": ([A.mov_imm(4, 7), A.convert("u2f", 5, 4)], fb(7.0)),
    "i2f_m5": ([A.mov_imm(4, u32(-5)), A.convert("i2f", 5, 4)], fb(-5.0)),
    "f2u_79": ([A.mov_imm(4, fb(7.9)), A.convert("f2u", 5, 4)], 7),
    "f2i_m55": ([A.mov_imm(4, fb(-5.5)), A.convert("f2i", 5, 4)], u32(-5)),
    "shl_reg": ([A.mov_imm(4, 0x10), A.mov_imm(3, 3), A.shift(5, 4, 3, False)], 0x80),
    "shr_reg": ([A.mov_imm(4, 0x80001000), A.mov_imm(3, 4), A.shift(5, 4, 3, True)], 0x08000100),
    "shl_imm": ([A.mov_imm(4, 0x10), A.shift_imm(5, 4, 3, False)], 0x80),
    "shr_imm": ([A.mov_imm(4, 0x80001000), A.shift_imm(5, 4, 12, True)], 0x00080001),
}
out, lines = {}, []
for name, (body, want) in tests.items():
    out[name] = A.program(body + [A.store(5, 0, 0)], nregs=8)
    lines.append("%s %08x" % (name, want))
g9pshaders.pack(out, os.path.join(HERE, "../../../build/gpu/isa-check6.bin"))
open(os.path.join(HERE, "../../../build/gpu/isa-check6.txt"), "w").write("\n".join(lines) + "\n")
print("temps=0x20 k0=0x4103")
