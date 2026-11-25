#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Register fields of the two-source ALU (iadd): 32 values loaded (8 uint4
loads), o[0] = v[i] + v[j] * 3 -- the multiply's and the add's source
registers known from the loads' destination registers. Prints, per (reg A,
reg B), the ALU instruction."""
import itertools
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import g9pisa  # noqa: E402

N = 32
random.seed(1)
pairs = random.sample([(i, j) for i in range(N) for j in range(N) if i != j], 120)
k = {}
for i, j in pairs:
    loads = " ".join("uint4 v%d = A[%d];" % (q, q) for q in range(N // 4))
    keep = " + ".join("v%d.x" % q for q in range(N // 4))     # keep all loads live
    el = lambda e: "v%d.%s" % (e // 4, "xyzw"[e % 4])
    k["p%d_%d" % (i, j)] = ("kernel void p%d_%d(device uint *o [[buffer(0)]], device const uint4 *A [[buffer(1)]]) "
                            "{ %s o[0] = %s ^ %s; o[1] = %s; }" % (i, j, loads, el(i), el(j), keep))
c = g9pisa.corpus("regmap", k)
for name, ins in c.items():
    i, j = map(int, name[1:].split("_"))
    elreg = {}
    for x in ins:
        if x[0] & 0xff == 0x12 and len(x) >= 3 and not x[1] & 0x80:       # a load
            reg = (x[1] >> 1) & 0x3f
            cnt = {4: 1, 5: 2, 6: 3, 7: 4}.get(x[0] >> 11 & 7, 1)
            off = x[2] & 0xff
            for e in range(cnt):
                elreg[off * 4 + e] = reg + e
    xors = [x for x in ins if (x[0] & 0xff) in (0x99, 0x9d) and (x[-1] & 0xff) == 0xc6]
    if len(xors) != 1 or i not in elreg or j not in elreg:
        print(name, "?", " | ".join(g9pisa.fmt(x)[7:] for x in ins if (x[0] & 0xff) in (0x99, 0x9d)))
        continue
    print("r%-2d ^ r%-2d : %s" % (elreg[i], elreg[j], g9pisa.fmt(xors[0])))
