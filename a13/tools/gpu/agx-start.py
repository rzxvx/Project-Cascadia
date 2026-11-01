#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""G4 (a13/docs/research/gpu.md): start the A13's GPU firmware from the Mac
through m1n1's proxy, with m1n1's own Python AGX driver.

    M1N1DEVICE=/dev/cu.usbmodemXXXX agx-start.py [--boot-pmp] [SECONDS]

The phone runs `a13/cascadia boot proxy` (m1n1 alone; iBoot has loaded the
GPU firmware: G3). Needs a13/build/m1n1 with patch 0007 (T8030 / G12) and the
venv a13/build/venv-gpu (construct, pyserial). Powers gfx-asc and sgx, boots
the RTKit firmware, builds initdata (CHIP_INFO 0x8030), sends it and DC_Init,
then runs the firmware's channels for SECONDS (default 10) and leaves a shell.
--boot-pmp: if iBoot loaded the PMP's firmware but left the PMP stopped, boot
its RTKit first (it carries out the GPU's power state changes).
"""
import os
import sys
import time

PROXY = os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../build/m1n1/proxyclient")
sys.path.insert(0, os.environ.get("M1N1_PROXYCLIENT", PROXY))

from m1n1.setup import *  # noqa: E402,F401,F403  (p, u, iface)
from m1n1.constructutils import Ver  # noqa: E402

Ver.set_version(u)
print(f"[agx-start] firmware interface {Ver._version}")

from m1n1.shell import run_shell  # noqa: E402
from m1n1.agx import AGX  # noqa: E402

args = [a for a in sys.argv[1:] if a != "--boot-pmp"]
boot_pmp = "--boot-pmp" in sys.argv[1:]
seconds = float(args[0]) if args else 10.0

# the power-management coprocessor carries out the GPU's power state changes
# (the GPU firmware rings it, then waits for Fender+0x1000): is it there?
try:
    pmp = u.adt["/arm-io/pmp"]
    nub = u.adt["/arm-io/pmp/iop-pmp-nub"]
    pbase = pmp.get_reg(0)[0]
    print(f"[agx-start] PMP @ {pbase:#x}: pre-loaded {getattr(nub, 'pre_loaded', None)}, "
          f"CPU_CONTROL {p.read32(pbase + 0x44):#x} CPU_STATUS {p.read32(pbase + 0x48):#x}")
    if boot_pmp and getattr(nub, "pre_loaded", 0) and not p.read32(pbase + 0x44) & 0x10:
        from m1n1.fw.asc import StandardASC
        p.pmgr_adt_power_enable("/arm-io/pmp")
        pmp_asc = StandardASC(u, pbase)
        pmp_asc.verbose = 1
        pmp_asc.start()
        print(f"[agx-start] PMP booted: CPU_CONTROL {p.read32(pbase + 0x44):#x}")
except Exception as e:
    print(f"[agx-start] PMP: {e!r}")

p.pmgr_adt_power_enable("/arm-io/gfx-asc")
p.pmgr_adt_power_enable("/arm-io/sgx")

agx = AGX(u)
agx.verbose = 10
try:
    agx.start()
    end = time.time() + seconds
    while time.time() < end:
        agx.work()
    print("[agx-start] Fender+0x1000:", hex(p.read32(0x204d01000)),
          "DevCtrl read ptr:", agx.ch.devctrl.state.READ_PTR.val)
except Exception as e:
    print(f"[agx-start] stopped: {e!r}")

run_shell(globals(), msg="agx = the running driver; agx.work() to pump the firmware")
