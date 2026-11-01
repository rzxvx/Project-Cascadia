#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Names the C++ classes in `kmem ptrs` output.

    kmem ptrs VA LEN | kclass.py VTABLES.tsv SLIDE

VTABLES.tsv: iometa -a output reduced to vtab/size/meta/parent/metavtab/class/
bundle columns (a13/build/gpu/20E247/vtables.tsv).  SLIDE: from `kmem info`.
"""
import re
import sys

vt = {}
for line in open(sys.argv[1]):
    f = line.rstrip("\n").split("\t")
    if int(f[0], 16):
        vt[int(f[0], 16)] = (f[5], int(f[1], 16))
slide = int(sys.argv[2], 0)
for line in sys.stdin:
    m = re.search(r"\[0x([0-9a-f]+)\]$", line.rstrip())
    cls = vt.get(int(m.group(1), 16) - slide) if m else None
    print(line.rstrip() + (f"  {cls[0]} (0x{cls[1]:x})" if cls else ""))
