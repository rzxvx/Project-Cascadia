#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""G4 (a13/docs/research/gpu.md): start the A13's GPU firmware from the Mac
through m1n1's proxy, with m1n1's own Python AGX driver.

    M1N1DEVICE=/dev/cu.usbmodemXXXX agx-start.py [--boot-pmp|--pmp-dvfs|--pmp-doorbell] [--fender] [--gpu-trig] [--gfx-auto] [--poke] [--ios-pmgr [--fabric-auto]] [--fender-ios] [--fender-dump] [--dump-regions] [--pll-probe] [--try ADDR|=MASK|ADDR=VAL ...] [--pmp-vote DOM:LVL,...] [--fender-init] [--pmu-dump] [SECONDS]

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
--gfx-auto: set AUTO_ENABLE (bit 28) in PMGR's GFX power-state register
(0x23b080390), as iOS has it: there the hardware moves GFX between states 0,
4 and 0xf by itself, and Fender's 0x11 -> 0x01 takes ~16 us with +0x1010 = 0
(kmem mmiowatch, 2026-11-02). Printed either way.
--poke: keep m1n1's M1 "early init" (AGX.poke_sgx: sgx+0xd14000 = Fender
+0x14000 = 0x70001, reads back 0x30001 here). Skipped by default: iOS leaves
+0x14000 at 0 (+0x14004 = 1). Fender's +0x1000 and +0x14000 pages (nonzero
words) are printed before the GPU starts and after its request, to compare
with iOS's (gpu.md).
--ios-pmgr: PMGR as the iOS diff (gpu.md, a13/build/gpu/pmgr-diff) has it:
before the GPU starts, the pwrgate block's first word 0x23b09c000 |=
0x80000000 (0 under m1n1) and GFX AUTO_ENABLE. With --fabric-auto also,
if Fender is still 0x11 after SECONDS, AUTO_ENABLE on the fabric/memory
domains iOS runs in auto (MCC, DCS0-3, SMX, IMX, RMX, MMX, SBR, PMS_BUSIF,
PMS, PMS_SRAM, PMP) while the request is pending, then SECONDS more: it did
not move Fender, and m1n1's USB was gone after the session (2026-11-02).
--fender-ios: the two Fender config words iOS has and m1n1 does not (whole-
Fender diff, gpu.md): +0x4200 = 0x10000 and +0x409c |= 0x100, before the GPU
starts; +0x1018 (the power sequencer's step, stuck at 0x10 here) is printed.
--fender-dump: all of Fender (0x20000) into a13/build/gpu/pmgr-diff/
proxy-fender-{before,after}.txt (m1n1's USB was gone after the session that
did this, maybe by chance).
--dump-regions: the blocks of pmgr-regions.txt (as pmgr-dump.py) into
a13/build/gpu/pmgr-diff/proxy-regions-{before,after}.txt.
--pll-probe: read the PMGR PLL words one at a time: PLL0-3 (0x23b000000 +
0x4000 * n, iOS: 0x80c80300, 0x80590100, 0x80320181, 0x803d0101) and then
0x23b010000/14000/18000 (PLL4-6?), before the GPU and last of all after its
request. On idle iOS a read from 0x23b010000 took the phone down: a PLL that
is off may not answer, so this may end the proxy too.
--try ADDR|=MASK or ADDR=VAL (repeatable): with the GPU's request pending
(Fender still 0x11), OR each MASK into ADDR (or write VAL) in turn, pump the firmware for 1 s and print
Fender after each; stops at the first that moves it. For hypotheses from
the iOS diffs without a fresh proxy boot apiece.
--pmp-vote DOM:LVL,...: (implies --boot-pmp) PM command 0xf for the GPU
(device index 3 in the PMP's table) before the GPU starts: a minimum level
LVL (0-2, the PMP's domains have 3) on rail/perf domain DOM (0 SOC, 1 DCS,
2 DISP, 3 AVE; the GPU's entry takes votes on SOC and DCS only). M3 Pro's
GPU needs the same kind of PMP vote (0x0020f50100000000) before its
firmware can start (aurora-silicon/linux#358). No reply in this PMP version.
--fender-init: what iOS's AGX driver does itself at init (AGXG12P 20E247,
fn 0xfffffff0084f1b54, at 0xfffffff0084f2340): Fender +0x10000 = 0xffff0000,
+0x0 = 0x40000000, then +0x1000 = 0x11 and wait for bit 4 (pending) to
clear -- the host powers the GPU up once before its firmware runs. Done here
before the firmware starts, printing +0x1000/+0x1018 while it waits (1 s).
--pmu-dump: the PMU's registers (spmi0, slave from its ADT reg, 0x0000-0xffff
by 8-byte extended-long reads; addresses that do not answer are skipped)
before the GPU starts and with its request pending, into a13/build/gpu/
pmgr-diff/proxy-pmu-{before,after}.bin, and the bytes that changed: if the
PMGR asked the PMU for the GPU's voltage, a buck's voltage register moves.
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
tries = []
argv = sys.argv[1:]
while "--try" in argv:
    i = argv.index("--try")
    if "|=" in argv[i + 1]:
        a, m = argv[i + 1].split("|=")
        tries.append((int(a, 0), int(m, 0), True))
    else:
        a, m = argv[i + 1].split("=")
        tries.append((int(a, 0), int(m, 0), False))
    del argv[i:i + 2]
votes = []
if "--pmp-vote" in argv:
    i = argv.index("--pmp-vote")
    votes = [tuple(int(x, 0) for x in v.split(":")) for v in argv[i + 1].split(",")]
    del argv[i:i + 2]
args = [a for a in argv if not a.startswith("--")]
pmp_dvfs = "--pmp-dvfs" in sys.argv[1:]
pmp_doorbell = "--pmp-doorbell" in sys.argv[1:]
boot_pmp = "--boot-pmp" in sys.argv[1:] or pmp_dvfs or pmp_doorbell or bool(votes)
fender = "--fender" in sys.argv[1:]
gpu_trig = "--gpu-trig" in sys.argv[1:]
gfx_auto = "--gfx-auto" in sys.argv[1:]
poke = "--poke" in sys.argv[1:]
ios_pmgr = "--ios-pmgr" in sys.argv[1:]
fabric = "--fabric-auto" in sys.argv[1:]
fender_ios = "--fender-ios" in sys.argv[1:]
fender_dump = "--fender-dump" in sys.argv[1:]
dump_regions = "--dump-regions" in sys.argv[1:]
pll_probe = "--pll-probe" in sys.argv[1:]
fender_init = "--fender-init" in sys.argv[1:]
pmu_dump = "--pmu-dump" in sys.argv[1:]
gfx_auto = gfx_auto or ios_pmgr
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
        # PM command 0xf = a device's vote for a minimum level on a domain
        # (t8030pmp fn 0x100c228: domain = bits 43:40 < 4, device = the
        # table index in bits 39:32, arg1 = level below the domain's count;
        # recomputes the domain's level and applies it, fn 0x100e134)
        for d, lvl in votes:
            pm_send(f"GPU vote domain {d} level {lvl}", pm(0xf, d, 3, lvl, 0))
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


# power-state registers (bit 28 AUTO_ENABLE, 27:24 auto state, 7:4 actual,
# 3:0 target); iOS idle: GFX 0x1400034f / 0x1000030f / 0x1f0003ff (flickers),
# PMP 0x1f0f03ff
PS = {"GFX": 0x23b080390, "PMP": 0x23b080368}


def trig_regs(when):
    print(f"[agx-start] PMGR TRIG_CTL {when}: " + " ".join(
        f"{n}={p.read32(PMGR0 + o):#x}" for n, o in TRIG_CTL.items()) + "; ps " + " ".join(
        f"{n}={p.read32(a):#x}" for n, a in PS.items()))


p.pmgr_adt_power_enable("/arm-io/gfx-asc")
p.pmgr_adt_power_enable("/arm-io/sgx")
fender_regs("before the GPU")
trig_regs("before the GPU")
if gpu_trig:
    trig = getattr(u.adt["/arm-io/pmgr"], "gpu-trig", 0x20007)
    p.set32(PMGR0 + TRIG_CTL["GPU"], trig)
    trig_regs(f"after GPU |= {trig:#x}")
if gfx_auto:
    p.set32(PS["GFX"], 1 << 28)
    trig_regs("after GFX AUTO_ENABLE")
PWRGATE0 = 0x23b09c000
if ios_pmgr:
    print(f"[agx-start] pwrgate +0: {p.read32(PWRGATE0):#x}")
    p.set32(PWRGATE0, 0x80000000)
    print(f"[agx-start] pwrgate +0 after |= 0x80000000: {p.read32(PWRGATE0):#x}")

# domains iOS runs with AUTO_ENABLE (pmgr-diff), by name
FABRIC_AUTO = ("MCC", "DCS0", "DCS1", "DCS2", "DCS3", "SMX", "IMX", "RMX", "MMX",
               "SBR", "PMS_BUSIF", "PMS", "PMS_SRAM", "PMP")


def fabric_auto():
    for d in u.adt["/arm-io/pmgr"].devices:
        if d.name in FABRIC_AUTO and not d.flags.no_ps:
            a = u.adt.pmgr_dev_get_addr(d)
            p.set32(a, 1 << 28)
            print(f"[agx-start] {d.name} ps {a:#x} -> {p.read32(a):#x}")
if fender:
    p.write32(FENDER + 0x1010, p.read32(FENDER + 0x1010) | 1)
    fender_regs("after +0x1010 |= 1")


def fender_pages(when):
    for base, n in ((0x1000, 0x40), (0x14000, 0x80)):
        words = [(o, p.read32(FENDER + o)) for o in range(base, base + n * 4, 4)]
        print(f"[agx-start] Fender +{base:#x} page {when}: " + (" ".join(
            f"+{o:#x}={v:#x}" for o, v in words if v) or "all 0"))


fender_pages("before the GPU")

DUMPS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../build/gpu/pmgr-diff")


def fender_full(tag):
    # all of Fender (0x20000) in kmem mmio's format, for diffing with iOS's
    # (a13/build/gpu/pmgr-diff/ios-fender-{lo,hi}.txt)
    os.makedirs(DUMPS, exist_ok=True)
    fn = os.path.join(DUMPS, f"proxy-fender-{tag}.txt")
    with open(fn, "w") as f:
        for o in range(0, 0x20000, 4):
            v = p.read32(FENDER + o)
            if v:
                f.write(f"0x{FENDER + o:x} = 0x{v:08x}\n")
    print(f"[agx-start] Fender dumped to {fn}")




def regions(tag):
    fn = os.path.join(DUMPS, f"proxy-regions-{tag}.txt")
    os.makedirs(DUMPS, exist_ok=True)
    with open(fn, "w") as f:
        for line in open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "pmgr-regions.txt")):
            line = line.split("#")[0].split()
            if not line:
                continue
            pa, ln = int(line[0], 0), int(line[1], 0)
            for o in range(0, ln, 4):
                v = p.read32(pa + o)
                if v:
                    f.write(f"0x{pa + o:x} = 0x{v:08x}\n")
    print(f"[agx-start] regions dumped to {fn}")




def plls(when):
    for n in range(7):
        a = 0x23b000000 + 0x4000 * n
        print(f"[agx-start] PLL{n} {when} @ {a:#x}: " + " ".join(
            f"{p.read32(a + o):#x}" for o in (0, 4, 8)), flush=True)


if pll_probe:
    plls("before the GPU")
if fender_dump:
    fender_full("before")
if dump_regions:
    regions("before")
if fender_ios:
    p.write32(FENDER + 0x4200, 0x10000)
    p.set32(FENDER + 0x409c, 0x100)
    print(f"[agx-start] Fender +0x4200={p.read32(FENDER + 0x4200):#x} "
          f"+0x409c={p.read32(FENDER + 0x409c):#x}")


def fender_ack():
    # G12::AGXFenderCommon (AGXG12P 20E247, fn 0xfffffff00851f7fc): write
    # back +0x1014 & 7
    st = p.read32(FENDER + 0x1014) & 7
    if st:
        p.write32(FENDER + 0x1014, st)
        print(f"[agx-start] Fender interrupt {st:#x} acked, +0x1000="
              f"{p.read32(FENDER + 0x1000):#x}")

PMU_SNAP = {}


def pmu_snapshot(tag):
    from m1n1.hw.spmi import SPMI
    node = u.adt["/arm-io/spmi0/spmi-pmu"]
    slave = node.reg[0] if isinstance(node.reg[0], int) else node.reg[0].addr
    slave = int(slave) & 0xf
    spmi = SPMI(u, "/arm-io/spmi0")
    img, bad = bytearray(0x10000), 0
    t0 = time.time()
    for a in range(0, 0x10000, 8):
        try:
            img[a:a + 8] = spmi.read_extl(slave, a, 8)
        except Exception:
            bad += 1
    os.makedirs(DUMPS, exist_ok=True)
    open(os.path.join(DUMPS, f"proxy-pmu-{tag}.bin"), "wb").write(img)
    PMU_SNAP[tag] = img
    print(f"[agx-start] PMU (slave {slave:#x}) dumped {tag}: {bad} of 8192 reads failed, "
          f"{time.time() - t0:.0f} s", flush=True)


if pmu_dump:
    pmu_snapshot("before")

if fender_init:
    # AGXG12P 20E247 fn 0xfffffff0084f1b54: unless a platform flag says
    # otherwise, these two, then the power request, then a busy-wait
    p.write32(FENDER + 0x10000, 0xffff0000)
    p.write32(FENDER + 0x0, 0x40000000)
    fender_regs("after +0x10000/+0x0 init writes")
    p.write32(FENDER + 0x1000, 0x11)
    end = time.time() + 1
    while time.time() < end and p.read32(FENDER + 0x1000) & 0x10:
        pass
    fender_regs("after the host's own power-up request (iOS: 0x11 -> 0x01)")
    print(f"[agx-start] Fender +0x0={p.read32(FENDER):#x} +0x10000={p.read32(FENDER + 0x10000):#x}",
          flush=True)

if not poke:
    # iOS never writes Fender +0x14000 (m1n1's M1 "Early init, needed?")
    AGX.poke_sgx = lambda self: setattr(self, "sgx_base", self.sgx_dev.get_reg(0)[0])
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
    for a, m, orr in tries:
        if not p.read32(FENDER + 0x1000) & 0x10:
            break
        old = p.read32(a)
        p.write32(a, old | m if orr else m)
        print(f"[agx-start] try {a:#x}: {old:#x} -> {p.read32(a):#x}", flush=True)
        end = time.time() + 1
        while time.time() < end:
            agx.work()
            if pmp_asc:
                pmp_asc.work()
        fender_regs(f"after {a:#x} {'|' if orr else ''}= {m:#x}")
    if ios_pmgr and fabric and p.read32(FENDER + 0x1000) & 0x10:
        print(f"[agx-start] pwrgate +0: {p.read32(PWRGATE0):#x}; step 2: fabric AUTO_ENABLE")
        fabric_auto()
        end = time.time() + seconds
        while time.time() < end:
            agx.work()
            if pmp_asc:
                pmp_asc.work()
            if fender:
                fender_ack()
        fender_regs("after fabric AUTO_ENABLE")
    fender_pages("after the GPU's request")
    if fender_dump:
        fender_full("after")
    if dump_regions:
        regions("after")
    if pmu_dump:
        pmu_snapshot("after")
        a, b = PMU_SNAP["before"], PMU_SNAP["after"]
        diff = [i for i in range(0x10000) if a[i] != b[i]]
        print(f"[agx-start] PMU bytes changed by the GPU's request: {len(diff)}")
        for i in diff[:64]:
            print(f"[agx-start]   PMU +{i:#06x}: {a[i]:#04x} -> {b[i]:#04x}")
    trig_regs("after the GPU's request")
    print("[agx-start] DevCtrl read ptr:", agx.ch.devctrl.state.READ_PTR.val)
    if pll_probe:
        plls("after the GPU's request")
    if pmp_asc:
        pmp_asc.dart.show_error()
except Exception as e:
    print(f"[agx-start] stopped: {e!r}")

run_shell(globals(), msg="agx = the running driver; agx.work() to pump the firmware")
