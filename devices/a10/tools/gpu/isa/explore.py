#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compile kernels and print their code split into instructions, the frame
(prologue / epilogue) left out with -b.

    explore.py [-b] FILE.metal     one kernel per line (`kernel void NAME(...) {...}`)
    explore.py [-b] -e 'kernel void k(...) {...}' ...
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import g9pisa  # noqa: E402

FRAME = (0x81, 0xc8)


def body(insns):
    """the instructions between the prologue (0x81, first 0xc8) and the epilogue
    (last 0xc8 and the jump after it)"""
    i = 0
    while i < len(insns) and insns[i][0] & 0xff in FRAME and i < 2:
        i += 1
    j = len(insns)
    if j >= 2 and insns[j - 2][0] & 0xff == 0xc8:
        j -= 2
    return insns[i:j]


def main(argv):
    bare = "-b" in argv
    argv = [a for a in argv if a != "-b"]
    lines = argv[1:] if argv and argv[0] == "-e" else open(argv[0]).read().splitlines()
    kernels = {}
    for ln in lines:
        m = re.match(r"\s*kernel\s+void\s+(\w+)", ln)
        if m:
            kernels[m.group(1)] = ln
    c = g9pisa.corpus("explore", kernels)
    w = max(len(k) for k in kernels)
    for k in kernels:
        ins = body(c[k]) if bare else c[k]
        print(k.ljust(w), " | ".join(g9pisa.fmt(i)[7:] for i in ins))


if __name__ == "__main__":
    main(sys.argv[1:])
