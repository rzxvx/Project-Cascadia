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
sys.path.insert(0, HERE)

HEAP_VA = int(os.environ.get("HEAP_VA", "0x80400000"), 0)   # window 2: a mapping below 32 MiB (window 1) hangs the firmware on access (G3 bisect)
HEAP_SIZE = 0x400000           # 4 MiB: the second MiB holds the TA job (--kick-ta), the third the 3D job (--kick-3d)
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
    # --kick-ta: the TA queue and command (G6)
    "KT": (0x100000, 0x1000),   # TA queue object
    "QST": (0x101000, 0x1000),  # its state
    "CRT": (0x102000, 0x1000),  # its command ring
    "SET": (0x103000, 0x4000),  # its scheduling entries
    "STT": (0x107000, 0x1000),  # its status
    "CMDT": (0x108000, 0x1000), # the TA command (type 0; microsequence at +0x464)
    "UST": (0x109000, 0x1000),  # its microsequence
    "CTXT": (0x10a000, 0x1000), # CMDT+0x14: 0x90-byte context state
    "T74": (0x10b000, 0x1000),  # CMDT+0x20: 0x74 bytes
    "TBIG": (0x10c000, 0x4000), # CMDT+0x24: a large object (+0x3300 used)
    "TAA": (0x110000, 0x1000),  # StartTA +0x4: 8 x u32 (RGX 0xc78 ...)
    "TAB": (0x111000, 0x1000),  # StartTA +0x8: the 64-bit register block
    "TAO": (0x112000, 0x1000),  # StartTA +0x10 / FinalizeTA +0x4 object
    "UT2": (0x113000, 0x1000),  # unit table
    "FL2": (0x114000, 0x1000),  # flags / threshold
    "TAC": (0x115000, 0x1000),  # FinalizeTA +0x8 counter object
    "CTX2": (0x116000, 0x1000), # StartTA +0x40: context state (word 0 = 0: fresh)
    "PMB": (0x120000, 0xe0000), # --ta-probe: real pages at GPU VA 0x300000.., one 0x7000 slice per StartTA slot
    # --kick-3d: the 3D queue, command and render state (G6)
    "K3": (0x200000, 0x1000),   # 3D queue object
    "QS3": (0x201000, 0x1000),  # its state
    "CR3": (0x202000, 0x1000),  # its command ring
    "SE3": (0x203000, 0x4000),  # its scheduling entries
    "ST3": (0x207000, 0x1000),  # its status
    "CMD3": (0x208000, 0x1000), # the 3D command (type 1; microsequence at +0x14, >= 0x918 bytes)
    "US3": (0x209000, 0x1000),  # its microsequence
    "CTX3": (0x20a000, 0x1000), # CMD3+0x1c: 0x90-byte context state
    "S3A": (0x20b000, 0x1000),  # Start3D +0x4: 18 x u32 (RGX 0xf18 ...)
    "S3B": (0x20c000, 0x1000),  # Start3D +0x8: 29 x u64 (RGX 0xfa8 ...)
    "UT3": (0x20d000, 0x1000),  # unit table (0x28-byte entries)
    "FL3": (0x20e000, 0x1000),  # flags / threshold words
    "F3C": (0x20f000, 0x1000),  # Finalize3D +0x18 counter block
    "FLD": (0x210000, 0x1000),  # CMD3+0x20 (and CMDT+0x20): the 0x74-byte free-list descriptor
    "PMH": (0x211000, 0x1000),  # PM slot +0xc0: the slot header
    "PTR2": (0x212000, 0x1000), # page table for GPU VA 0x400000..0x5fffff
    "PTR0": (0x213000, 0x1000), # page table for GPU VA 0..0x1fffff
    "VA0": (0x214000, 0x1000),  # the page at GPU VA 0
    "G3": (0x240000, 0x100000), # GPU VA 0x400000..0x4fffff: render target, programs, PM buffers
    "ZP": (0xf0000, 0x1000),
    "HP": (0xf4000, 0x4000),    # --store-ios: the PDS / USC heap at GPU VA 0x300000    # --pad: the page behind the unused part of GPU VA 0x200000..0x3fffff
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
    if "--pad" in sys.argv:
        # the rest of the 2 MiB (GPU VA 0x204000..0x3fffff) onto one zero page,
        # ZP: absorbs reads past GV, so they show up as data, not MMU faults
        ents = b"".join(struct.pack("<Q", pa["ZP"] | 1) for _ in range(4, 512))
        iface.writemem(pa["PTR"] + 8 * 4, ents)
    # GV: +0 CDM stream (terminate), +0x1000 PDS HALT program / +0x1010 data,
    # +0x2000 context storage
    w32("GV", 0x1000, 0xd3000000)
    if "--kernel" in sys.argv:
        # G5 step A: one COMPUTE_KERNEL block (Mesa csbgen rogue/cdm.xml:
        # K0 K1 K2 K3 K4 K5 K8, no indirect / global offsets; --rec8 adds a
        # zero eighth word) whose PDS program is a HALT at GV+0x1100, data
        # at GV+0x1180; then the terminate. K0: usc_target ANY (bit 1), no
        # PDS data / temps, no USC sizes. K3-K5: workgroups - 1; K8: 1x1x1.
        rec = [0x2, GVA + 0x1180, GVA + int(os.environ.get("KREC_K2", "0x1100"), 0), 0, 0, 0, 0]
        if "--rec8" in sys.argv:
            rec.append(0)
        for i, w in enumerate(rec + [0x80000000]):
            w32("GV", 4 * i, w)
        w32("GV", 0x1100, 0xd3000000)
        if "--usc" in sys.argv:
            # G5 step B: the PDS program launches a USC task (Mesa
            # pvr_rogue_pds_defs.h): DOUTU, END=1, src0 = const64 0 ->
            # 0xf4000002, then HALT. Its data (K0 pds_data_size 1 = 16 bytes):
            # the DOUTU word, EXE_OFF = the USC code's address (bits 31:2),
            # TEMPS bits 40:35 (units of 4), here 0. The USC code: our
            # `empty` kernel (USC_KERNEL=n picks another) from
            # build/gpu/isa/g5.g9p.metallib at GV+0x1400.
            import g9pbin
            code = g9pbin.kernels(os.path.join(HERE, "../../build/gpu/isa/g5.g9p.metallib"))
            usc = code[int(os.environ.get("USC_KERNEL", "0"))]
            usc += bytes(-len(usc) % 4)
            for i in range(0, len(usc), 4):
                w32("GV", 0x1400 + i, struct.unpack_from("<I", usc, i)[0])
            w32("GV", 0x0, 0x2 | (1 << 2))
            w32("GV", 0x1100, 0xf4000002)
            w32("GV", 0x1104, 0xd3000000)
            w32("GV", 0x1180, GVA + 0x1400)
            w32("GV", 0x1184, 0)
    else:
        w32("GV", 0x0, 0x80000000)
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
    if "--store" in sys.argv:
        build_store(w32, va, pa)
    if "--store-ios" in sys.argv:
        build_store_ios(w32, va, pa)


def build_ta(w32, va, pa):
    """G6 step 1: a TA job with an empty VDM stream (one STREAM_TERMINATE,
    0xc0000000, at GPU VA 0x203c00), on the TA channel (0) with its own
    queue (KT). Microsequence: StartTA (op 5, 0x68) -> WaitIRQ (event bit 5,
    TA_FINISHED) -> FinalizeTA (op 6, 0x40, "already finished" fast path)
    -> End. Stamp: STAMP+0x20 = 1. docs/gpu.md "G6 progress"."""
    ring = pa["W"]                                   # channel 0 = group 0, TA
    iface.writemem(ring, struct.pack("<IIIIHBB", 0, 0, va["KT"], 0, 1, 0, 0))
    w32("G", 0x10 * 0 + 4, 1)
    w32("KT", 0x00, va["QST"])
    w32("KT", 0x04, va["CRT"])
    w32("KT", 0x0c, va["SET"])
    w32("KT", 0x44, 1)
    w32("KT", 0x88, va["STT"])
    w32("QST", 0x50, 256)
    w32("CRT", 0x00, va["CMDT"])
    # the command: type 0, its pointers, the microsequence at +0x464
    w32("CMDT", 0x000, 0)
    w32("CMDT", 0x014, va["CTXT"])
    w32("CMDT", 0x020, va["T74"])
    w32("CMDT", 0x024, va["TBIG"])
    # the VDM stream: terminate only; its base in StartTA's block B[0x1e] (RGX+0x408)
    w32("GV", 0x3c00, 0xc0000000)
    w32("TAB", 0x78, GVA + 0x3c00)
    u = "UST"
    w32(u, 0x00, 5)                 # StartTA
    w32(u, 0x04, va["TAA"])
    w32(u, 0x08, va["TAB"])
    w32(u, 0x0c, va["FL2"] + 0x30)  # -> a word copied to the unit table (a null here faults, run 1)
    w32(u, 0x10, va["TAO"])
    w32(u, 0x14, va["UT2"])
    w32(u, 0x18, 1)
    w32(u, 0x1c, va["FL2"])         # cancel flag (0)
    w32(u, 0x40, va["CTX2"])        # context state: 0 = fresh start
    w32(u, 0x64, va["FL2"] + 0x10)  # threshold for +0x5c / +0x60
    w32("FL2", 0x10, 0xffffffff)
    w32("FL2", 0x14, 0xffffffff)
    # StartTA is 0x68 bytes: a WaitIRQ at +0x6c (as if 4 + 0x68) never ends
    # and the PM writes nothing (G6 run 6)
    w32(u, 0x68, 0x20 << 6)         # WaitIRQ: TA_FINISHED
    f = 0x6c
    if "--tri" in sys.argv and "TA_REALWAIT" not in os.environ:
        os.environ["TA_REALWAIT"] = "0x20"     # with the 0x470 program the TA raises TA_FINISHED
    if os.environ.get("TA_REALWAIT"):
        # a WaitIRQ that is executed (StartTA is 0x6c): never ends on an
        # empty stream without the PPP terminate / TE_PSG completeonterminate
        w32(u, 0x6c, int(os.environ["TA_REALWAIT"], 0) << 6)
        f = 0x70
    w32(u, f + 0x00, 6)             # FinalizeTA
    w32(u, f + 0x04, va["TAO"])
    w32(u, f + 0x08, va["TAC"])
    w32(u, f + 0x0c, va["UT2"])
    w32(u, f + 0x14, va["FL2"] + 0x20)   # "already finished"
    w32("FL2", 0x20, 1)
    w32(u, f + 0x30, va["STAMP"] + 0x20)
    w32(u, f + 0x34, 1)
    w32(u, f + 0x40, 2)             # End
    w32("CMDT", 0x464, va["UST"])
    w32("CMDT", 0x468, f + 0x44)
    # the command's completion record for the TA post-processing callback
    # (fw 0x13758 -> 0x95b0): *(+0x554) = +0x55c (left 0: a crash, G6 run 4)
    w32("CMDT", 0x554, va["STAMP"] + 0x30)
    w32("CMDT", 0x55c, 0x7a)
    if "--ta-probe" in sys.argv:
        ta_probe_setup(w32, va, pa)
    if "--pm" in sys.argv:
        pm_setup(w32, va, pa)


TA_SLOTS = ([("TAB", o) for o in (0x00, 0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0x40, 0x48, 0x50,
                                  0x58, 0x60, 0x68, 0x70, 0x80, 0x88, 0x90, 0x98, 0xa0, 0xa8, 0xb0,
                                  0xb8, 0xc0, 0xc8, 0xd0, 0xd8)]
            + [("TAO", o) for o in (0xa0, 0xa8, 0xb0)])
TA_PROBE_VA, TA_PROBE_STRIDE = 0x300000, 0x7000


def ta_probe_setup(w32, va, pa):
    """--ta-probe: GPU VA 0x300000..0x3dffff onto region PMB (real pages); every
    64-bit StartTA slot (block B and the TAO object's +0xa0 / +0xa8 / +0xb0)
    gets its own 0x7000-byte slice, so the TA writes land instead of
    faulting, and the end of the run shows which slice got written."""
    n = 0xe0000 // 0x1000
    ents = b"".join(struct.pack("<Q", (pa["PMB"] + 0x1000 * k) | 1) for k in range(n))
    iface.writemem(pa["PTR"] + 8 * ((TA_PROBE_VA >> 12) & 0x1ff), ents)
    for i, (r, o) in enumerate(TA_SLOTS):
        w32(r, o, TA_PROBE_VA + TA_PROBE_STRIDE * i)
        w32(r, o + 4, 0)


# --pm: the render target and the parameter manager's buffers in region G3
# (GPU VA G3VA..), docs/gpu.md "G6 step 2"
G3 = {
    "code": 0x00000,    # PDS / USC code heap (64 KiB): PDS base = USC base = G3VA (the
                        # base is used 2 MiB-aligned: 0x410000 acted as 0x400000, run 28)
    "rt": 0x50000,      # 64x64 BGRA8 render target (0x4000)
    "rgn": 0x54000,     # region headers (TA 0xc38)
    "tpc": 0x55000,     # tail pointer cache (TA 0xc40)
    "mta": 0x56000,     # macrotile array (slot +0xa8 -> 0x240 / ISP 0xf20)
    "mlist": 0x57000,   # PM MMU list (slot +0xa0 -> 0x2d8 / 0x2e0), 0x2000
    "vheap": 0x59000,   # VHEAP table (slot +0xb0 -> 0x248), 0x2000
    "fl": 0x5b000,      # the free list: u32 physical page numbers
    "ctx": 0x5c000,     # the 0x428 / 0xcc0 buffers (+0x100 / +0x200); the TA reads
                        # far beyond them (data at ctx+0x2000 broke it, run 24): 0x8000
    "ppp": 0x64000,     # PPP state: the terminate block
    "pages": 0x80000,   # PM pages (4 KiB each) handed out by the free list
}
# PM_PAGES: 1 works (G6 3D runs 16-34); with 128 the PM's start-up takes 45
# pages (VCE page tables + 40 data pages), writes no macrotile array, and the
# 3D's IPF then reads PM contexts that hold no catalog (fault, ctx 4 / 5).
# With one page the start-up cannot allocate and writes the macrotile array
# instead -- a workaround until the PM start-up is understood.
PM_PAGES = int(os.environ.get("PM_PAGES", "128" if "--tri" in sys.argv else "1"), 0)


def pm_setup(w32, va, pa):
    """The TA's parameter manager for a 64x64 target (32x32 tiles: 2x2 tiles,
    macrotiles 4x4 as upstream pvr_hwrt.c computes them): free list, MLIST,
    VHEAP table, macrotile array, region headers, TPC; TE / PPP registers
    in StartTA's 32-bit block; a VDM stream of the PPP terminate state
    (Mesa csbgen ppp.xml: clip to tile 1 / 1) and STREAM_TERMINATE."""
    g = lambda k: G3VA + G3[k]
    map_g3(w32, pa)
    # free list: PM_PAGES physical pages, top = n - 1 (descriptor CMDT+0x20 =
    # T74: +0x1c base -> RGX 0x210, +0x28 -> 0x220, +0x40 top -> 0x224,
    # +0x48 / +0x4c -> 0x390 / 0x394; fw 0x17a10)
    pfns = [(pa["G3"] + G3["pages"] + 0x1000 * i) >> 12 for i in range(PM_PAGES)]
    iface.writemem(pa["G3"] + G3["fl"], struct.pack(f"<{PM_PAGES}I", *pfns))
    w32("T74", 0x1c, g("fl"))
    w32("T74", 0x20, 0)
    w32("T74", 0x28, 0)
    w32("T74", 0x40, PM_PAGES - 1)
    # slot: MLIST, macrotile array, VHEAP table
    w32("TAO", 0xa0, g("mlist"))
    w32("TAO", 0xa8, g("mta"))
    w32("TAO", 0xb0, g("vheap"))
    for o in (0xa4, 0xac, 0xb4):
        w32("TAO", o, 0)
    # 32-bit block: 0xc78, 0xc88 PPP_CTRL, 0xc98 PPP_SCREEN, 0xc18 TE_SCREEN,
    # 0xc08 / 0xc10 TE_MTILE1 / 2, 0xc20 macrotile stride, 0xc28 TE_PSG
    for i, x in enumerate((0x88, 0x2, 0x003f003f, 0x1001, 0x10100c, 0x10100c, 16, 0x20000)):
        w32("TAA", 4 * i, x)
    # 64-bit block (StartTA +0x8)
    b = {0x00: 0x1e3ce508,           # 0xc90 PDS_CTRL
         0x08: g("rgn"),             # 0xc38 region headers
         0x10: g("tpc"),             # 0xc40 tail pointer cache
         0x18: 0,                    # 0xc00 TE_AA (1 sample)
         0x20: 0x88,                 # 0xc80 PPP_MULTISAMPLECTL
         0x28: g("ctx") + 0x100,     # 0x428
         0x30: g("ctx") + 0x200,     # 0xcc0
         0x68: int(os.environ.get("TA_470", "0x600" if "--tri" in sys.argv else
                                  "0x1880" if "--clear" in sys.argv else "0x100"), 0),   # 0x470: a PDS program (offset in the code heap)
         0x70: 0,                    # 0x418 VDM call stack pointer
         0x78: GVA + 0x3c00}         # 0x408 VDM stream
    for o, x in b.items():
        w32("TAB", o, x)
        w32("TAB", o + 4, x >> 32)
    # the code heap (PDS base = USC base = its VA: P+0x10 / P+0x18, given
    # on the command line): PDS HALT words throughout, so any PDS program
    # the TA starts ends at once
    iface.writemem(pa["G3"] + G3["code"], struct.pack("<I", 0xd3000000) * (0x10000 // 4))
    if "--tri" in sys.argv:
        tri_setup(w32, va, pa)
    if "--clear" in sys.argv:
        clear_setup(w32, va, pa)
    # PPP terminate state: header pres_terminate, TERMINATE0 clip right /
    # bottom tile 1, TERMINATE1 0
    for i, x in enumerate((0x02000000, 0x00040001, 0)):
        w32("G3", G3["ppp"] + 4 * i, x)
    pv = g("ppp")
    for i, x in enumerate(((3 << 8) | (pv >> 32), pv & 0xfffffffc, 0xc0000000)):
        w32("GV", 0x3c00 + 4 * i, x)


CLEAR_HEAP = os.path.join(HERE, "../../build/gpu/ioscap/rclear/done/6060f4000-10000.bin")
CLEAR_COLOUR = [float(x) for x in os.environ.get("CLEAR", "0.25,0.5,0.75,1.0").split(",")]


def clear_setup(w32, va, pa):
    """--clear: the background object and the end-of-tile program of the
    iPadOS clear capture (build/gpu/ioscap/rclear, docs/gpu.md "G6 step 3"):
    its 64 KiB PDS / USC heap copied to ours at the same offsets (PDS base =
    USC base = G3VA), so the record's offsets hold: background
    PDS 0x2200 (DOUTU -> USC 0x20c0) / 0x2180 (DOUTD of the uniform data),
    end-of-tile PDS 0x2500 (DOUTD of the image state, DOUTU -> USC 0x2440).
    The PDS data the capture did not hold is ours: heap+0x3c030 {colour VA,
    0x90000004}, heap+0x3c040 {image state VA, 0x90000007, DOUTU 0x2440 /
    temps}; the colour (4 floats) and the 7-word image state (BGRA8 linear
    64x64, 256-byte rows, address >> 1 in word 2) at G3+0x68000 / +0x68010."""
    # only the window with these four programs and their two USC shaders;
    # the rest stays PDS HALT (with the whole heap the TA's zero-offset PDS
    # tasks ran Apple's code at offset 0: a USC fetch fault, G6 3D run 18)
    lo, hi = (int(x, 0) for x in os.environ.get("CLEAR_WIN", "0x2000,0x2580").split(","))
    heap = open(CLEAR_HEAP, "rb").read()
    iface.writemem(pa["G3"] + G3["code"] + lo, heap[lo:hi])
    co = int(os.environ.get("CLEAR_DATA", "0x68000"), 0)
    cva = G3VA + co
    dva = G3VA + co + 0x10
    iface.writemem(pa["G3"] + co, struct.pack("<4f", *CLEAR_COLOUR))
    img = [0xfb050502, 0x00007e01, (G3VA + G3["rt"]) >> 1, 0x00018000 | (int(os.environ.get("IMG3_HI", "0x80000000"), 0)),
           0, 0xc0000000, 0]
    iface.writemem(pa["G3"] + co + 0x10, struct.pack("<7I", *img))
    d = pa["G3"] + G3["code"] + 0x3c030
    iface.writemem(d, struct.pack("<QQ", cva, 0x90000004))
    iface.writemem(d + 0x10, struct.pack("<QQII", dva, 0x90000007, 0x2440, 0x10))
    # the render target: a marker pattern, so written pixels show
    iface.writemem(pa["G3"] + G3["rt"], struct.pack("<I", int(os.environ.get("RT_FILL", "0x5a5a5a5a"), 0)) * 0x1000)


TRI_CAP = os.path.join(HERE, "../../build/gpu/ioscap/rtri2/done")


def tri_setup(w32, va, pa):
    """--tri (with --pm --clear): the one-triangle draw of the iPadOS `tri`
    capture (build/gpu/ioscap/rtri2, docs/gpu.md "G6 step 4") replayed
    from our own VDM stream. The capture's low GPU window (PDS base 0) is
    rebuilt at our PDS base: its PDS / USC heap (CPU 0xcda0dc000) at +0, its
    PDS data regions 0x30000 (0xcda158000) and 0x3c000 (0xcda160000); the
    PPP state blocks the draw names (in 0x1_00008000 = 0xcda0ec000,
    0x1_0013c000, 0x1_000f4000, 0x1_00148000) are copied into G3+0x6c000
    and the stream points there; the vertex shader's six constants (the
    triangle's corners) at G3+0x6c800, the vertex PDS data's DMA address
    patched to them."""
    def cap(name):
        return open(os.path.join(TRI_CAP, name), "rb").read()
    code = pa["G3"] + G3["code"]
    heap = cap("cda0dc000-10000.bin")
    lo = int(os.environ.get("TRI_HEAP_LO", "0x40"), 0)
    iface.writemem(code + lo, heap[lo:])
    iface.writemem(code + 0x30000, cap("cda158000-8000.bin"))
    iface.writemem(code + 0x3c000, cap("cda160000-8000.bin"))
    t = 0x6c000
    tva = G3VA + t
    consts = struct.pack("<6f", *[float(x) for x in os.environ.get(
        "TRI_XY", "-1,-1,0,-1,-1,1").split(",")])
    iface.writemem(pa["G3"] + t + 0x800, consts)
    iface.writemem(code + 0x302c0, struct.pack("<Q", tva + 0x800))
    if os.environ.get("TRI_VS_USC"):        # experiment: the vertex shader's USC offset (DOUTU at 0x302d0)
        iface.writemem(code + 0x302d0, struct.pack("<I", int(os.environ["TRI_VS_USC"], 0)))
    blocks = [
        ("var", [0x001c0000, 0, 0, 0]),
        ("frag", [0x000000c0, 0x000001c0, 0x000000c0, 0x00800000, 0x00000000, 0x00000140]),
        ("os", [0x00020000, 0x04010000]),
        ("isp", [0x0000001f, 0x03c40000, 0x003e0f00, 0x0e000000, 0x00100f00, 0x00000000]),
        ("vp", [0x00000c00, 0x80000001, 0x00000001, 0x42000000, 0x42000000, 0x42000000,
                0xc2000000, 0x00000000, 0x3f800000]),
        ("ctrl", [0x00210000, 0x1e3ce508, 0x000004a0]),
        ("dbsc", [0x00000020, 0x00000000]),
        ("so", [0x00c00000, 0x00000001, 0x00004022, 0x00030040]),
        ("term", [0x02000000, 0x00040001, 0x00000000]),
    ]
    at = {}
    off = 0x100
    for name, words in blocks:
        at[name] = (tva + off, len(words))
        iface.writemem(pa["G3"] + t + off, struct.pack(f"<{len(words)}I", *words))
        off += 0x40

    def ppp(name):
        a, n = at[name]
        return [(n << 8) | (a >> 32), a & 0xfffffffc]
    vs_code = int(os.environ.get("TRI_VS_CODE", "0x440"), 0)
    stream = ([0x44002f80, vs_code, 0x00000004, 0x00001081] + ppp("var")
              + [0x20020001, 0x000302c0, 0x000003c0, 0x48002f80, 0x000302d0]
              + ppp("frag") + ppp("os") + ppp("isp") + ppp("vp") + ppp("ctrl") + ppp("dbsc") + ppp("so")
              + [0x6e440600, 0x00000003, 0x00000000, 0x00000000, 0x000000aa]
              + ppp("term") + [0xc0000000])
    iface.writemem(pa["G3"] + t, struct.pack(f"<{len(stream)}I", *stream))
    w32("TAB", 0x78, tva)
    w32("TAB", 0x7c, 0)


def ta_probe_report(pa):
    for i, (r, o) in enumerate(TA_SLOTS):
        d = iface.readmem(pa["PMB"] + TA_PROBE_STRIDE * i, TA_PROBE_STRIDE)
        nz = [j for j in range(0, len(d), 4) if d[j:j + 4] != b"\0\0\0\0"]
        if nz:
            w = struct.unpack_from("<8I", d, nz[0] & ~0x1f)
            out(f"probe {r}+{o:#x} (VA {TA_PROBE_VA + TA_PROBE_STRIDE * i:#x}): {len(nz) * 4:#x} bytes written, "
                f"first +{nz[0]:#x}, last +{nz[-1]:#x}: " + " ".join(f"{x:08x}" for x in w))


G3VA = 0x400000      # GPU VA of region G3 (--kick-3d)


def map_pages(w32, pa, ptr, va0, region, npages, off=0):
    """GPU VA va0.. (inside one 2 MiB page table `ptr`) onto `region`+off.."""
    ents = b"".join(struct.pack("<Q", (pa[region] + off + 0x1000 * k) | 1) for k in range(npages))
    iface.writemem(pa[ptr] + 8 * ((va0 >> 12) & 0x1ff), ents)


def map_g3(w32, pa):
    """Page tables: PD entry 2 (VA 0x400000..0x5fffff) -> PTR2 -> G3; with
    --va0, PD entry 0 -> PTR0, VA 0 -> page VA0 (a page at GPU VA 0)."""
    for pde_i, ptr in ((2, "PTR2"),) + (((0, "PTR0"),) if "--va0" in sys.argv else ()):
        w32("PDR", 8 * pde_i, (pa[ptr] & 0xffffffff) | 1)
        w32("PDR", 8 * pde_i + 4, pa[ptr] >> 32)
    map_pages(w32, pa, "PTR2", G3VA, "G3", 0x100)
    if "--va0" in sys.argv:
        map_pages(w32, pa, "PTR0", 0, "VA0", 1)


def build_3d(w32, va, pa):
    """G6 step 2: a 3D job on the 3D channel (1) with its own queue (K3): a
    type-1 command (CMD3: +0x14 / +0x18 microsequence, +0x1c context state,
    +0x20 free-list descriptor, +0x24 PM slot, +0x89c / +0x8a4 completion
    record) and Start3D (op 7, 0x78) -> WaitIRQ (PIXELBE_END_RENDER, bit 3)
    -> Finalize3D (op 8, 0x50) -> End. Stamp: STAMP+0x40 = 1; the
    command's completion record STAMP+0x50 = 0x3d. GPU VA 0x400000.. is
    region G3 (1 MiB). docs/gpu.md "G6 step 2"."""
    map_g3(w32, pa)
    # the queue on channel 1 = group 0, 3D: ring W+0x1400, host write index G+0x14
    ring = pa["W"] + 0x1400 * 1
    iface.writemem(ring, struct.pack("<IIIIHBB", 0, 0, va["K3"], 1, 1, 0, 0))
    # the host write index (G+0x14) is set by the caller right before the
    # doorbell: the firmware scans the channels whenever it wakes (the TA's
    # completion does it), and saw a half-written command (CMD3+0x14 = 0,
    # a null microsequence, G6 3D run 2)
    w32("K3", 0x00, va["QS3"])
    w32("K3", 0x04, va["CR3"])
    w32("K3", 0x0c, va["SE3"])
    w32("K3", 0x44, 1)
    w32("K3", 0x88, va["ST3"])
    w32("QS3", 0x50, 256)
    w32("CR3", 0x00, va["CMD3"])
    # PM slot: the TA's (TAO) when the TA runs first, so the 3D reads what
    # the TA left; its +0xc0 is the slot header (the post-processing callback
    # fw 0x1468c writes it)
    slot = "TAO"
    w32(slot, 0xc0, va["PMH"])
    if os.environ.get("CAT3D"):
        # experiment: a hand-made PM page catalog in the slot at the offset
        # CAT3D names (Start3D loads it into an RGX 0xe0xx register, fw
        # 0x17606): PC (G3+0x20000) entry 0 -> PD (+0x21000) entry 0 -> PT
        # (+0x22000) entry 0 -> the page G3+0x23000 (filled with CAT3D_FILL)
        base = pa["G3"] + 0x20000
        iface.writemem(base, bytes(0x4000))
        w32("G3", 0x20000, ((base + 0x1000) >> 12 << 4) | 1)
        for lvl in (1, 2):
            nxt = base + 0x1000 * (lvl + 1)
            w32("G3", 0x20000 + 0x1000 * lvl, (nxt & 0xffffffff) | 1)
            w32("G3", 0x20004 + 0x1000 * lvl, nxt >> 32)
        fill = int(os.environ.get("CAT3D_FILL", "0"), 0)
        iface.writemem(base + 0x3000, struct.pack("<I", fill) * 0x400)
        o = int(os.environ["CAT3D"], 0)
        w32(slot, o, (base & 0xffffffff) | 1)
        w32(slot, o + 4, base >> 32)
    # the free-list descriptor: the TA command's (CMDT+0x20, T74). The TA's
    # dispatch registers it as free list 0 (fw table 0xd6ca0); Start3D's
    # +0x34 picks the id (fw 0x1866c: a never registered id is a null
    # pointer, G6 3D run 1). Post-processing reads +0x48 and compares
    # *(+0x50) with +0x0 (equal: the registration ends, fw 0x1468c)
    fld = "T74"
    w32(fld, 0x50, va[fld] + 0x80)
    # the command
    c = "CMD3"
    w32(c, 0x000, 1)
    w32(c, 0x01c, va["CTX3"])
    w32(c, 0x020, va[fld])
    w32(c, 0x024, va[slot])
    w32(c, 0x89c, va["STAMP"] + 0x50)
    w32(c, 0x8a4, 0x3d)
    # Start3D's register blocks (docs/gpu.md "3D layouts"); values for a
    # 64x64 target, to be checked one by one
    a = [0] * 18
    a[0] = 0x00040004       # 0xf18
    a[1] = 0x3f800000       # 0xf80: 1.0f (the iPadOS record's +0x5c4)
    a[2] = 0x300            # 0xf88 (record +0x5c8)
    a[3] = 0xf              # (record +0x5cc; not loaded by Start3D)
    a[4] = 0x1f             # 0x6d0
    if "--clear" in sys.argv:
        a[5], a[6], a[7] = 0x2500, 0x3c040, 0x402   # 0x618 / 0x620 / 0x628: end-of-tile PDS code, data, info
    a[8] = 0x0023000f       # 0xf38 ISP_CTL
    a[9] = 0x408            # 0x3e00
    a[10] = 4               # 0xfd8
    a[11] = 0xf9            # 0x4070
    for i, x in enumerate(a):
        w32("S3A", 4 * i, x)
    b = [0] * 29
    b[0] = 0x88             # 0xfa8
    b[10] = 0x14            # 0xf28
    for i in range(22, 28):
        b[i] = 0x1d3        # 0x11080 x 6
    if "--clear" in sys.argv:
        b[11] = (0x2180 << 32) | 0x2200     # 0x6a0 PDS_BGRND0: texunicode 0x2180 / shader 0x2200
        b[13] = 0x3c030                     # 0x6b0 PDS_BGRND2: uniform data
        b[14] = 0x0080000000800000          # 0x6b8 PDS_BGRND3: sizes
    for i, x in enumerate(b):
        w32("S3B", 8 * i, x)
        w32("S3B", 8 * i + 4, x >> 32)
    u = "US3"
    w32(u, 0x00, 7)                 # Start3D, 0x78 bytes
    w32(u, 0x04, va["S3A"])
    w32(u, 0x08, va["S3B"])
    w32(u, 0x0c, va[slot])          # PM slot (+0xa0 / +0xa8 / +0xb8 / +0xbc ...; fw 0x17606)
    w32(u, 0x10, va["UT3"])         # unit table (0x28-byte entries)
    w32(u, 0x14, va["FL3"] + 0x40)  # -> 0 (non-zero: the partial-render path)
    w32(u, 0x18, va["FL3"] + 0x44)  # -> 0
    w32(u, 0x1c, va["FL3"])         # cancel flag (0)
    w32(u, 0x20, 1)                 # -> unit table +0x24
    w32(u, 0x24, va[c])             # the command (+0x24 -> +0xc8 counter on the partial path)
    w32(u, 0x28, 0)                 # context id byte -> RGX+0xf079
    w32(u, 0x34, 0)                 # free-list id (fw 0x1866c), as StartTA +0x2c
    w32(u, 0x48, va["FL3"] + 0x48)  # -> 0 (non-zero: resume, fw 0xe816)
    w32(u, 0x74, va["FL3"] + 0x10)  # threshold for +0x6c / +0x70
    w32("FL3", 0x10, 0xffffffff)
    w32("FL3", 0x14, 0xffffffff)
    w = int(os.environ.get("WAIT3D", "0x8"), 0)
    f = 0x78
    if w:
        w32(u, 0x78, w << 6)        # WaitIRQ: PIXELBE_END_RENDER
        f = 0x7c
    w32(u, f + 0x00, 8)             # Finalize3D, 0x50 bytes
    w32(u, f + 0x0c, va["STAMP"] + 0x40)
    w32(u, f + 0x10, 1)
    w32(u, f + 0x14, va[slot])
    w32(u, f + 0x18, va["F3C"])     # counter block ([0] count, [4] / [5])
    w32(u, f + 0x1c, 1)
    w32(u, f + 0x20, va["UT3"])
    w32(u, f + 0x24, va["FL3"] + 0x20)   # "already finished" (non-zero: wait for PM_3D_MEM_FREE only)
    w32("FL3", 0x20, 1)
    w32(u, f + 0x30, va[c])         # the command (+0x864)
    w32(u, f + 0x38, va["FL3"] + 0x50)
    w32(u, f + 0x48, 0x50)          # skip on the context-store path
    w32(u, f + 0x4c, va["FL3"] + 0x60)
    w32(u, f + 0x50, 2)             # End
    w32(c, 0x014, va[u])
    w32(c, 0x018, f + 0x54)


HEAPVA = int(os.environ.get("HEAPVA_IOS", "0x300000"), 0)   # --store-ios: PDS base = USC base (P+0x10 = P+0x18 = this)


def build_store_ios(w32, va, pa):
    """G5, the iPadOS layout byte for byte (docs/gpu.md "G5 ground truth"): a
    heap at GPU VA 0x300000 (region HP, mapped over --pad's zero page) with
    the 0x140 word at +0, store_const's USC code at +0x40, the DMA PDS
    program at +0x180, the kernel's PDS program at +0x200 and its data at
    +0x280 (EXE_OFF 0x40, TEMPS 2); the DMA data and the argument entry
    (iOS: in other regions) at +0x1000 / +0x1100; the output buffer stays
    GV+0x2800. The stream at GPU VA 0x200000: DMA record, kernel record,
    terminate. Caller sets P+0x10 = P+0x18 = 0x300000."""
    import g9pbin
    for i in range(4):
        pte = 8 * (((HEAPVA >> 12) & 0x1ff) + i)
        page = pa["HP"] + 0x1000 * i
        w32("PTR", pte, (page & 0xffffffff) | 1)
        w32("PTR", pte + 4, page >> 32)
    usc = g9pbin.kernels(os.path.join(HERE, "../../build/gpu/isa/g5.g9p.metallib"))[1]
    usc += bytes(-len(usc) % 4)
    w32("HP", 0x0, 0x140)
    for i in range(0, len(usc), 4):
        w32("HP", 0x40 + i, struct.unpack_from("<I", usc, i)[0])
    for i, w in enumerate((0xf0001000, 0xf4001808, 0xd3000000)):
        w32("HP", 0x180 + 4 * i, w)
    for i, w in enumerate((0xc2c00003, 0xf0000a01, 0xd3000000, 0xf0000e01, 0xfc000002, 0xd3000000)):
        w32("HP", 0x200 + 4 * i, w)
    for i, w in enumerate((0x40, 0x10, 0x90000000, 0x80000000)):
        w32("HP", 0x280 + 4 * i, w)
    out = GVA + 0x2800
    for i in range(0, 0x40, 4):
        w32("GV", 0x2800 + i, 0xabadbeef)
    for i, w in enumerate((HEAPVA + 0x1100, 0, HEAPVA + 0x1110, 0, 0x10000002, 0, 0x90004003, 0)):
        w32("HP", 0x1000 + 4 * i, w)
    for i, w in enumerate((out, 0, 0, 0, 0x00010001)):
        w32("HP", 0x1100 + 4 * i, w)
    w32("HP", 0x2000, 0xd3000000)      # terminate / context-store PDS
    dma = [0x4, 0x1000 | 0x2, 0x180, 0, 0, 0, 0x40000001, 0]
    ker = [0x2103, 0x280, 0x200, 0, 0, 0, 0x40000001, 0]
    for i, w in enumerate(dma + ker + [0x80000000]):
        w32("GV", 4 * i, w)
    w32("CA", 0x08, 0x2000)
    w32("CA", 0x0c, 0x2010)
    w32("CTX", 0x38, 0x2000)
    w32("CTX", 0x3c, 0x2010)


def build_store(w32, va, pa):
    """G5: our store_const kernel writes a buffer, set up the way iPadOS 18.7.9
    does it (tools/gpu/ios/gpucap, docs/gpu.md "G5 ground truth"):
    PDS base = USC base = GPU VA 0x200000 (P+0x10 / P+0x18, set by the
    caller), every PDS / USC address below an offset from it.
      +0x0000  CDM stream: DMA record, kernel record (8 words each), terminate
      +0x1000  HALT (terminate / context-store PDS, CA / CTX point here)
      +0x1100  DMA PDS program, +0x1180 its data (32 bytes)
      +0x1200  kernel PDS program, +0x1280 its data (16 bytes)
      +0x1400  store_const's USC code
      +0x1600  the argument entry {u64 buffer VA, u64 0, u32 0x00010001}
      +0x2800  the output buffer (0xabadbeef, the kernel writes 0xcafe0000)
    """
    import g9pbin
    usc = g9pbin.kernels(os.path.join(HERE, "../../build/gpu/isa/g5.g9p.metallib"))[1]
    usc += bytes(-len(usc) % 4)
    for i in range(0, len(usc), 4):
        w32("GV", 0x1400 + i, struct.unpack_from("<I", usc, i)[0])
    out = GVA + 0x2800
    for i in range(0, 0x40, 4):
        w32("GV", 0x2800 + i, 0xabadbeef)
    # stream: K0 K1 K2 K3 K4 K5 K8 pad; pds_data_size = 16-byte units - 1
    dma = [0x4, 0x1180 | 0x2, 0x1100, 0, 0, 0, 0x40000001, 0]      # usc_common_shared
    ker = [0x2103, 0x1280, 0x1200, 0, 0, 0, 0x40000001, 0]         # fence, ANY, temps 1, unified 2
    for i, w in enumerate(dma + ker + [0x80000000]):
        w32("GV", 4 * i, w)
    for i, w in enumerate((0xf0001000, 0xf4001808, 0xd3000000)):
        w32("GV", 0x1100 + 4 * i, w)
    for i, w in enumerate((GVA + 0x1600, 0, GVA + 0x1610, 0, 0x10000002, 0, 0x90004003, 0)):
        w32("GV", 0x1180 + 4 * i, w)
    for i, w in enumerate((0xc2c00003, 0xf0000a01, 0xd3000000, 0xf0000e01, 0xfc000002, 0xd3000000)):
        w32("GV", 0x1200 + 4 * i, w)
    for i, w in enumerate((0x1400, 0x10, 0x90000000, 0x80000000)):    # EXE_OFF, TEMPS 2
        w32("GV", 0x1280 + 4 * i, w)
    for i, w in enumerate((out, 0, 0, 0, 0x00010001)):
        w32("GV", 0x1600 + 4 * i, w)
    w32("GV", 0x1000, 0xd3000000)
    # terminate / context-store PDS: now offsets from the PDS base too
    w32("CA", 0x08, 0x1000)
    w32("CA", 0x0c, 0x1010)
    w32("CTX", 0x38, 0x1000)
    w32("CTX", 0x3c, 0x1010)


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
    # P+0x0..0xc: {u64 base, u64 size} -> the four BIF tiling ranges the
    # firmware programs at power-up (fw 0x5660: RGX+0x12e0 / 0x12f0 / 0x1300 /
    # 0x1310, upstream ROGUE_CR_BIF_TILING_CFG*, enabled). Left 0 the range
    # ends at -1: the whole GPU VA space is tiled and every access is
    # swizzled (a stream at 0x200800 read at 0x208000; a phantom PDS read at
    # 0x201100; no USC task: G5 runs 27-96). Keep it far above our VAs.
    w32("P", 0x0, 0x00000000)
    w32("P", 0x4, 0x6)              # base 0x6_0000_0000
    w32("P", 0x8, 0xffc00000)       # size
    w32("P", 0xc, 0)
    if "--store" in sys.argv:
        w32("P", 0x10, GVA)         # PDS base (RGX+0x610)
        w32("P", 0x18, GVA)         # USC base (RGX+0x4008 / 0x4010 / 0x4028)
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
                      "CDR", "UT", "FL", "GV", "PDR", "PTR", "HP"):
                w32(r2, int(o2, 0), va[v2[1:]] if v2.startswith("@") else pa[v2[1:]] if v2.startswith("%") else int(v2, 0))
        out("kick: compute channel 2, write index 1, doorbell (0x21 << 56) | (3 << 48) | 2")
        kf.send((0x21 << 56) | (3 << 48) | 2)
    if "--kick-ta" in sys.argv:
        try:
            kf.work_for(2.0)
        except Exception as e:
            out(f"crashed before the kick: {e!r}")
        if "--job" in sys.argv and "--kick-cp" not in sys.argv:
            build_job(w32, va, pa)          # the GPU page tables, the flush fence
        build_ta(w32, va, pa)
        for s2 in sets:
            k2, v2 = s2.split("=")
            r2, o2 = k2.split("+")
            if r2 in ("KT", "QST", "CRT", "SET", "STT", "CMDT", "UST", "CTXT", "T74", "TBIG", "TAA",
                      "TAB", "TAO", "UT2", "FL2", "TAC", "CTX2", "GV", "PTR", "STAMP"):
                w32(r2, int(o2, 0), va[v2[1:]] if v2.startswith("@") else pa[v2[1:]] if v2.startswith("%") else int(v2, 0))
        out("kick: TA channel 0, write index 1, doorbell (0x21 << 56) | (3 << 48) | 0")
        kf.send((0x21 << 56) | (3 << 48) | 0)
    if "--kick-3d" in sys.argv:
        try:
            if "--kick-ta" in sys.argv:
                # the TA first: wait for its completion record (STAMP+0x30)
                t0 = time.time()
                while time.time() - t0 < float(os.environ.get("TA_WAIT", "2")) and p.read32(pa["STAMP"] + 0x30) == 0:
                    kf.work_for(0.01)
                out(f"TA: STAMP+0x30 = {p.read32(pa['STAMP'] + 0x30):#x} after {time.time() - t0:.2f} s")
            else:
                kf.work_for(2.0)
        except Exception as e:
            out(f"crashed before the 3D kick: {e!r}")
        if "--job" in sys.argv and "--kick-cp" not in sys.argv and "--kick-ta" not in sys.argv:
            build_job(w32, va, pa)          # the GPU page tables, the flush fence
        build_3d(w32, va, pa)
        for s2 in sets:
            k2, v2 = s2.split("=")
            r2, o2 = k2.split("+")
            if r2 in ("K3", "QS3", "CR3", "SE3", "ST3", "CMD3", "US3", "CTX3", "S3A", "S3B", "UT3", "FL3",
                      "F3C", "FLD", "T74", "PMH", "TAO", "G3", "PTR2", "PTR0", "VA0", "STAMP"):
                w32(r2, int(o2, 0), va[v2[1:]] if v2.startswith("@") else pa[v2[1:]] if v2.startswith("%") else int(v2, 0))
        w32("G", 0x10 * 1 + 4, 1)
        out("kick: 3D channel 1, write index 1, doorbell (0x21 << 56) | (3 << 48) | 1")
        kf.send((0x21 << 56) | (3 << 48) | 1)
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
        if "--kick-3d" in sys.argv:
            out(f"STAMP+0x40 = {p.read32(pa['STAMP'] + 0x40):#x} (Finalize3D: 1), "
                f"+0x50 = {p.read32(pa['STAMP'] + 0x50):#x} (the 3D command's completion record: 0x3d)")
        if "--pm" in sys.argv:
            if True:
                for k, o in G3.items():
                    if k in ("code", "pages"):
                        continue
                    n = 0x1000
                    words = struct.unpack(f"<{n // 4}I", iface.readmem(pa["G3"] + o, n))
                    nz = [(i * 4, x) for i, x in enumerate(words) if x]
                    out(f"G3 {k} (+{o:#x}): {len(nz)} words: " + " ".join(f"+{a:#x}={x:#x}" for a, x in nz[:32]))
                rtb = iface.readmem(pa["G3"] + G3["rt"], 0x4000)
                if os.environ.get("RT_OUT"):
                    open(os.environ["RT_OUT"], "wb").write(rtb)
                rt = struct.unpack("<4096I", rtb)
                hist = {}
                for x in rt:
                    hist[x] = hist.get(x, 0) + 1
                out("render target: " + ", ".join(f"{x:#010x} x{c}" for x, c in sorted(hist.items(), key=lambda t: -t[1])[:8])
                    + f"; px(0,0) {rt[0]:#010x} px(63,0) {rt[63]:#010x} px(0,63) {rt[63 * 64]:#010x} px(63,63) {rt[4095]:#010x}")
                used = [i for i in range(PM_PAGES) if any(iface.readmem(pa["G3"] + G3["pages"] + 0x1000 * i, 0x1000))]
                out(f"PM pages written: {used}")
                for i in used[:6]:
                    words = struct.unpack("<1024I", iface.readmem(pa["G3"] + G3["pages"] + 0x1000 * i, 0x1000))
                    nz = [(j * 4, x) for j, x in enumerate(words) if x]
                    out(f"  page {i}: {len(nz)} words: " + " ".join(f"+{a:#x}={x:#x}" for a, x in nz[:24]))
            for k in ("UT3", "UT2", "TAO", "PMH", "T74", "F3C", "FL3", "CMD3"):
                words = struct.unpack("<1024I", iface.readmem(pa[k], 0x1000))
                nz = [(i * 4, x) for i, x in enumerate(words) if x]
                out(f"{k}: " + " ".join(f"+{o:#x}={x:#x}" for o, x in nz[:40]))
        if "--kick-ta" in sys.argv:
            out(f"STAMP+0x20 = {p.read32(pa['STAMP'] + 0x20):#x} (FinalizeTA: 1), "
                f"+0x30 = {p.read32(pa['STAMP'] + 0x30):#x} (the TA command's completion record: 0x7a)")
            if "--ta-probe" in sys.argv:
                ta_probe_report(pa)
        out(f"STAMP+0 = {p.read32(pa['STAMP']):#x} (FinalizeCompute: 1), "
            f"+0x10 = {p.read32(pa['STAMP'] + 0x10):#x} (the command's completion record: 0xc0de)")
        if "--store" in sys.argv or "--store-ios" in sys.argv:
            outw = struct.unpack("<8I", iface.readmem(pa["GV"] + 0x2800, 0x20))
            out("output buffer GV+0x2800: " + " ".join(f"{x:08x}" for x in outw)
                + "  (store_const: 0xcafe0000 + thread id; 0xabadbeef = untouched)")
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
    if "--rgx" in sys.argv:
        # stop the coprocessor first, so nothing changes the GPU's power any
        # more, then read RGX only if the core was left on (rgx_on())
        p.write32(gb.FENDER + 0x2048, p.read32(gb.FENDER + 0x2048) & ~1)
        time.sleep(0.02)
        if rgx_on():
            extra = [int(x, 0) for x in os.environ.get("RGX_READ", "").replace(",", " ").split()]
            for off in [0x4a0, 0x480, 0x478, 0xe150, 0xe154, 0xe238, 0xe240] + extra:
                out(f"RGX +{off:#x} = {p.read32(RGX_PA + off):#010x}")
        else:
            out(f"--rgx: firmware stopped with the GPU off (Fender +0x1000 = "
                f"{p.read32(gb.FENDER + 0x1000):#x}); RGX not read")
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
