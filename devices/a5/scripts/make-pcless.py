#!/usr/bin/env python3
"""The files iBEC's `upgrade` boot loads from /boot, for the PC-less route.

    make-pcless.py --ipsw IPSW --bundle staging-bundle.bin \\
                   --devicetree DEVICETREE --out DIR

writes DIR/kernelcache, DIR/ramdisk and DIR/devicetree, plaintext img3s (iBEC
started by kloader has no GID key, so a KBAG could not be opened).  The plan
and the reverse engineering: docs/research/p105-pcless-boot.md.

kernelcache   The bundle ./cascadia a5 build makes for USB (loader at 0, kernel
              at 0x8000, the machine's layer behind it), unchanged, as the one
              segment of a Mach-O at 0x80000000 with its entry at the loader,
              in Apple's complzss wrapper.  The stored LZSS is all literals:
              a zImage does not compress, and iBEC's decoder takes it as it is.
ramdisk       iBEC refuses to go on without one; a page of zeroes.  The layer
              already travels in the bundle.
devicetree    iBEC requires one and fills it in; Linux never reads it.  Either
              a plaintext img3 or a raw ADT.

The containers reuse the tags of the IPSW's own images (scripts/img3pack.py).

Limits from iBEC iBoot-2261.30.37: the img3 is loaded at 0x84000000, at most
0x3f00000 bytes; the compressed data is copied to the heap, whose large chunk
is 48 MiB at 0x8c000000 and also holds the NAND driver's tables; the Mach-O
must not reach the load buffer at 0x84000000.
"""
from __future__ import annotations

import argparse
import struct
import sys
import zipfile
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from img3pack import pack_plain, parse_tags  # noqa: E402

VMADDR = 0x80000000
SEG_FILEOFF = 0x1000
COMP_DATA_OFF = 0x180
LOAD_MAX = 0x3F00000
HEAP_ROOM = 40 << 20              # of the 48 MiB chunk, the rest for iBEC itself
MACHO_END_LIMIT = 0x84000000

MH_MAGIC = 0xFEEDFACE
CPU_TYPE_ARM = 12
CPU_SUBTYPE_ARM_V7 = 9
MH_EXECUTE = 2
MH_NOUNDEFS = 1
LC_SEGMENT = 1
LC_UNIXTHREAD = 5
ARM_THREAD_STATE = 1
ARM_THREAD_STATE_COUNT = 17
PC_INDEX = 15


def macho(bundle: bytes, entry: int = VMADDR) -> bytes:
    """A 32-bit ARM Mach-O: one segment holding the bundle, pc = entry."""
    vmsize = (len(bundle) + 0xFFF) & ~0xFFF
    seg = struct.pack("<II16sIIIIIIII", LC_SEGMENT, 56, b"__TEXT", VMADDR, vmsize,
                      SEG_FILEOFF, len(bundle), 7, 5, 0, 0)
    state = [0] * ARM_THREAD_STATE_COUNT
    state[PC_INDEX] = entry
    state[16] = 0x1D3                 # cpsr: SVC, IRQ/FIQ masked, ARM
    thread = struct.pack("<IIII", LC_UNIXTHREAD, 16 + 4 * len(state),
                         ARM_THREAD_STATE, ARM_THREAD_STATE_COUNT)
    thread += struct.pack("<%dI" % len(state), *state)
    cmds = seg + thread
    head = struct.pack("<IIIIIII", MH_MAGIC, CPU_TYPE_ARM, CPU_SUBTYPE_ARM_V7,
                       MH_EXECUTE, 2, len(cmds), MH_NOUNDEFS)
    if len(head) + len(cmds) > SEG_FILEOFF:
        raise SystemExit("Mach-O header does not fit before the segment")
    return head + cmds + b"\0" * (SEG_FILEOFF - len(head) - len(cmds)) + bundle


def lzss_literal(data: bytes) -> bytes:
    """LZSS as Apple's decoder reads it, every byte a literal: 0xff + 8 bytes."""
    out = bytearray(len(data) + len(data) // 8 + 1)
    w = 0
    for r in range(0, len(data), 8):
        chunk = data[r:r + 8]
        out[w] = 0xFF
        out[w + 1:w + 1 + len(chunk)] = chunk
        w += 1 + len(chunk)
    return bytes(out[:w])


def complzss(plain: bytes) -> bytes:
    body = lzss_literal(plain)
    head = struct.pack(">4s4sIII", b"comp", b"lzss", zlib.adler32(plain) & 0xFFFFFFFF,
                       len(plain), len(body))
    return head + b"\0" * (COMP_DATA_OFF - len(head)) + body


def check_limits(bundle: bytes, image: bytes, comp: bytes) -> None:
    body = len(comp) - COMP_DATA_OFF
    end = VMADDR + ((len(bundle) + 0xFFF) & ~0xFFF)
    if end > MACHO_END_LIMIT:
        raise SystemExit("the bundle would reach iBEC's load buffer at 0x84000000")
    if len(image) > LOAD_MAX or len(comp) > LOAD_MAX:
        raise SystemExit(f"{len(comp)} bytes: over the {LOAD_MAX:#x} iBEC loads")
    if body > HEAP_ROOM:
        raise SystemExit(f"{body >> 20} MiB compressed: more than iBEC's heap can hold")


def ident(img3: bytes) -> bytes:
    return img3[16:20][::-1]


def tag_data(img3: bytes, tag: bytes) -> bytes:
    for off, t, _total, dlen in parse_tags(img3):
        if t == tag:
            return img3[off + 12:off + 12 + dlen]
    raise SystemExit(f"no {tag[::-1]!r} tag")


def devicetree(path: Path, template: bytes) -> bytes:
    d = path.read_bytes()
    if d[:4] != b"3gmI":
        return pack_plain(template, d)          # a raw ADT
    if ident(d) != b"dtre":
        raise SystemExit(f"{path} is an img3 of type {ident(d)!r}, not dtre")
    if any(t == b"GABK" for _, t, _, _ in parse_tags(d)):
        raise SystemExit(f"{path} is encrypted: iBEC cannot open it after kloader")
    return pack_plain(d, tag_data(d, b"ATAD"))


def ipsw_img3(z: zipfile.ZipFile, want: bytes, names: list[str]) -> bytes:
    for name in names:
        with z.open(name) as f:
            if f.read(20)[16:20][::-1] == want:
                return z.read(name)
    raise SystemExit(f"no {want!r} img3 among {names}")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ipsw", type=Path, required=True)
    ap.add_argument("--bundle", type=Path, required=True)
    ap.add_argument("--devicetree", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    a = ap.parse_args(argv)

    with zipfile.ZipFile(a.ipsw) as z:
        small_dmgs = [i.filename for i in z.infolist()
                      if i.filename.endswith(".dmg") and i.file_size < 100 << 20]
        dt_names = [n for n in z.namelist() if n.endswith("DeviceTree.p105ap.img3")]
        krnl_t = ipsw_img3(z, b"krnl", ["kernelcache.release.p105"])
        rdsk_t = ipsw_img3(z, b"rdsk", small_dmgs)
        dtre_t = ipsw_img3(z, b"dtre", dt_names)

    bundle = a.bundle.read_bytes()
    image = macho(bundle)
    comp = complzss(image)
    check_limits(bundle, image, comp)

    a.out.mkdir(parents=True, exist_ok=True)
    outputs = {
        "kernelcache": pack_plain(krnl_t, comp),
        "ramdisk": pack_plain(rdsk_t, b"\0" * 4096),
        "devicetree": devicetree(a.devicetree, dtre_t),
    }
    for name, data in outputs.items():
        if any(t == b"GABK" for _, t, _, _ in parse_tags(data)):
            raise SystemExit(f"{name}: a KBAG survived")
        (a.out / name).write_bytes(data)
        print(f"  {name:<12} {ident(data).decode()}  {len(data):>10} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
