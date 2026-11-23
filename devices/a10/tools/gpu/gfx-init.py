#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""G3 (devices/a10/docs/gpu.md): boot the GFX firmware (gfx-boot.py) and hand
it init data on endpoint 0x20, crash-driven: every field we do not know yet
is zero, and the firmware's crash log says what it missed.

    ./cascadia a10 boot proxy
    M1N1DEVICE=/dev/cu.usbmodemXXXX1 build/proxyvenv/bin/python tools/gpu/gfx-init.py [SECONDS] [--power] [KEY=VAL ...]

Run from devices/a10; can be run again in the same proxy session (it stops
and reloads the firmware). KEY=VAL sets a 32-bit word: `P+0xd34=1`,
`X+0x1b0=0x101000`, `T+0x8=2`, `I+0x98=...` (regions below; VAL may name
another region's firmware VA, `@T`, or physical address, `%R`; region R is
128 KiB of RAM to stand in for RGX: `P+0x8c0=%R`). --power: Fender state 1
before the init message, as the kext has it before the firmware boots.
--channels / --kick-cp / --job: G4 (docs/gpu.md "G4 progress"). --crash: at
the end, ask for a soft crash dump and print the firmware's scheduler state
(FW_STATE) before and after it.

What the 22H355 firmware does with it (fw 0x19370, docs/gpu.md "G3"):
- the message: (0x20 << 56) | (0x81 << 48) | PA of the init page;
- the init page (registered 0x1000, the first 0xa8 bytes copied to fw
  0xd74d0): +0x08 / +0x0c init sequence PA (fw 0x5660; a first entry with
  type 0 = empty), +0x18 X (fw VA; X+0 = P), +0x1c T, +0x8c / +0x90 PA of the
  memory descriptors (0x4000 registered), +0x94 their count, +0x98 / +0x9c /
  +0xa0 / +0xa4 four more fw VAs (fw 0xd7470 / 0xd77c0 / 0xd77c8 / 0xd7468);
- a memory descriptor (0x10 bytes): +0 fw VA, +4 PA >> 12, +0xc pages
  (u16), +0xe flags (u16; bit 1 = cacheable, 0 here);
- P: +0x8c0 / +0x8c4 RGX PA, +0x8cc size (mapped, VA stored at +0x8c8);
  +0xd34 / +0xd3c gate the firmware's own perf-state requests (0: it only
  records "state 1"); about a hundred more words +0xd24..+0x1b90 copied.

Layout: one 1 MiB heap, mapped at fw VA 0x100000 by descriptor 0.
"""
import importlib.util
import os
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("gfxboot", os.path.join(HERE, "gfx-boot.py"))
gb = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gb)
p, u, iface, out = gb.p, gb.u, gb.iface, gb.out

HEAP_VA = int(os.environ.get("HEAP_VA", "0x80400000"), 0)   # window 2: a mapping below 32 MiB (window 1) hangs the firmware on access (G3 bisect)
HEAP_SIZE = 0x100000
REGIONS = {          # name: (offset in the heap, size)
    "X": (0x00000, 0x1000),
    "P": (0x10000, 0x4000),
    "T": (0x20000, 0x1000),
    "B": (0x30000, 0x8000),     # +0x9c: read up to +0x6a28
    "A": (0x40000, 0x1000),     # +0x98
    "C": (0x41000, 0x1000),     # +0xa0
    "D": (0x42000, 0x1000),     # +0xa4
    "S": (0x43000, 0x1000),
    "E": (0x44000, 0x1000),     # X+0x4 -> two words copied to fw 0xd6c90 (G3 run 5)
    "F": (0x45000, 0x1000),
    "G": (0x46000, 0x1000),
    "H": (0x48000, 0x8000),     # X+0x311 (byte offset, unaligned): the power-state struct (fw 0x18300 / 0x23e5c)     # scratch RAM (P+0xac0 PA): fw page 0x32000 is copied there and remapped
    "R": (0x80000, 0x20000),
    "W": (0xa0000, 0x12000),    # --channels: 12 work rings (256 x 0x14) + device control (256 x 0x24)
    "K": (0xb2000, 0x1000),     # --kick-cp: the compute queue object
    "L": (0xb3000, 0x1000),
    "M": (0xb4000, 0x1000),
    "N": (0xb5000, 0x4000),     # X+0x150: a status struct (+0 == 2 checked; doorbell type 5 writes +4)
    "Y": (0xb9000, 0x1000),     # T+0x7ec: GPU MMU roots, u64 page catalog PA per context (fw 0x19d18)
    "Z": (0xbc000, 0x4000),     # context 0's page catalog (Rogue PC)
    "GEN": (0xba000, 0x1000),   # T+0x7e8: host MMU generation per context (u32); fw 0x19f0e
    "XG": (0xbb000, 0x1000),    # X+0x198: the firmware's copy of those generations
    "QS": (0xc0000, 0x1000),    # compute queue: state (+0x30 command-ring write index, +0x50 ring size)
    "CR": (0xc1000, 0x1000),    # compute queue: command ring (fw VAs of commands)
    "SE": (0xc2000, 0x4000),    # compute queue: scheduling entries
    "ST": (0xc6000, 0x1000),    # compute queue: status
    "CMD": (0xc7000, 0x1000),   # the compute command
    "US": (0xc8000, 0x1000),    # its microsequence (first op & 0x3f == 0xb: StartCompute)
    "CTX": (0xc9000, 0x1000),   # CMD+0x14: a 0x90-byte context state
    "CA": (0xca000, 0x1000),    # CDM arguments
    "STAMP": (0xcb000, 0x1000), # completion stamps
    "P2": (0xcc000, 0x1000),
    "P3": (0xcd000, 0x1000),
    "P4": (0xce000, 0x1000),
    "P5": (0xcf000, 0x1000),
    "PDR": (0xd0000, 0x1000),   # --job: page directory of context 0
    "PTR": (0xd1000, 0x1000),   # --job: page table
    "GV": (0xd4000, 0x4000),    # --job: GPU VA 0x200000..0x204000 (stream, PDS HALT, context storage)
    "CDR": (0xd8000, 0x1000),   # --job: StartCompute +4 (two words -> RGX+0x4c0 / +0x4c8)
    "UT": (0xd9000, 0x1000),    # --job: unit table (0x18-byte entries)
    "FL": (0xda000, 0x1000),    # --job: flags / threshold words    # RAM stand-in for RGX (P+0x8c0=%R): see what the firmware writes
    # firmware -> host rings: control {+0 host read index, +0x20 firmware
    # write index}, entries 0x3c bytes; X+0xdc / +0xe0 (fw 0x95b0, 0x1bb7a:
    # job events) and X+0xf4 / +0xf8 (fw 0x92d0: power events) 256 entries,
    # X+0xec / +0xf0 (fw 0x15d02: trace, gated by fw 0x32044) 512 entries
    "EC0": (0xdb000, 0x100),
    "EC1": (0xdb100, 0x100),
    "EC2": (0xdb200, 0x100),
    "EV0": (0xdc000, 0x4000),
    "EV1": (0xe0000, 0x4000),
    "EV2": (0xe4000, 0x8000),
    "TS": (0xec000, 0x4000),    # RAM for I/O type 5, TempSensorRegister (P+0x6e0), which the
                                # firmware's power model reads on a cold power-up (fw 0x6914)
}
RGX_PA = gb.adt["/arm-io/sgx"].get_reg(0)[0]


GVA = 0x200000   # GPU VA of region GV in context 0


def build_job(w32, va, pa):
    """A terminate-only compute job (docs/gpu.md, "G4 progress"): context 0's
    Rogue page tables map GPU VA 0x200000.. onto GV; the CDM stream there is
    a single terminate word; StartCompute -> WaitIRQ -> FinalizeCompute -> End;
    FinalizeCompute's stamp (STAMP+0 = 1) shows completion."""
    # Rogue MMU, 4 KiB pages: PC u32 (PD >> 12) << 4 | 1 at VA[39:30];
    # PD u64 PT | 1 at VA[29:21]; PT u64 page | 1 at VA[20:12]
    w32("Z", 4 * (GVA >> 30), ((pa["PDR"] >> 12) << 4) | 1)
    pde = 8 * ((GVA >> 21) & 0x1ff)
    w32("PDR", pde, (pa["PTR"] & 0xffffffff) | 1)
    w32("PDR", pde + 4, pa["PTR"] >> 32)
    for i in range(4):
        pte = 8 * (((GVA >> 12) & 0x1ff) + i)
        page = pa["GV"] + 0x1000 * i
        w32("PTR", pte, (page & 0xffffffff) | 1)
        w32("PTR", pte + 4, page >> 32)
    # GV: +0 CDM stream (terminate), +0x1000 PDS HALT program / +0x1010 data,
    # +0x2000 context storage
    w32("GV", 0x0, 0x80000000)
    w32("GV", 0x1000, 0xd3000000)
    # CDM arguments (StartCompute +8): [0..1] RGX+0x498 storage, [2..3]
    # RGX+0x4b8 PDS code / data, [4..5] RGX+0x480 stream base
    w32("CA", 0x00, GVA + 0x2000)
    w32("CA", 0x08, GVA + 0x1000)
    w32("CA", 0x0c, GVA + 0x1010)
    w32("CA", 0x10, GVA)
    # [6..0x11]: six 64-bit addresses through RGX+0x11040 (+0x11ff8 = 0..5);
    # left 0 they make the GPU write to VA 0 (MMU fault at the PD level, G4
    # run 20) -- give each its own 0x100 bytes in GV
    for i in range(6):
        w32("CA", 0x18 + 8 * i, GVA + 0x3000 + 0x100 * i)
    # context state (StartCompute +0x2c): word 0 = 0 (fresh start), [0xe..0x10]
    # -> RGX+0x4a8 / +0x4ac / +0x4b0
    w32("CTX", 0x38, GVA + 0x1000)
    w32("CTX", 0x3c, GVA + 0x1010)
    # the microsequence (0x88 bytes; 0x84 with --no-wait)
    u = "US"
    w32(u, 0x00, 0xb)              # StartCompute, 0x4c bytes
    w32(u, 0x04, va["CDR"])
    w32(u, 0x08, va["CA"])
    w32(u, 0x0c, va["UT"])
    w32(u, 0x2c, va["CTX"])
    # StartCompute programs the CDM only while the 64-bit +0x40 is strictly
    # below the one at *(+0x48); otherwise it sets *(+0x3c) = 1 (cancelled)
    # and starts nothing (fw 0xb49c case 0xb; G4 run 19)
    w32(u, 0x3c, va["FL"])         # must be 0; the firmware sets 1 on a cancel
    w32(u, 0x48, va["FL"] + 0x10)  # the threshold
    w32("FL", 0x10, 0xffffffff)
    w32("FL", 0x14, 0xffffffff)
    # --no-wait: no WaitIRQ (to run against the RAM stand-in for RGX, where
    # no interrupt ever comes); FinalizeCompute's fast path does not wait
    f = 0x4c
    if "--no-wait" not in sys.argv:
        w32(u, 0x4c, 4 << 6)       # WaitIRQ, event bit 2
        f = 0x50
    w32(u, f + 0x00, 0xc)          # FinalizeCompute, 0x34 bytes on completion
    w32(u, f + 0x04, va["UT"])
    w32(u, f + 0x20, va["STAMP"])  # stamp address
    w32(u, f + 0x24, 1)            # stamp value
    w32(u, f + 0x28, 0x34)         # skip on the context-store path
    # +0x2c: "already finished" flag: nonzero takes FinalizeCompute's fast path
    # (no CDM stop / context store, which faults in the PDS without a
    # context-store program, G4 runs 20-28); our terminate-only stream runs
    # no kernel that could set it, so the host does (run 29)
    w32(u, f + 0x2c, va["FL"] + 0x20)
    w32("FL", 0x20, 1)
    w32(u, f + 0x30, va["FL"] + 0x30)
    w32(u, f + 0x34, 2)            # End
    us_size = f + 0x38
    # the command
    w32("CMD", 0x000, 3)
    w32("CMD", 0x014, va["CTX"])
    w32("CMD", 0x1e8, va["US"])
    w32("CMD", 0x1ec, us_size)
    w32("CMD", 0x298, va["STAMP"] + 0x100)
    w32("CMD", 0x29c, va["STAMP"] + 0x200)
    # the command's completion record, used by the CL post-processing callback
    # (fw 0x15750 -> 0x95b0): *(+0x26c) = +0x274 once the command is done;
    # left 0 the firmware writes to address 0 and crashes (G4 run 29)
    w32("CMD", 0x26c, va["STAMP"] + 0x10)
    w32("CMD", 0x274, 0xc0de)


def rgx_on():
    """Fender's perf state settled and nonzero. Not a safe gate for reading
    RGX once the firmware manages power itself (a cold GPU): it powers the
    idle core off within milliseconds, and an RGX read that loses that race
    is an SError panic on the AP (L2C_ERR_ADR = the RGX address; G4 runs
    43-44). So RGX is read at the end only with --rgx."""
    v = p.read32(gb.FENDER + 0x1000)
    return v & 0xf != 0 and not v & 0x10


def watch(kf, secs, pa):
    """--watch: instead of a plain wait, poll what shows the job's progress
    and print each change with its time since the kick: the stamps,
    Fender's perf state, the rings' write indices. No RGX reads (rgx_on()).
    An MMU fault shows as power event 0x16 (the firmware's recovery, which
    reads RGX+0xe150 itself) and B+0x4a00 = 1."""
    probes = {
        "STAMP+0": pa["STAMP"], "STAMP+0x10": pa["STAMP"] + 0x10,
        "Fender+0x1000": gb.FENDER + 0x1000,
        "EC0 wr": pa["EC0"] + 0x20, "EC1 wr": pa["EC1"] + 0x20,
        "B+0x4a00 recovery": pa["B"] + 0x4a00,
    }
    last = {}
    t0 = time.time()
    while time.time() - t0 < secs:
        kf.work_for(0.002)
        for name, a in probes.items():
            v = p.read32(a)
            if last.get(name) != v:
                out(f"watch +{(time.time() - t0) * 1000:8.1f} ms  {name} = {v:#x}")
                last[name] = v


RINGS = []      # (name, control PA, entries PA, entry count): set in main()
EVENTS = {}     # (ring, event type) -> count


def drain_rings():
    """Consume the firmware -> host rings (host read index := firmware write
    index): a full ring makes the firmware wait ("Waiting for space"), G4
    run 23. Counts the event types (each entry's first word)."""
    for name, ctl, ents, n in RINGS:
        rd, wr = p.read32(ctl), p.read32(ctl + 0x20)
        while rd != wr:
            t = p.read32(ents + 0x3c * rd)
            EVENTS[(name, t)] = EVENTS.get((name, t), 0) + 1
            rd = (rd + 1) % n
        p.write32(ctl, wr)


class InitEndpoint(gb.AGXEndpoint):
    """Endpoints 0x20 / 0x21. 0x2042 << 48 on 0x20 = the firmware has put
    something in its rings: drain them (logged once, then counted)."""
    SHORT = "agxinit"
    seen = {}

    def handle_msg(self, msg):
        n = InitEndpoint.seen.get(msg, 0)
        InitEndpoint.seen[msg] = n + 1
        if n == 0:
            out(f"ep {self.epnum:#x} message {msg:#018x}")
        if msg >> 48 == 0x2042:
            drain_rings()
        return True


def main():
    secs = 3.0
    sets = []
    for a in sys.argv[1:]:
        if a.startswith("--"):
            continue
        if "=" in a:
            sets.append(a)
        else:
            secs = float(a)

    gb.gfx_power_on()
    kf, fwbuf = gb.boot_firmware(5.0, endpoints={0x20: InitEndpoint, 0x21: InitEndpoint})
    gb_buf[0] = fwbuf

    heap = u.heap.memalign(0x10000, HEAP_SIZE)
    page = u.heap.memalign(0x4000, 0x4000)       # init page; +0x1000 init sequence
    desc = u.heap.memalign(0x4000, 0x4000)
    iface.writemem(heap, bytes(HEAP_SIZE))
    iface.writemem(page, bytes(0x4000))
    iface.writemem(desc, bytes(0x4000))
    seq = page + 0x1000
    va = {k: HEAP_VA + off for k, (off, _) in REGIONS.items()}
    pa = {k: heap + off for k, (off, _) in REGIONS.items()}
    pa["I"] = page
    pa["Q"] = desc       # the memory descriptors (Q+0x0 = descriptor 0's VA, ...)

    def w32(region, off, val):
        # through writemem: some fields are unaligned (X+0x311 is the power struct)
        iface.writemem(pa[region] + off, struct.pack("<I", val & 0xffffffff))

    # memory descriptor 0: the heap
    p.write32(desc + 0x0, HEAP_VA)
    p.write32(desc + 0x4, heap >> 12)
    p.write32(desc + 0x8, 0)
    p.write32(desc + 0xc, (HEAP_SIZE >> 12) & 0xffff)      # u16 pages, u16 flags = 0
    # init page
    w32("I", 0x08, seq)
    w32("I", 0x0c, seq >> 32)
    w32("I", 0x18, va["X"])
    w32("I", 0x1c, va["T"])
    w32("I", 0x8c, desc)
    w32("I", 0x90, desc >> 32)
    w32("I", 0x94, 1)
    w32("I", 0x98, va["A"])
    w32("I", 0x9c, va["B"])
    w32("I", 0xa0, va["C"])
    w32("I", 0xa4, va["D"])
    # X -> P; P: RGX
    w32("X", 0x0, va["P"])
    # firmware -> host rings (REGIONS EC* / EV*); without X+0xf4 / +0xf8 a cold
    # power-up faults on address 0x20 (fw 0x92d0, G4 run 15)
    for (cx, ex), c, e, n in (((0xdc, 0xe0), "EC0", "EV0", 256), ((0xf4, 0xf8), "EC1", "EV1", 256),
                              ((0xec, 0xf0), "EC2", "EV2", 512)):
        w32("X", cx, va[c])
        w32("X", ex, va[e])
        RINGS.append((c, pa[c], pa[e], n))
    w32("P", 0x8c0, RGX_PA)
    w32("P", 0x8c4, RGX_PA >> 32)
    w32("P", 0x8cc, 0x20000)
    # 13 work / status channels: X+0xc / X+0x14, ... X+0xcc / X+0xd4 point at
    # each channel's two ring indices (fw 0x4f80 / 0x5062 compare them); one
    # zero word each on page G (G3 run 7).
    for ch in range(13):
        for k in range(2):
            w32("X", 0xc + 0x10 * ch + 8 * k, va["G"] + 0x10 * ch + 4 * k)
    if "--channels" in sys.argv:
        # a channel is 16 bytes at X+0xc + 0x10 * ch: {&fw read index, &copy,
        # &host write index, ring base}; ch = 3 * group + dm (0 TA, 1 3D,
        # 2 CP); ch 12 = device control (0x24-byte entries) -- fw 0x2167c
        for ch in range(13):
            w32("X", 0x10 + 0x10 * ch, va["G"] + 0x100 + 4 * ch)
            w32("X", 0x18 + 0x10 * ch, va["W"] + 0x1400 * ch)
    if "--job" in sys.argv:
        # X+0x4 -> E: two words the firmware copies at init (fw 0xd6c90) and
        # programs into RGX+0xe240 at every GPU power-up (fw 0x5660): the GPU
        # VA of the cache flush's scratch / fence write. Left 0, the flush
        # before an idle power-off (fw 0x61a0 -> 0x17cb0) writes to GPU VA 0:
        # the PDS MMU fault of G4 runs 20-37. Must be set before the init
        # message; the page is mapped by the job's page tables (GV+0x3800).
        w32("E", 0x0, GVA + 0x3800)
        w32("E", 0x4, 0)
    for s in sets:
        k, v = s.split("=")
        region, off = k.split("+")
        val = va[v[1:]] if v.startswith("@") else pa[v[1:]] if v.startswith("%") else int(v, 0)
        w32(region, int(off, 0), val)
        out(f"set {region}+{int(off, 0):#x} = {val:#x}")
    for addr in (heap, page, desc):
        p.dc_cvac(addr, 0x4000 if addr != heap else HEAP_SIZE)
    out("heap " + " ".join(f"{k}=va {va[k]:#x}/pa {pa[k]:#x}" for k in REGIONS) +
        f"; init page {page:#x}, sequence {seq:#x}, descriptors {desc:#x}")

    if "--power" in sys.argv:
        # what the kext does before the firmware ever runs: Fender state 1
        F = gb.FENDER
        p.write32(F + 0x1000, 0x11)
        t = time.time()
        while time.time() - t < 0.2 and (p.read32(F + 0x1000) & 0x1f) != 1:
            pass
        time.sleep(0.01)
        out(f"--power: Fender +0x1000 = {p.read32(F + 0x1000):#x}, +0x14 = {p.read32(F + 0x14):#x}")

    msg = (0x20 << 56) | (0x81 << 48) | page
    out(f"init message {msg:#018x}")
    kf.send(msg)
    if "--kick-cp" in sys.argv:
        try:
            kf.work_for(2.0)
        except Exception as e:
            out(f"crashed before the kick: {e!r}")
        # one work-ring entry on the compute channel (2): {+8 queue, +0xc dm 2,
        # +0x10 u16 1}; host write index 1; doorbell type 3, dm 2 (fw 0x1db94)
        ring = pa["W"] + 0x1400 * 2
        iface.writemem(ring, struct.pack("<IIIIHBB", 0, 0, va["K"], 2, 1, 0, 0))
        w32("G", 0x10 * 2 + 4, 1)
        # the queue object K (fw 0x2167c): +0 state, +4 command ring, +0xc
        # scheduling entries, +0x44 unit mask, +0x88 status; one command
        w32("K", 0x00, va["QS"])
        w32("K", 0x04, va["CR"])
        w32("K", 0x0c, va["SE"])
        w32("K", 0x44, 1)
        w32("K", 0x88, va["ST"])
        w32("QS", 0x50, 256)
        w32("CR", 0x00, va["CMD"])
        if "--job" in sys.argv:
            build_job(w32, va, pa)
        for s2 in sets:                      # KEY=VAL on these regions again, over the job's defaults
            k2, v2 = s2.split("=")
            r2, o2 = k2.split("+")
            if r2 in ("K", "QS", "CR", "SE", "ST", "CMD", "US", "CTX", "CA", "STAMP", "P2", "P3",
                      "CDR", "UT", "FL", "GV", "PDR", "PTR"):
                w32(r2, int(o2, 0), va[v2[1:]] if v2.startswith("@") else pa[v2[1:]] if v2.startswith("%") else int(v2, 0))
        out("kick: compute channel 2, write index 1, doorbell (0x21 << 56) | (3 << 48) | 2")
        kf.send((0x21 << 56) | (3 << 48) | 2)
    try:
        if "--watch" in sys.argv:
            watch(kf, secs, pa)
        else:
            kf.work_for(secs)
        out(f"no crash in {secs} s; ping: {'pong' if gb.ping(kf) else 'NO REPLY'}")
    except Exception as e:
        out(f"stopped on {e!r}")
    gb.regs("end")
    drain_rings()
    out("messages: " + " ".join(f"{m:#x} x{c}" for m, c in InitEndpoint.seen.items()))
    out("ring events: " + " ".join(f"{r}:{t:#x} x{c}" for (r, t), c in sorted(EVENTS.items())))
    if "--job" in sys.argv:
        out(f"STAMP+0 = {p.read32(pa['STAMP']):#x} (FinalizeCompute: 1), "
            f"+0x10 = {p.read32(pa['STAMP'] + 0x10):#x} (the command's completion record: 0xc0de)")
        rgx = gb.adt["/arm-io/sgx"].get_reg(0)[0]
        if "--rgx" in sys.argv and rgx_on():
            for off in (0x4a0, 0x480, 0x478, 0xe140, 0xe148, 0xe150, 0xe154, 0xe238):
                out(f"RGX +{off:#x} = {p.read32(rgx + off):#010x}")
    # what the firmware wrote back into our structures (nonzero words)
    for k in ("X", "A", "C", "D", "R", "SE", "P2", "P3", "P4", "EC0", "EC1", "EC2", "EV0", "EV1"):
        data = iface.readmem(pa[k], 0x200)
        words = struct.unpack("<128I", data)
        nz = [(i * 4, w) for i, w in enumerate(words) if w]
        out(f"{k}: " + " ".join(f"+{o:#x}={w:#x}" for o, w in nz[:24]))
    if "--rdump" in sys.argv:    # with P+0x8c0=%R: every RGX word the firmware wrote
        words = struct.unpack("<32768I", iface.readmem(pa["R"], 0x20000))
        out("R (RGX stand-in): " + " ".join(f"+{i * 4:#x}={w:#x}" for i, w in enumerate(words) if w))
    b = iface.readmem(gb_buf[0] + 0xd74d0, 0xa8)
    w = struct.unpack("<42I", b)
    out("fw copy of the init page (0xd74d0): " + " ".join(f"+{i * 4:#x}={x:#x}" for i, x in enumerate(w) if x))
    for name, a in (("main flag 0xd7b28", 0xd7b28), ("X 0xd74c8", 0xd74c8), ("RGX va 0x323cc", 0x323cc)):
        out(f"{name}: {p.read32(gb_buf[0] + a):#x}")
    fw_state("before the crash request")
    if "--crash" in sys.argv:
        # the coprocessor's D-cache hides its own writes from DRAM; a soft
        # crash (ep 1, 0x40) makes the firmware write its crash log, and with
        # it (to check) its dirty lines -- then read the scheduler's state
        out("asking for a crash dump (ep 1, 0x40)")
        kf.send((1 << 56) | 0x40)
        try:
            kf.work_for(4.0)
            out("no crash report within 4 s")
        except Exception as e:
            out(f"stopped on {e!r}")
        fw_state("after the crash")
    out("end")


# The firmware's scheduler state (22H355 data addresses; docs/gpu.md "G4
# progress"): wake bits, the dispatcher's gates, the per-data-master slots,
# the queue masks, the queue table.
FW_STATE = (
    ("d6c90 flush fence VA", 0xd6c90, 2), ("d75d0 agx_interrupt wake", 0xd75d0, 1), ("d75f4 hw events", 0xd75f4, 1),
    ("d743c", 0xd743c, 1), ("a4c90 halt", 0xa4c90, 1), ("a4c5c/a4c60", 0xa4c5c, 2),
    ("a4d34 pending dm", 0xa4d34, 1), ("a4d50", 0xa4d50, 1), ("a4d64", 0xa4d64, 1),
    ("a4e8c slots", 0xa4e88, 2), ("323cc rgx va", 0x323cc, 1), ("d74c8 X", 0xd74c8, 1),
    ("slots a4c98", 0xa4c98, 0x54), ("masks b0320", 0xb0320, 0x68),
    ("queues a4e58", 0xa4e58, 0x30), ("queue 0 a4e90", 0xa4e90, 10),
)


def fw_state(tag):
    for name, a, n in FW_STATE:
        w = struct.unpack(f"<{n}I", iface.readmem(gb_buf[0] + a, 4 * n))
        nz = " ".join(f"+{i * 4:#x}={x:#x}" for i, x in enumerate(w) if x) or "all 0"
        out(f"[{tag}] {name} @{a:#x}: {nz}")


gb_buf = [0]
main()
