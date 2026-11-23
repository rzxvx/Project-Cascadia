#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""G1 run 6 (devices/a10/docs/gpu.md): apply the GPU's PMGR fabric-bridge
settings from the ADT, then power the core and read its ID.

    ./cascadia a10 boot proxy
    M1N1DEVICE=/dev/cu.usbmodemXXXX1 build/proxyvenv/bin/python tools/gpu/gpu-bridge.py [--no-apply] [--reset] [--bridges 8,9]

Run from devices/a10. The ADT's pmgr node has `#bridges` (14),
`bridge-reg-index` (10) and `bridge-settings-N` (offset / value pairs); ApplePMGR
maps bridge i from pmgr reg (bridge-reg-index + i) ("Bridge %u, virtual
address ... physical address") and writes its settings; neither the iBSS nor
m1n1 does. Bridges 8 (pmgr reg 18 = 0x201f00000) and 9 (reg 19 =
0x201f80000) sit inside the GPU's address block. Bridge 9's settings are 32
pairs, offsets 0..0x7c, all 0x40101.

1. PMGR GFX on (as in runs 1-5).
2. The chosen bridges' registers at their settings' offsets, read.
3. Unless --no-apply: the settings written in ADT order, read back.
4. Fender +0x1000 = 0x11, wait for state 1, sleep 10 ms (the kext's IOSleep);
   with --reset also Fender +0x2058 = 1 and wait for it to clear.
5. RGX +0x18 / +0x20 (32-bit). This is the read that hung runs 1-4.
"""
import os
import struct
import sys
import time

PROXY = os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../build/m1n1/proxyclient")
sys.path.insert(0, os.environ.get("M1N1_PROXYCLIENT", PROXY))

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gpusetup import p, u, iface, GUARD  # noqa: E402  (m1n1 proxy, or GPU_BACKEND=linux)

adt = u.adt
sgx = adt["/arm-io/sgx"]
RGX = sgx.get_reg(0)[0]
FENDER = sgx.get_reg(1)[0]
pm = adt["/arm-io/pmgr"]
breg = pm.bridge_reg_index
bridges = [8, 9]
for i, a in enumerate(sys.argv):
    if a == "--bridges":
        bridges = [int(x) for x in sys.argv[i + 1].split(",")]


def out(s):
    print(f"[gpu-bridge] {s}", flush=True)


def settings(n):
    b = bytes(pm._properties[f"bridge-settings-{n}"])
    w = struct.unpack(f"<{len(b) // 4}I", b)
    return [(w[i], w[i + 1]) for i in range(0, len(w) - 1, 2)]


def rd(addr):
    n = p.get_exc_count()
    v = p.read32(addr)
    return None if p.get_exc_count() != n else v


p.set_exc_guard(GUARD.SKIP | GUARD.SILENT)
gfx = next(d for d in pm.devices if adt.pmgr_dev_get_id(d) == 124)
PS = adt.pmgr_dev_get_addr(gfx)
p.pmgr_adt_power_enable("/arm-io/sgx")
p.pmgr_adt_power_enable("/arm-io/gfx-kf")
time.sleep(0.01)
v = p.read32(PS)
out(f"PMGR GFX = {v:#010x}")
if (v >> 4) & 0xf != 0xf:
    sys.exit("[gpu-bridge] GFX not on; stop")

for n in bridges:
    base = pm.get_reg(breg + n)[0]
    st = settings(n)
    out(f"bridge {n} @ {base:#x}: {len(st)} settings")
    for off, val in st:
        out(f"  +{off:#06x} = {rd(base + off)!s:>10}  (ADT {val:#010x})" if rd(base + off) is None else
            f"  +{off:#06x} = {rd(base + off):#010x}  (ADT {val:#010x})")
    if "--no-apply" not in sys.argv:
        for off, val in st:
            p.write32(base + off, val)
        for off, val in st:
            r = rd(base + off)
            out(f"  after: +{off:#06x} = " + ("<fault>" if r is None else f"{r:#010x}"))

p.write32(FENDER + 0x1000, 0x11)
t = time.time()
while time.time() - t < 0.2:
    s = p.read32(FENDER + 0x1000)
    if s & 0x1f == 1:
        break
out(f"Fender +0x1000 = 0x11 -> {s:#x}")
time.sleep(0.01)
if "--reset" in sys.argv:
    p.write32(FENDER + 0x2058, 1)
    t = time.time()
    while time.time() - t < 0.2 and p.read32(FENDER + 0x2058) & 1:
        pass
    out(f"Fender +0x2058 -> {p.read32(FENDER + 0x2058):#x}")
if s & 0x1f != 1:
    sys.exit("[gpu-bridge] no state 1; not reading RGX")
for off in (0x18, 0x20):
    out(f"reading RGX +{off:#x} ...")
    r = rd(RGX + off)
    out(f"RGX +{off:#x} = " + ("<fault>" if r is None else f"{r:#010x}"))
p.set_exc_guard(GUARD.OFF)
out("end")
