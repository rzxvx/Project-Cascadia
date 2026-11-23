#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""G1 (devices/a10/docs/gpu.md): power the iPad 7's GPU from the Mac through
m1n1's proxy and read the Rogue core's identity registers.

    ./cascadia a10 boot proxy
    M1N1DEVICE=/dev/cu.usbmodemXXXX1 build/proxyvenv/bin/python tools/gpu/gpu-id.py [--req] [--arm-reset] [--no-core] [--no-reset] [--keep] [--rgx-all]

Run from devices/a10. Read-only up to step 3; the only writes are the PMGR
power state of GFX (m1n1's own pmgr_adt_power_enable) and Fender's
power-state request.

1. Prints the GPU's ADT facts as the live device tree has them (sgx,
   gfx-kf, PMGR GFX power state).
2. Turns PMGR GFX (id 124, the one real domain behind sgx's and gfx-kf's
   power-gates 148/149/150) on and checks it reports "on".
3. Reads a handful of Fender words (sgx reg 1 = 0x201d00000). Fender is read
   only while GFX is on: on the A13 a Fender read with GFX off hung m1n1.
4. Unless --no-core: asks Fender for GPU power state 1 (+0x1000 = 0x11, state
   + request pending) and waits up to 200 ms for +0x1000 & 0x1f == 1. Only
   then reads the Rogue registers (sgx reg 0 = 0x201000000). Run 2
   (2026-11-23): Fender acknowledges, yet the read of +0x18 still hangs m1n1
   (a DFU reset follows) -- something more is needed first (docs/gpu.md, G1).
4b. Unless --no-reset: the firmware's reset step, Fender +0x2058 = 1 and wait
   for bit 0 to clear (run 4 on; runs 1-3 had no reset and hung on RGX).
5. Unless --keep: asks Fender for state 0 again (+0x1000 = 0x10).

Every Rogue read is a 32-bit read with m1n1's exception guard on (a
synchronous abort is skipped and reported, not fatal; an SError still is).
"""
import os
import sys
import time

PROXY = os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../build/m1n1/proxyclient")
sys.path.insert(0, os.environ.get("M1N1_PROXYCLIENT", PROXY))

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gpusetup import p, u, iface, GUARD  # noqa: E402  (m1n1 proxy, or GPU_BACKEND=linux)

no_core = "--no-core" in sys.argv
keep = "--keep" in sys.argv

adt = u.adt
sgx = adt["/arm-io/sgx"]
kf = adt["/arm-io/gfx-kf"]
RGX = sgx.get_reg(0)[0]
# RGX_ALIAS=0xf000000000: read the core through one of m1n1's MMIO aliases
# (0xc0.. Device-GRE, 0xd0.. nGRE, 0xe0.. nGnRnE, 0xf0.. nGnRE) instead of the
# identity mapping.
RGX += int(os.environ.get("RGX_ALIAS", "0"), 0)
FENDER = sgx.get_reg(1)[0]
KF = kf.get_reg(0)[0]

print(f"[gpu-id] chip 0x{adt['/chosen'].chip_id:x}, board {adt['/chosen'].board_id:#x}")
print(f"[gpu-id] sgx: rgx {RGX:#x} +{sgx.get_reg(0)[1]:#x}, fender {FENDER:#x} +{sgx.get_reg(1)[1]:#x}, "
      f"irqs {list(sgx.interrupts)}, power-gates {list(sgx.power_gates)}, clock-ids {list(sgx.clock_ids)}")
print(f"[gpu-id] gfx-kf: iop {KF:#x} +{kf.get_reg(0)[1]:#x}, irqs {list(kf.interrupts)}, "
      f"power-gates {list(kf.power_gates)}, iop-version {kf.iop_version}")

pm = adt["/arm-io/pmgr"]
gfx = next(d for d in pm.devices if adt.pmgr_dev_get_id(d) == 124)
PS_GFX = adt.pmgr_dev_get_addr(gfx)


def ps(tag):
    v = p.read32(PS_GFX)
    print(f"[gpu-id] {tag}: PMGR {gfx.name} {PS_GFX:#x} = {v:#010x} (target {v & 0xf:#x}, actual {(v >> 4) & 0xf:#x})")
    return v


before = ps("before")
p.pmgr_adt_power_enable("/arm-io/sgx")
p.pmgr_adt_power_enable("/arm-io/gfx-kf")
time.sleep(0.01)
after = ps("after pmgr_adt_power_enable")
if (after >> 4) & 0xf != 0xf:
    sys.exit("[gpu-id] GFX did not report on; stopping before any Fender read")

FENDER_WORDS = [0x0, 0x4, 0x8, 0xc, 0x10, 0x14, 0x1000, 0x1004, 0x1010, 0x1014, 0x1018,
                0x2000, 0x2048, 0x2050, 0x2054, 0x2058]


def fender(tag):
    vals = {o: p.read32(FENDER + o) for o in FENDER_WORDS}
    print(f"[gpu-id] Fender {tag}: " + " ".join(f"+{o:#x}={v:#x}" for o, v in vals.items()))
    return vals


p.set_exc_guard(GUARD.SKIP | GUARD.SILENT)
fender("with GFX on")
if no_core:
    sys.exit(0)

# Kingfisher (the kext's G9P Fender class) vtable +0x50: Fender +0x10 |= 1 << arg
# for arg 0, 1, 2 -- the AP's request bits; +0x80 tests bit 0. Runs 1-6 left
# +0x10 at 0. With --req: set them one at a time, logging Fender after each.
if "--req" in sys.argv:
    for bit in (1, 2, 4):
        p.write32(FENDER + 0x10, p.read32(FENDER + 0x10) | bit)
        time.sleep(0.005)
        fender(f"after +0x10 |= {bit}")

# --arm-reset: the reset request armed while the core is still off, then the
# power request (the order the GFX firmware's path leaves Fender in when it
# writes +0x2058 = 1 before any state is up); wait for +0x2058 to clear after.
if "--arm-reset" in sys.argv:
    p.write32(FENDER + 0x2058, 1)
    time.sleep(0.001)
    fender("reset armed (state 0)")

p.write32(FENDER + 0x1000, 0x11)
t0 = time.time()
state = None
while time.time() - t0 < 0.2:
    state = p.read32(FENDER + 0x1000)
    if state & 0x1f == 1:
        break
dt = (time.time() - t0) * 1000
print(f"[gpu-id] Fender +0x1000 = 0x11 -> {state:#x} after {dt:.1f} ms")
fender("after the request")
if state & 0x1f != 1:
    sys.exit("[gpu-id] Fender did not acknowledge state 1; not touching the Rogue registers")

# The firmware's power-up (fw 0x6ce4, "AGFHALExecuteResetSequence"): after the
# perf-state request it writes Fender +0x2058 = 1 and polls bit 0 back to 0
# (timeout 250 us), then runs the host's init sequence. G1 runs 1-3 skipped it.
if "--arm-reset" in sys.argv:
    t0 = time.time()
    while time.time() - t0 < 0.2 and p.read32(FENDER + 0x2058) & 1:
        pass
    r = p.read32(FENDER + 0x2058)
    print(f"[gpu-id] armed reset: +0x2058 = {r:#x} after {(time.time() - t0) * 1000:.1f} ms")
    time.sleep(0.01)
    fender("after the armed reset")
    if r & 1:
        sys.exit("[gpu-id] +0x2058 did not clear after power-up; not touching the Rogue registers")
elif "--no-reset" not in sys.argv:
    p.write32(FENDER + 0x2058, 1)
    t0 = time.time()
    while time.time() - t0 < 0.2:
        r = p.read32(FENDER + 0x2058)
        if r & 1 == 0:
            break
    print(f"[gpu-id] Fender +0x2058 = 1 -> {r:#x} after {(time.time() - t0) * 1000:.1f} ms")
    time.sleep(0.01)  # the kext's IOSleep(10) after the power request
    fender("after the reset")
    if r & 1:
        sys.exit("[gpu-id] Fender +0x2058 did not clear; not touching the Rogue registers")

# RGX_DELAY=SECONDS: wait before the first core read (under Linux the read
# that worked came hundreds of ms after the power-up; here it was ~10 ms).
time.sleep(float(os.environ.get("RGX_DELAY", "0")))
if os.environ.get("GUARD") == "off":
    p.set_exc_guard(GUARD.OFF)       # let m1n1 print any exception on the UART
rgx = {}
# +0x18 / +0x20 first (the identity registers); the rest only with --rgx-all:
# run 1 (2026-11-23) lost m1n1 on the read of +0x00.
offs = [0x18, 0x20] + ([o for o in range(0, 0x40, 8) if o not in (0x18, 0x20)] if "--rgx-all" in sys.argv else [])
for off in offs:
    print(f"[gpu-id] reading RGX +{off:#04x} ...", flush=True)
    before_exc = p.get_exc_count()
    v = p.read32(RGX + off)  # 32-bit, as the ARM32 firmware reads
    faulted = p.get_exc_count() != before_exc
    rgx[off] = None if faulted else v
    print(f"[gpu-id] RGX +{off:#04x} = " + ("<fault>" if faulted else f"{v:#010x}"))

core_id = rgx.get(0x18)
core_rev = rgx.get(0x20)
if core_id is not None:
    print(f"[gpu-id] CORE_ID (+0x18) = {core_id & 0xffffffff:#010x}: "
          f"id {(core_id >> 16) & 0xffff:#06x}, config {core_id & 0xffff:#06x}")
if core_rev is not None:
    print(f"[gpu-id] CORE_REVISION (+0x20) = {core_rev & 0xffffffff:#010x}: "
          f"major {(core_rev >> 16) & 0xff}, minor {(core_rev >> 8) & 0xff}, maint {core_rev & 0xff}")

if not keep:
    p.write32(FENDER + 0x1000, 0x10)
    t0 = time.time()
    while time.time() - t0 < 0.2:
        state = p.read32(FENDER + 0x1000)
        if state & 0x1f == 0:
            break
    print(f"[gpu-id] Fender +0x1000 = 0x10 -> {state:#x}")
p.set_exc_guard(GUARD.OFF)
