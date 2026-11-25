#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The USC code the kernel's GPU self-tests run (devices/a10 G7: the powervr
driver's Apple backend, pvr_apple_test.c), packed into a firmware file:

    python3 tools/gpu/g9p-selftest.py      # -> build/firmware/apple/g9p-selftest.bin

Our own Metal source (g9pshaders.py), built by Xcode's offline compiler for
applegpu_g9p; compiler output, so it stays in build/, never in git. Format:
"G9PT", u32 count, count x {char name[24], u32 offset, u32 size}, the code.
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from g9pshaders import own_shaders, g5_shaders  # noqa: E402

OUT = os.path.join(HERE, "../../build/firmware/apple/g9p-selftest.bin")

code = {}
own = own_shaders()
for k in ("tri_vs", "tri_fs", "bg_fs"):
    code[k] = own[k]
code["store_const"] = g5_shaders()["store_const"]
# the same code G5 ran (build/gpu/isa/g5.g9p.metallib), if it is there
old = os.path.join(HERE, "../../build/gpu/isa/g5.g9p.metallib")
if os.path.exists(old):
    import g9pbin
    assert g9pbin.kernels(old)[1] == code["store_const"], "store_const differs from G5's build"

hdr = struct.pack("<4sI", b"G9PT", len(code))
off = len(hdr) + 32 * len(code)
ents, blob = b"", b""
for name, c in code.items():
    ents += struct.pack("<24sII", name.encode(), off + len(blob), len(c))
    blob += c + bytes(-len(c) % 4)
os.makedirs(os.path.dirname(OUT), exist_ok=True)
open(OUT, "wb").write(hdr + ents + blob)
for name, c in code.items():
    print(f"{name}: {len(c):#x} bytes")
print(f"wrote {os.path.relpath(OUT)} ({len(hdr + ents + blob)} bytes)")
