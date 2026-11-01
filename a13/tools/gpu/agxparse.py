#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Parses GPU firmware structures dumped from the iPhone's iOS 16.4 with
m1n1's AGX definitions (Asahi, firmware interface V13_3).

    agxparse.py [-n] DUMPDIR STRUCT GPUVA [M1N1_PROXYCLIENT]

DUMPDIR holds bufs.list ("kernelVA gpuVA size" per line) and the dumps
bufs/b_<gpuVA>.bin (kmem kd / pd).  STRUCT: a class in m1n1.fw.agx.initdata,
e.g. InitData_RegionB.  Pointers inside are followed through the dumps,
unless -n.
"""
import io
import os
import sys

args = sys.argv[1:]
recurse = True
if args[0] == "-n":
    recurse = False
    args = args[1:]
dumpdir, struct_name, va = args[0], args[1], int(args[2], 0)
proxy = args[3] if len(args) > 3 else os.path.join(
    os.path.dirname(__file__), "../../build/m1n1/proxyclient")
os.environ.setdefault("AGX_FWVER", "V13_3")
os.environ.setdefault("AGX_GPU", "G13")
sys.path.insert(0, os.path.abspath(proxy))

import construct  # noqa: E402
from m1n1.fw.agx import initdata  # noqa: E402

# Asahi's constants are what M1/M2 firmware wants; on G12 some differ.  Note
# them instead of stopping at the first one.
mismatches = []
_const_parse = construct.Const._parse


def _soft_const(self, stream, context, path):
    pos = stream.tell()
    try:
        return _const_parse(self, stream, context, path)
    except construct.ConstError:
        stream.seek(pos)
        got = self.subcon._parsereport(stream, context, path)
        mismatches.append((path, pos, self.value, got))
        return got


construct.Const._parse = _soft_const


class GpuMem(io.RawIOBase):
    """The dumped allocations as one stream addressed by GPU VA."""

    def __init__(self, d):
        self.regions = []
        for line in open(os.path.join(d, "bufs.list")):
            _, g, _ = line.split()
            p = os.path.join(d, "bufs", f"b_{g}.bin")
            if os.path.exists(p):
                self.regions.append((int(g, 16), open(p, "rb").read()))
        self.pos = 0
        self.recurse = True
        self.misses = set()

    def seekable(self):
        return True

    def readable(self):
        return True

    def seek(self, off, whence=0):
        self.pos = off if whence == 0 else self.pos + off
        return self.pos

    def tell(self):
        return self.pos

    def read(self, n=-1):
        for base, data in self.regions:
            if base <= self.pos < base + len(data):
                o = self.pos - base
                out = data[o:o + n]
                self.pos += len(out)
                return out + bytes(n - len(out))
        self.misses.add(self.pos & ~0x3fff)
        self.pos += n
        return bytes(n)


mem = GpuMem(dumpdir)
mem.recurse = recurse
cls = getattr(initdata, struct_name)
mem.seek(va)
obj = cls.parse_stream(mem)
print(obj)
for path, pos, want, got in mismatches:
    print(f"CONST MISMATCH {path} @ {pos:#x}: m1n1 {want!r}, here {got!r}")
if mem.misses:
    print("not dumped:", " ".join(hex(m) for m in sorted(mem.misses)), file=sys.stderr)
