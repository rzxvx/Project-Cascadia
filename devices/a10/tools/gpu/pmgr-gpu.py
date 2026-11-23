#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""G1 run 5 (devices/a10/docs/gpu.md): the GPU's DVFM side of PMGR under a
cold m1n1, read-only except PMGR GFX on and Fender's state request.

    ./cascadia a10 boot proxy
    M1N1DEVICE=/dev/cu.usbmodemXXXX1 build/proxyvenv/bin/python tools/gpu/pmgr-gpu.py [--rgx]

Run from devices/a10. From the iBSS (iBoot-8422.100.650, fn 0x28af8, which
fills the ADT's GPU perf-states): the GPU perf-state table (voltage domain
0x17) is PMGR 0x20e070000 + 0x10 * state, word 0: post-div bits 3:0,
pre-div bits 8:4, multiplier bits 20:12 (f = 24 MHz * mult / pre /
(post + 1)), voltage code bits 31:24; the SRAM rail's code is word 1 bits
7:0 (domain 0x1f). The SoC's table is 0x20e068018 + 0x10 * i, with its
state in the low bits of 0x20e068000.

1. PS registers of GFX (124) and DWI (the PMGR voltage interface to the PMU,
   0x20e200000, clock-gate 51), as m1n1 left them.
2. The GPU table (16 states) decoded, and PMGR 0x20e070000..+0x400 (nonzero
   words: whatever controls this table should be near it), the SoC's
   0x20e068000..+0x100 for comparison, DWI's first 0x200 bytes if DWI is on.
3. GFX on, Fender +0x1000 = 0x11; the same blocks again, changed words only.
4. PLL5 (0x20e014000, the GPU's PLL per the iBSS) and 0x20e070200, last.
5. Only with --rgx: one 32-bit read of RGX +0x18 (hung m1n1 in runs 1-4).
Every line is printed as it is read.
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
DWI = adt["/arm-io/dwi"].get_reg(0)[0]
pm = adt["/arm-io/pmgr"]
PMGR0 = pm.get_reg(0)[0]
GPUTAB = PMGR0 + 0x70000
SOC = PMGR0 + 0x68000


def out(s):
    print(f"[pmgr-gpu] {s}", flush=True)


def psaddr(dev_id):
    d = next(x for x in pm.devices if adt.pmgr_dev_get_id(x) == dev_id)
    return d.name, adt.pmgr_dev_get_addr(d)


def ps(dev_id):
    name, a = psaddr(dev_id)
    v = p.read32(a)
    out(f"PS {name} ({dev_id}) {a:#x} = {v:#010x} (target {v & 0xf:#x}, actual {(v >> 4) & 0xf:#x})")
    return v


def dump(base, size, tag, prev=None):
    vals = {}
    for off in range(0, size, 4):
        n = p.get_exc_count()
        v = p.read32(base + off)
        if p.get_exc_count() != n:
            v = None
        vals[off] = v
        if prev is None:
            if v:
                out(f"{tag} +{off:#05x} = {v:#010x}")
            elif v is None:
                out(f"{tag} +{off:#05x} <fault>")
        elif prev.get(off) != v:
            out(f"{tag} +{off:#05x} = {v!s:>10} (was {prev.get(off)!s})"
                if v is None or prev.get(off) is None else
                f"{tag} +{off:#05x} = {v:#010x} (was {prev.get(off):#010x})")
    out(f"{tag} done")
    return vals


def gputable():
    for s in range(16):
        w0 = p.read32(GPUTAB + 0x10 * s)
        w1 = p.read32(GPUTAB + 0x10 * s + 4)
        pre = (w0 >> 4) & 0x1f
        mhz = (24 * ((w0 >> 12) & 0x1ff) / pre / ((w0 & 0xf) + 1)) if pre else 0
        out(f"gpu state {s:2}: {w0:#010x} {w1:#010x}  {mhz:7.1f} MHz, volt code {w0 >> 24:#04x}, sram code {w1 & 0xff:#04x}")


p.set_exc_guard(GUARD.SKIP | GUARD.SILENT)
ps(124)
dwi_ps = None
for d in pm.devices:
    if d.name.startswith("DWI"):
        dwi_ps = ps(adt.pmgr_dev_get_id(d))
gputable()
t0 = dump(GPUTAB, 0x400, "pmgr+0x70000")
s0 = dump(SOC, 0x100, "pmgr+0x68000")
dwi_on = dwi_ps is not None and (dwi_ps >> 4) & 0xf == 0xf
w0 = dump(DWI, 0x200, "dwi") if dwi_on else None
if not dwi_on:
    out("DWI not reported on: its registers are not read")

p.pmgr_adt_power_enable("/arm-io/sgx")
p.pmgr_adt_power_enable("/arm-io/gfx-kf")
time.sleep(0.01)
v = ps(124)
if (v >> 4) & 0xf != 0xf:
    sys.exit("[pmgr-gpu] GFX not on; stop")
t1 = dump(GPUTAB, 0x400, "pmgr+0x70000(gfx on)", t0)
p.write32(FENDER + 0x1000, 0x11)
t = time.time()
while time.time() - t < 0.2:
    s = p.read32(FENDER + 0x1000)
    if s & 0x1f == 1:
        break
time.sleep(0.02)
out(f"Fender +0x1000 = 0x11 -> {s:#x}")
dump(GPUTAB, 0x400, "pmgr+0x70000(state 1)", t1)
dump(SOC, 0x100, "pmgr+0x68000(state 1)", s0)
if w0 is not None:
    dump(DWI, 0x200, "dwi(state 1)", w0)
ps(124)
# iBSS pmgr init (fn 0x28f68), right after writing the GPU table: PLL5
# (0x20e014000: +4 |= 1 << 24, +0 &= ~(1 << 27), wait for bit 25 to clear),
# then 0x20e070200 |= 1 << 31. Read last: an unpowered PLL may not answer.
out(f"pmgr+0x70200 = {p.read32(GPUTAB + 0x200):#010x}")
for off in (0x0, 0x4, 0x8, 0xc, 0x10, 0x14):
    out(f"PLL5 0x20e014000 +{off:#x} = {p.read32(PMGR0 + 0x14000 + off):#010x}")
if "--rgx" in sys.argv and s & 0x1f == 1:
    out("reading RGX +0x18 (32-bit) ...")
    n = p.get_exc_count()
    v = p.read32(RGX + 0x18)
    out(f"RGX +0x18 = {v:#010x}" + (" <fault>" if p.get_exc_count() != n else ""))
p.set_exc_guard(GUARD.OFF)
out("end")
