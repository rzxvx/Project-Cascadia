#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""G1 run 8 (devices/a10/docs/gpu.md): does the Rogue core answer once the
gfx-kf coprocessor runs -- any code, not the GFX firmware?

    ./cascadia a10 boot proxy
    M1N1DEVICE=/dev/cu.usbmodemXXXX1 build/proxyvenv/bin/python tools/gpu/gpu-kfrun.py [--no-run] [--f0]

Run from devices/a10. Runs 1-7 replayed every Fender write the kext and the
GFX firmware make on the way to the core (+0x10 = 7, +0x1000 = 0x11, +0x2058
reset) and RGX still hung; iOS only ever touches the core while gfx-kf runs
(Fender +0x2048 bit 0 = 1). Here gfx-kf runs a harmless image instead of the
firmware: 16 KiB of ARM `b .` (0xeafffffe), every vector a loop on itself.

The window and run bit as the 22H355 kernelcache programs them
(AppleA7IOP-AKFWrap-v2, kext text 0xfffffff006c165e0 / 0xfffffff006c16680):
Fender +0x2014 / +0x2018 = IOP address (0), +0x201c = length & ~0xfff, +0x2020
= 0, +0x200c / +0x2010 = physical address; run = Fender +0x2048 bit 0.

1. PMGR GFX on; gfx-kf's IOP registers +0x00..+0x50 printed (read-only).
2. The loop image in m1n1's heap (16 KiB aligned), cleaned to memory.
3. Fender window, then (unless --no-run) +0x2048 |= 1; Fender printed.
4. Fender +0x10 = 7, with --f0 +0x0 = 0x40000000 (run 9), +0x1000 = 0x11 (wait), +0x2058 = 1 (wait), 10 ms.
5. RGX +0x18 / +0x20 (32-bit).
6. Run bit cleared again (only reached if 5 returned).
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
IOP = adt["/arm-io/gfx-kf"].get_reg(0)[0]
pm = adt["/arm-io/pmgr"]
SIZE = 0x4000


def out(s):
    print(f"[kfrun] {s}", flush=True)


def rd(a):
    n = p.get_exc_count()
    v = p.read32(a)
    return None if p.get_exc_count() != n else v


def fender(tag):
    offs = [0x0, 0x4, 0x8, 0x10, 0x14, 0x1000, 0x2000, 0x200c, 0x2010, 0x2014, 0x2018, 0x201c, 0x2020,
            0x2040, 0x2048, 0x2050, 0x2054, 0x2058]
    out(f"Fender {tag}: " + " ".join(f"+{o:#x}={rd(FENDER + o)!s}" if rd(FENDER + o) is None
                                     else f"+{o:#x}={rd(FENDER + o):#x}" for o in offs))


def iop(tag):
    out(f"IOP {tag}: " + " ".join(f"+{o:#x}={rd(IOP + o):#x}" for o in range(0, 0x54, 4)
                                  if rd(IOP + o)))


p.set_exc_guard(GUARD.SKIP | GUARD.SILENT)
gfx = next(d for d in pm.devices if adt.pmgr_dev_get_id(d) == 124)
PS = adt.pmgr_dev_get_addr(gfx)
p.pmgr_adt_power_enable("/arm-io/sgx")
p.pmgr_adt_power_enable("/arm-io/gfx-kf")
time.sleep(0.01)
v = p.read32(PS)
out(f"PMGR GFX = {v:#010x}")
if (v >> 4) & 0xf != 0xf:
    sys.exit("[kfrun] GFX not on; stop")
iop("before")
fender("before")

buf = u.heap.memalign(SIZE, SIZE)
iface.writemem(buf, struct.pack("<I", 0xeafffffe) * (SIZE // 4))
p.dc_cvac(buf, SIZE)
out(f"loop image at {buf:#x}, first word {p.read32(buf):#010x}")

p.write32(FENDER + 0x2014, 0)
p.write32(FENDER + 0x2018, 0)
p.write32(FENDER + 0x201c, SIZE & ~0xfff)
p.write32(FENDER + 0x2020, 0)
p.write32(FENDER + 0x200c, buf & 0xffffffff)
p.write32(FENDER + 0x2010, buf >> 32)
if "--no-run" not in sys.argv:
    p.write32(FENDER + 0x2048, (p.read32(FENDER + 0x2048) & ~1) | 1)
    time.sleep(0.01)
fender("window + run")
iop("after run")

p.write32(FENDER + 0x10, 7)
# --f0: Fender +0x0 = 0x40000000 before the state request (written by nothing
# decoded so far; on the A9's Fender it is part of waking the core).
if "--f0" in sys.argv:
    out(f"Fender +0x0 before: {p.read32(FENDER):#010x}")
    p.write32(FENDER, 0x40000000)
    time.sleep(0.001)
    out(f"Fender +0x0 after write 0x40000000: {p.read32(FENDER):#010x}")
p.write32(FENDER + 0x1000, 0x11)
t = time.time()
while time.time() - t < 0.2:
    s = p.read32(FENDER + 0x1000)
    if s & 0x1f == 1:
        break
out(f"Fender +0x1000 = 0x11 -> {s:#x}")
p.write32(FENDER + 0x2058, 1)
t = time.time()
while time.time() - t < 0.2 and p.read32(FENDER + 0x2058) & 1:
    pass
time.sleep(0.01)
fender("state 1 + reset")
if s & 0x1f != 1:
    sys.exit("[kfrun] no state 1; not reading RGX")
for off in (0x18, 0x20):
    out(f"reading RGX +{off:#x} ...")
    r = rd(RGX + off)
    out(f"RGX +{off:#x} = " + ("<fault>" if r is None else f"{r:#010x}"))
p.write32(FENDER + 0x2048, p.read32(FENDER + 0x2048) & ~1)
p.set_exc_guard(GUARD.OFF)
out("end")
