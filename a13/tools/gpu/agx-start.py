#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""G4 (a13/docs/research/gpu.md): start the A13's GPU firmware from the Mac
through m1n1's proxy, with m1n1's own Python AGX driver.

    M1N1DEVICE=/dev/cu.usbmodemXXXX agx-start.py [--boot-pmp|--pmp-dvfs|--pmp-doorbell] [--fender] [--gpu-trig] [SECONDS]

The phone runs `a13/cascadia boot proxy` (m1n1 alone; iBoot has loaded the
GPU firmware: G3). Needs a13/build/m1n1 with patch 0007 (T8030 / G12) and the
venv a13/build/venv-gpu (construct, pyserial). Powers gfx-asc and sgx, boots
the RTKit firmware, builds initdata (CHIP_INFO 0x8030), sends it and DC_Init,
then runs the firmware's channels for SECONDS (default 10) and leaves a shell.
--boot-pmp: if iBoot loaded the PMP's firmware but left the PMP stopped, boot
its RTKit first (it carries out the GPU's power state changes), in this same
session: each proxyclient session starts m1n1's heap afresh and a second one
overwrites the first one's memory (the GPU's page tables).
--pmp-dvfs: --boot-pmp, then PM command 6 (a DVFS domain's state range) with
the full range for domains 0-6 (all of them), before the GPU starts.
--pmp-doorbell: --boot-pmp, then PM command 0xc (register a device's doorbell)
for the GPU: device id 0x81 (the PMP's own device table, entry "GPU"),
doorbell 0x30 (the GPU firmware rings 0x23bc34000 with 1); the ack is 0xd
with the same arguments, zeros if refused.
--fender: what iOS's G12::AGXFenderCommon does to Fender, the only Fender
registers the host touches: set +0x1010 bit 0 (Fender power management on)
before the GPU starts, acknowledge +0x1014 (interrupt status,
write-1-to-clear: sgx interrupts 1-3) whenever it is set, and after SECONDS
watch +0x1000 for the firmware's request to finish.
--gpu-trig: what iOS's PMGR driver does at start for every `*-trig` property
of /arm-io/pmgr (ApplePMGR setTrigger, AppleT8030PMGR register groups): the
GPU's DVFM trigger control, PMGR reg 0 + 0xfd000 (GPU_TRIG_CTL) |= gpu-trig
(0x20007). The trigger controls are printed either way.
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
args = [a for a in sys.argv[1:] if not a.startswith("--")]
pmp_dvfs = "--pmp-dvfs" in sys.argv[1:]
pmp_doorbell = "--pmp-doorbell" in sys.argv[1:]
boot_pmp = "--boot-pmp" in sys.argv[1:] or pmp_dvfs or pmp_doorbell
fender = "--fender" in sys.argv[1:]
gpu_trig = "--gpu-trig" in sys.argv[1:]
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
        from m1n1.hw.asc import R_INBOX1
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

        def pm(cmd, domain=0, device=0, arg1=0, arg2=0):
            return (0x0020000000000000 | (cmd & 0xff) << 44 | (domain & 0xf) << 40
                    | (device & 0xff) << 32 | (arg1 & 0xffff) << 16 | (arg2 & 0xffff))

        def pm_send(what, msg):
            # hw/asc.py: send(msg0, msg1 register), recv() -> (msg0, msg1)
            pmp_asc.send(msg, R_INBOX1(EP=0x20))
            end = time.time() + 0.5
            ack = None
            while time.time() < end and ack is None:
                m0, m1 = pmp_asc.recv()
                if m0 is not None:
                    ack = (m1.EP, m0)
            print(f"[agx-start] PMP {what}: sent {msg:#018x}, "
                  f"got {'nothing' if ack is None else f'EP {ack[0]:#x} {ack[1]:#018x}'}")

        # PM command 6 = a DVFS domain's allowed state range (ApplePMP::
        # _setDVFSStates): give every domain all its states; acks are 7
        if pmp_dvfs:
            for d in range(7):   # 7 domains; an 8th crashes the PMP
                pm_send(f"DVFS domain {d}", pm(6, d, 0, 0, 0xf))
        # PM command 0xc = register a doorbell (t8030pmp fn 0x100c004): arg1 a
        # device id from the PMP's table (15 entries at VA 0x1045050, 20E247:
        # GPU = 0x81), arg2 the doorbell (>= 0x20; page 0x23bc1c000 +
        # (n >> 3) * 0x4000, bit n & 7: the GPU rings 0x23bc34000 = 0x30)
        if pmp_doorbell:
            pm_send("GPU doorbell", pm(0xc, 0, 0, 0x81, 0x30))
        pdart.show_error()
except Exception as e:
    print(f"[agx-start] PMP: {e!r}")

FENDER = 0x204d00000


def fender_regs(when):
    # +0x1000 state (low 4 bits) | 0x10 request pending, +0x1010 host control,
    # +0x1014 interrupt status, +0x1018
    print(f"[agx-start] Fender {when}: " + " ".join(
        f"+{o:#x}={p.read32(FENDER + o):#x}" for o in (0x1000, 0x1010, 0x1014, 0x1018)))


# PMGR trigger controls (AppleT8030PMGR register groups 10, 11, 13 and 15,
# PMGR reg 0 + 0xdc000 + ...: ECPU, PCPU, ANE, GPU; T8030 has no PCPU1, ANE1,
# GPU1-3): iOS ORs in the pmgr node's ecpu-/pcpu-/ane-/gpu-trig at start
# (ApplePMGR 20E247 fn 0xfffffff0091b2eb4, called for each from fn
# 0xfffffff0091b07bc)
PMGR0 = u.adt["/arm-io/pmgr"].get_reg(0)[0]
TRIG_CTL = {"ECPU": 0xe8000, "PCPU": 0xe9000, "ANE": 0xf9000, "GPU": 0xfd000}


def trig_regs(when):
    print(f"[agx-start] PMGR TRIG_CTL {when}: " + " ".join(
        f"{n}={p.read32(PMGR0 + o):#x}" for n, o in TRIG_CTL.items()))


p.pmgr_adt_power_enable("/arm-io/gfx-asc")
p.pmgr_adt_power_enable("/arm-io/sgx")
fender_regs("before the GPU")
trig_regs("before the GPU")
if gpu_trig:
    trig = getattr(u.adt["/arm-io/pmgr"], "gpu-trig", 0x20007)
    p.set32(PMGR0 + TRIG_CTL["GPU"], trig)
    trig_regs(f"after GPU |= {trig:#x}")
if fender:
    p.write32(FENDER + 0x1010, p.read32(FENDER + 0x1010) | 1)
    fender_regs("after +0x1010 |= 1")


def fender_ack():
    # G12::AGXFenderCommon (AGXG12P 20E247, fn 0xfffffff00851f7fc): write
    # back +0x1014 & 7
    st = p.read32(FENDER + 0x1014) & 7
    if st:
        p.write32(FENDER + 0x1014, st)
        print(f"[agx-start] Fender interrupt {st:#x} acked, +0x1000="
              f"{p.read32(FENDER + 0x1000):#x}")

agx = AGX(u)
agx.verbose = 10
try:
    agx.start()
    end = time.time() + seconds
    while time.time() < end:
        agx.work()
        if pmp_asc:
            pmp_asc.work()
        if fender:
            fender_ack()
    fender_regs("after the GPU's request")
    trig_regs("after the GPU's request")
    print("[agx-start] DevCtrl read ptr:", agx.ch.devctrl.state.READ_PTR.val)
    if pmp_asc:
        pmp_asc.dart.show_error()
except Exception as e:
    print(f"[agx-start] stopped: {e!r}")

run_shell(globals(), msg="agx = the running driver; agx.work() to pump the firmware")
