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
its RTKit first (it carries out the GPU's power state changes), in this same
session: each proxyclient session starts m1n1's heap afresh and a second one
overwrites the first one's memory (the GPU's page tables).
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

pmp_asc = None
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
        import struct
        from m1n1.fw.asc import StandardASC
        from m1n1.hw.dart import DART
        p.pmgr_adt_power_enable("/arm-io/pmp")
        p.pmgr_adt_power_enable("/arm-io/dart-pmp")

        # the PMP's RTKit boot arguments as iBoot left them in its SRAM
        # (image VA 0x1000000 at region-base; table at VA 0x1032800 in 20E247)
        sram = nub.region_base
        tab = iface.readmem(sram + 0x32800, 0x2d0)
        o, bootargs = 0, {}
        while o < len(tab) - 8:
            tag = tab[o:o + 4][::-1].decode("latin1")
            sz = struct.unpack_from("<I", tab, o + 4)[0]
            if sz not in (1, 2, 4, 8):
                break
            bootargs[tag] = int.from_bytes(tab[o + 8:o + 8 + sz], "little")
            o += 8 + sz
        print("[agx-start] PMP boot args:", " ".join(f"{k}={v:#x}" for k, v in bootargs.items()))

        # its DART (SID 0 translates, SID 15 bypass) and m1n1's PMP driver
        # (Asahi's v1 protocol, fw/pmp.py): it lays the PMP's register windows
        # (pmp reg[3..]) out from 0xc0000000 in that DART, hands the table
        # over in a shared buffer (Configure), then Init1/Init2
        from m1n1.fw import pmp as fwpmp
        from m1n1.fw.asc.base import ASCTimeout

        class PMPEndpoint(fwpmp.PMPEndpoint):
            # log what the host sends (m1n1 logs only what it receives)
            def send(self, msg):
                print(f"[pmp] > {msg}")
                return super().send(msg)

        class PMPClient(fwpmp.PMPClient):
            ENDPOINTS = {0x20: PMPEndpoint}

        # bound the endless "while not acked: work()" loops of fw/pmp.py
        def patched_work(self):
            if getattr(self, "_deadline", None) and time.time() > self._deadline:
                raise ASCTimeout("PMP: no ack within 3 s")
            return fwpmp.PMPClient.work(self)
        PMPClient.work = patched_work
        pdart = DART.from_adt(u, "/arm-io/dart-pmp")
        pdart.initialize()
        pmp_asc = PMPClient(u, pbase, dart=pdart)
        pmp_asc.verbose = 3
        pmp_asc.mgmt.verbose = 1
        try:
            pmp_asc.start()
        except Exception as e:
            print(f"[agx-start] PMP boot: {e!r} (going on)")
        try:
            pmp_asc._deadline = time.time() + 5
            pmp_asc.start_ep(0x20)
            pmp_asc.work_for(3)
        except Exception as e:
            print(f"[agx-start] PMP endpoint 0x20: {e!r}")
        pmp_asc._deadline = None
        print(f"[agx-start] PMP: CPU_CONTROL {p.read32(pbase + 0x44):#x} "
              f"CPU_STATUS {p.read32(pbase + 0x48):#x}")
        pdart.show_error()
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
        if pmp_asc:
            pmp_asc.work()
    print("[agx-start] Fender+0x1000:", hex(p.read32(0x204d01000)),
          "DevCtrl read ptr:", agx.ch.devctrl.state.READ_PTR.val)
    if pmp_asc:
        pmp_asc.dart.show_error()
except Exception as e:
    print(f"[agx-start] stopped: {e!r}")

run_shell(globals(), msg="agx = the running driver; agx.work() to pump the firmware")
