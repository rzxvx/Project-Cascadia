#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Uniform (common-store) first sources: d = CS dword u + r[zero] for
several u and destination registers, stored to out[0]. The probe's tab=
fills the common store (dword u = 0x1000 + u); run with the args printed.
build/gpu/isa-check9.{bin,txt}. (Dwords from 16 on read other values in
g9p-probe's compute task -- its common store's size, to check; the Gallium
driver's vertex shaders read dwords 4-19 right.)"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, ".."))
import g9pasm as A  # noqa: E402
import g9pshaders  # noqa: E402

NREGS = 31
US = (2, 10, 14, 15)
out, lines = {}, []
for u in US:
    for d in (4, 16, 20, 28):
        body = [A.mov_imm(1, 0), A.alu2_uniform("iadd", d, u, 1), A.store(d, 0, 0)]
        out["u%d_r%d" % (u, d)] = A.program(body, nregs=NREGS, nbufs=1, count_half=60)
        lines.append("u%d_r%d %08x" % (u, d, 0x1000 + u))
g9pshaders.pack(out, os.path.join(HERE, "../../../build/gpu/isa-check9.bin"))
open(os.path.join(HERE, "../../../build/gpu/isa-check9.txt"), "w").write("\n".join(lines) + "\n")
print("w6=1 temps=0x80 k0=0x10103 tab=30:0x00010001," + ",".join("%d:0x%x" % (u, 0x1000 + u) for u in US))
