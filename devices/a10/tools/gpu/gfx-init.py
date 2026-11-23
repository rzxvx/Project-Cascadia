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
    "R": (0x80000, 0x20000),    # RAM stand-in for RGX (P+0x8c0=%R): see what the firmware writes
}
RGX_PA = gb.adt["/arm-io/sgx"].get_reg(0)[0]


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
    try:
        kf.work_for(secs)
        out(f"no crash in {secs} s; ping: {'pong' if gb.ping(kf) else 'NO REPLY'}")
    except Exception as e:
        out(f"stopped on {e!r}")
    gb.regs("end")
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
