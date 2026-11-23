#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""G1 run 3 (devices/a10/docs/gpu.md): a read-only survey of what the GPU's
power path looks like under a cold m1n1, to find what is missing before the
Rogue registers answer.

    ./cascadia a10 boot proxy
    M1N1DEVICE=/dev/cu.usbmodemXXXX1 build/proxyvenv/bin/python tools/gpu/gpu-survey.py [--no-rgx]

Run from devices/a10. Every line is printed as it is read, so a hang leaves
what came before it in the log.

1. PMGR perf-regs block 0 (reg 0 + 0xb4000, 0x400 bytes) with GFX off.
2. PMGR GFX on (m1n1's pmgr_adt_power_enable of sgx and gfx-kf); the perf
   block again, changed words only.
3. All of Fender's first 0x4000 bytes, nonzero words (exception guard on).
4. Fender +0x1000 = 0x11 (state 1, acknowledged in 0.2 ms in runs 1-2), then
   Fender again, changed words only; the perf block again.
5. Unless --no-rgx: one 32-bit read of RGX +0x18 (runs 1-2: a 64-bit read hung).
"""
import os
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
PMGR0 = pm.get_reg(0)[0]
PERF = PMGR0 + pm.perf_regs[0].offset
gfx = next(d for d in pm.devices if adt.pmgr_dev_get_id(d) == 124)
PS_GFX = adt.pmgr_dev_get_addr(gfx)


def out(s):
    print(f"[survey] {s}", flush=True)


def dump(base, size, tag, prev=None, step=4):
    vals = {}
    errs = 0
    for off in range(0, size, step):
        n = p.get_exc_count()
        v = p.read32(base + off)
        if p.get_exc_count() != n:
            errs += 1
            vals[off] = None
            if prev is None or prev.get(off, 0) is not None:
                out(f"{tag} +{off:#06x} <fault>")
            continue
        vals[off] = v
        if prev is None:
            if v:
                out(f"{tag} +{off:#06x} = {v:#010x}")
        elif prev.get(off) != v:
            out(f"{tag} +{off:#06x} = {v:#010x} (was {prev.get(off)!s})")
    out(f"{tag} done ({size // step} words, {errs} faults)")
    return vals


p.set_exc_guard(GUARD.SKIP | GUARD.SILENT)
out(f"PMGR GFX {PS_GFX:#x} = {p.read32(PS_GFX):#010x}; perf block {PERF:#x}")
perf0 = dump(PERF, 0x400, "perf(gfx off)")

p.pmgr_adt_power_enable("/arm-io/sgx")
p.pmgr_adt_power_enable("/arm-io/gfx-kf")
time.sleep(0.01)
v = p.read32(PS_GFX)
out(f"PMGR GFX after enable = {v:#010x}")
if (v >> 4) & 0xf != 0xf:
    sys.exit("[survey] GFX not on; stop")
perf1 = dump(PERF, 0x400, "perf(gfx on)", perf0)

f0 = dump(FENDER, 0x4000, "fender")
p.write32(FENDER + 0x1000, 0x11)
t0 = time.time()
while time.time() - t0 < 0.2:
    s = p.read32(FENDER + 0x1000)
    if s & 0x1f == 1:
        break
out(f"Fender +0x1000 = 0x11 -> {s:#x}")
f1 = dump(FENDER, 0x4000, "fender(state 1)", f0)
dump(PERF, 0x400, "perf(state 1)", perf1)

if "--no-rgx" not in sys.argv and s & 0x1f == 1:
    out("reading RGX +0x18 (32-bit) ...")
    n = p.get_exc_count()
    v = p.read32(RGX + 0x18)
    out(f"RGX +0x18 = {v:#010x}" + (" <fault>" if p.get_exc_count() != n else ""))
p.set_exc_guard(GUARD.OFF)
out("end")
