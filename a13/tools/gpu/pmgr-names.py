#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Annotate `PA = value` lines (kmem mmio / pmgr-dump.py) with the PMGR
device whose power-state register PA is, from an ADT dump; decodes the
power-state fields. With two files: side by side, differing lines marked.

    pmgr-names.py ADT ios.txt [proxy.txt]
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "../../build/m1n1/proxyclient"))
from m1n1.adt import load_adt  # noqa: E402

adt = load_adt(open(sys.argv[1], "rb").read())
names = {}
for d in adt["/arm-io/pmgr"].devices:
    if d.flags.no_ps:
        continue
    try:
        names.setdefault(adt.pmgr_dev_get_addr(d), d.name)
    except Exception:
        pass


def load(f):
    out = {}
    for line in open(f):
        m = re.match(r"(0x[0-9a-f]+) = (0x[0-9a-f]+)", line)
        if m:
            out[int(m[1], 16)] = int(m[2], 16)
    return out


def ps(v):
    return (f"auto={v >> 28 & 1} autost={v >> 24 & 0xf:x} min={v >> 16 & 0xf:x} "
            f"act={v >> 4 & 0xf:x} tgt={v & 0xf:x}")


files = [load(f) for f in sys.argv[2:]]
for a in sorted(set().union(*files)):
    vals = [f.get(a, 0) for f in files]
    mark = "*" if len(set(vals)) > 1 else " "
    n = names.get(a, "")
    cols = "  ".join(f"{v:#010x}" for v in vals)
    extra = "  |  ".join(ps(v) for v in vals) if n else ""
    print(f"{mark} {a:#x} {n:14} {cols}  {extra}")
