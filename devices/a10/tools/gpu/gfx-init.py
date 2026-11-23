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
    # context state (StartCompute +0x2c): word 0 = 0 (fresh start), [0xe..0x10]
    # -> RGX+0x4a8 / +0x4ac / +0x4b0
    w32("CTX", 0x38, GVA + 0x1000)
    w32("CTX", 0x3c, GVA + 0x1010)
    # the microsequence (0x88 bytes)
    u = "US"
    w32(u, 0x00, 0xb)              # StartCompute, 0x4c bytes
    w32(u, 0x04, va["CDR"])
    w32(u, 0x08, va["CA"])
    w32(u, 0x0c, va["UT"])
    w32(u, 0x2c, va["CTX"])
    w32(u, 0x3c, va["FL"])         # must be 0
    w32(u, 0x48, va["FL"] + 0x10)  # cancel threshold (0)
    w32(u, 0x4c, 4 << 6)           # WaitIRQ, event bit 2
    w32(u, 0x50, 0xc)              # FinalizeCompute, 0x34 bytes on completion
    w32(u, 0x54, va["UT"])
    w32(u, 0x70, va["STAMP"])      # +0x20: stamp address
    w32(u, 0x74, 1)                # +0x24: stamp value
    w32(u, 0x78, 0x34)             # +0x28: skip on the context-store path
    w32(u, 0x7c, va["FL"] + 0x20)  # +0x2c: flag (0)
    w32(u, 0x80, va["FL"] + 0x30)  # +0x30
    w32(u, 0x84, 2)                # End
    # the command
    w32("CMD", 0x000, 3)
    w32("CMD", 0x014, va["CTX"])
    w32("CMD", 0x1e8, va["US"])
    w32("CMD", 0x1ec, 0x88)
    w32("CMD", 0x298, va["STAMP"] + 0x100)
    w32("CMD", 0x29c, va["STAMP"] + 0x200)


class InitEndpoint(gb.AGXEndpoint):
    SHORT = "agxinit"


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
        for s2 in sets:                      # allow KEY=VAL on these regions too
            k2, v2 = s2.split("=")
            r2, o2 = k2.split("+")
            if r2 in ("K", "QS", "CR", "SE", "ST", "CMD", "US", "CTX", "CA", "STAMP", "P2", "P3"):
                w32(r2, int(o2, 0), va[v2[1:]] if v2.startswith("@") else pa[v2[1:]] if v2.startswith("%") else int(v2, 0))
        if "--job" in sys.argv:
            build_job(w32, va, pa)
        out("kick: compute channel 2, write index 1, doorbell (0x21 << 56) | (3 << 48) | 2")
        kf.send((0x21 << 56) | (3 << 48) | 2)
    try:
        kf.work_for(secs)
        out(f"no crash in {secs} s; ping: {'pong' if gb.ping(kf) else 'NO REPLY'}")
    except Exception as e:
        out(f"stopped on {e!r}")
    gb.regs("end")
    if "--job" in sys.argv:
        out(f"STAMP+0 = {p.read32(pa['STAMP']):#x} (1 = the job completed)")
        rgx = gb.adt["/arm-io/sgx"].get_reg(0)[0]
        if p.read32(gb.FENDER + 0x1000) & 0x1f:
            for off in (0x4a0, 0x480, 0x478, 0xe140, 0xe148):
                out(f"RGX +{off:#x} = {p.read32(rgx + off):#010x}")
    # what the firmware wrote back into our structures (nonzero words)
    for k in ("X", "A", "C", "D", "R"):
        data = iface.readmem(pa[k], 0x200)
        words = struct.unpack("<128I", data)
        nz = [(i * 4, w) for i, w in enumerate(words) if w]
        out(f"{k}: " + " ".join(f"+{o:#x}={w:#x}" for o, w in nz[:24]))
    b = iface.readmem(gb_buf[0] + 0xd74d0, 0xa8)
    w = struct.unpack("<42I", b)
    out("fw copy of the init page (0xd74d0): " + " ".join(f"+{i * 4:#x}={x:#x}" for i, x in enumerate(w) if x))
    for name, a in (("main flag 0xd7b28", 0xd7b28), ("X 0xd74c8", 0xd74c8), ("RGX va 0x323cc", 0x323cc)):
        out(f"{name}: {p.read32(gb_buf[0] + a):#x}")
    out("end")


gb_buf = [0]
main()
