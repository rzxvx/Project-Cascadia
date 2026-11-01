#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""G3 (research/gpu.md): compares what the phone had under Linux (g3-check.sh
output: live ADT + firmware __TEXT) with what iOS's iBoot does.

    g3-compare.py G3DIR ARMFW_BIN

ARMFW_BIN: the IM4P payload of Firmware/agx/armfw_g12p.im4p (rkosftab + Mach-O).
"""
import os
import struct
import sys



def adt_node(d, o):
    """Raw ADT: node = u32 nprops, u32 nchildren, props (name[32], u32 size
    with the top bit a template flag, value padded to 4), children."""
    nprops, nchild = struct.unpack_from("<II", d, o)
    o += 8
    props = {}
    for _ in range(nprops):
        name = d[o:o + 32].split(b"\0")[0].decode()
        size = struct.unpack_from("<I", d, o + 32)[0] & 0x7fffffff
        props[name] = d[o + 36:o + 36 + size]
        o += 36 + ((size + 3) & ~3)
    children = []
    for _ in range(nchild):
        c, o = adt_node(d, o)
        children.append(c)
    return (props, children), o


def find(node, path):
    for part in path.strip("/").split("/"):
        node = next(c for c in node[1] if c[0].get("name", b"").rstrip(b"\0").decode() == part)
    return node[0]


g3, armfw = sys.argv[1], sys.argv[2]
root, _ = adt_node(open(os.path.join(g3, "adt.bin"), "rb").read(), 0)
asc = find(root, "arm-io/gfx-asc")
nub = find(root, "arm-io/gfx-asc/iop-gfx-nub")
sr = asc.get("segment-ranges", b"")
for i in range(0, len(sr), 32):
    pa, va, pa2, size, flags = struct.unpack_from("<QQQII", sr, i)
    print(f"gfx-asc segment {i // 32}: phys {pa:#x} fw VA {va:#x} size {size:#x} flags {flags}")
print("iop-gfx-nub pre-loaded:", nub.get("pre-loaded", b"").hex() or "absent")
sgx = find(root, "arm-io/sgx")
for k in ("gfx-handoff-base", "gfx-shared-region-base", "gpu-region-base"):
    print(f"sgx {k}:", hex(struct.unpack("<Q", sgx[k])[0]) if k in sgx else "absent")
bc = find(root, "chosen").get("breadcrumbs", b"")
print("breadcrumbs mention gfxf (67667866):", b"67667866" in bc)
print("breadcrumbs:", bc.split(b"\0")[0].decode(errors="replace")[:400])

mo = open(armfw, "rb").read()[0x40:]
live = open(os.path.join(g3, "gfx-text.bin"), "rb").read()
ref = mo[0x4000:0x4000 + len(live)]
diff = [i for i in range(len(live)) if live[i] != ref[i]]
print(f"__TEXT vs IPSW: {len(diff)} bytes differ", "(all zero?)" if not any(live) else "")

# the RTKit boot-argument table iBoot fills (gpu.md, G1 results)
o = 0x4a02a
while o < 0x4a1f0:
    tag = live[o:o + 4][::-1].decode("ascii", "replace")
    sz = struct.unpack_from("<I", live, o + 4)[0]
    if sz not in (1, 2, 4, 8):
        break
    fmt = {1: "<B", 2: "<H", 4: "<I", 8: "<Q"}[sz]
    v, r = struct.unpack_from(fmt, live, o + 8)[0], struct.unpack_from(fmt, ref, o + 8)[0]
    if v != r:
        print(f"  {tag} = {v:#x} (IPSW {r:#x})")
    o += 8 + sz
