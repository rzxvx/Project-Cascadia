#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The move-immediate encoding of applegpu_g9p, measured: o[0] = 1 << i for
every bit (and 0), which encoding bits each value bit flips. Prints the map."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import g9pisa  # noqa: E402

kernels = {"k%d" % i: "kernel void k%d(device uint *o [[buffer(0)]]) { o[0] = 0x%08xu; }" % (i, 1 << i)
           for i in range(32)}
c = g9pisa.corpus("imm-map", kernels)


def mov(n):
    return [i for i in c[n] if i[0] & 0xff == 0x98][0]


forms = {i: mov("k%d" % i) for i in range(32)}
for i in (0, 16):
    print("form of bit %2d: %s" % (i, g9pisa.fmt(forms[i])))
for ob, fl in g9pisa.unique_bits(forms).items():
    print("value bit %2d -> %s" % (ob, ", ".join("W%d.%d" % f for f in fl)))
