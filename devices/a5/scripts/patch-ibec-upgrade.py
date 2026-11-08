#!/usr/bin/env python3
"""Patch iBEC to boot Linux from the NAND by itself: the PC-less route.

    patch-ibec-upgrade.py IBEC_PATCHED OUT

IBEC_PATCHED is the plaintext iBEC after iBoot32Patcher (signature checks
off), as ./cascadia a5 firmware leaves it in build/firmware/iBEC.patched;
OUT is the raw image kloader starts from iOS.  The plan and the reverse
engineering behind every address: docs/research/p105-pcless-boot.md.

iBEC already carries iBoot's NAND / LwVM / HFS+ stack and one boot command,
"upgrade" (the OTA path), which mounts the LwVM partition named "Update" and
loads /boot/devicetree, /boot/ramdisk and /boot/kernelcache from it.  The
patches make that the boot, from System, and keep it read-only on the NAND:

  autoboot    boot whatever NVRAM says about auto-boot (bne -> nop)
  command     the boot command is always "upgrade": the literal the
              dispatcher reads becomes the string itself, the getenv() goes
  partition   the partition iBEC treats as "Update" is the one named "System"
              (HFS+, unencrypted, writable from the jailbroken iOS)
  no-saveenv  upgrade's first act is unsetenv(boot-command) + saveenv();
              the saveenv() goes, so a boot never writes NVRAM
  no-record   on a failure upgrade stores ota-result / ota-failure-reason and
              saves them; that goes too -- a failed boot just reboots to iOS

Every site is checked against the expected original instruction (or the
patched one, so running it twice is harmless); anything else stops.
Offsets: iBEC iBoot-2261.30.37, P105, iOS 8.4.1 12H321, base 0x9ff00000.
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

BASE = 0x9FF00000
NOP16 = b"\x00\xbf"
NOP32 = NOP16 * 2

STR_UPGRADE = 0x9FF347E7          # "upgrade\0"
STR_BOOT_COMMAND = 0x9FF34828     # "boot-command\0"
SITE_AUTOBOOT = 0x9FF00CB2        # bne 0x9ff00d24 (auto-boot != true)
SITE_CMD_LDR = 0x9FF00D00         # ldr r0, [pc, #0x12c] -> "boot-command"
LIT_CMD = 0x9FF00E30
SITE_CMD_GETENV = 0x9FF00D02      # bl getenv (0x9ff17a18)
GETENV = 0x9FF17A18
NAME_UPDATE = 0x9FF439E4          # u"Update\0", compared over 14 bytes
SITE_SAVEENV = 0x9FF01486         # bl saveenv (0x9ff1c358)
SAVEENV = 0x9FF1C358
SITE_RECORD = 0x9FF01504          # bl record_upgrade_failure (0x9ff01438)
RECORD = 0x9FF01438
UPGRADE_HANDLER = 0x9FF0147C
BOOT_TABLE = 0x9FF44040           # {"upgrade", handler}, ...


def off(va: int) -> int:
    return va - BASE


def thumb_bl(at: int, target: int) -> bytes:
    """Encode a Thumb-2 BL at `at` to `target`."""
    d = target - (at + 4)
    if d & 1 or not -(1 << 24) <= d < (1 << 24):
        raise ValueError("BL out of range")
    s = (d >> 24) & 1
    i1 = (d >> 23) & 1
    i2 = (d >> 22) & 1
    imm10 = (d >> 12) & 0x3FF
    imm11 = (d >> 1) & 0x7FF
    j1 = (~(i1 ^ s)) & 1
    j2 = (~(i2 ^ s)) & 1
    return struct.pack("<HH", 0xF000 | (s << 10) | imm10, 0xD000 | (j1 << 13) | (j2 << 11) | imm11)


def thumb_bcond(at: int, cond: int, target: int) -> bytes:
    d = (target - (at + 4)) >> 1
    if not -128 <= d < 128:
        raise ValueError("B<cond> out of range")
    return struct.pack("<H", 0xD000 | (cond << 8) | (d & 0xFF))


def ldr_lit_refs(img: bytes, lit_va: int) -> list[int]:
    """Every Thumb LDR (literal) in the image that loads the word at lit_va."""
    refs = []
    for o in range(0, len(img) - 4, 2):
        hw = struct.unpack_from("<H", img, o)[0]
        pc = ((BASE + o + 4) & ~3)
        if hw >> 11 == 0b01001 and pc + (hw & 0xFF) * 4 == lit_va:
            refs.append(BASE + o)
        elif hw == 0xF8DF:
            imm = struct.unpack_from("<H", img, o + 2)[0] & 0xFFF
            if pc + imm == lit_va:
                refs.append(BASE + o)
    return refs


def cstr(img: bytes, va: int) -> bytes:
    o = off(va)
    return img[o:img.index(b"\0", o)]


def site(img: bytearray, name: str, va: int, orig: bytes, new: bytes) -> str:
    cur = bytes(img[off(va):off(va) + len(orig)])
    if cur == new:
        return f"{name:<11} {va:#x}: already patched"
    if cur != orig:
        raise SystemExit(f"{name}: unexpected bytes at {va:#x}: {cur.hex()} (want {orig.hex()})")
    img[off(va):off(va) + len(new)] = new
    return f"{name:<11} {va:#x}: {orig.hex()} -> {new.hex()}"


def patch(img: bytearray) -> list[str]:
    if img[0x200:0x20D] != b"iBEC for p105" or not img[0x280:0x290].startswith(b"iBoot-2261.30.37"):
        raise SystemExit("not the iBEC this patcher knows (iBEC for p105, iBoot-2261.30.37)")
    if cstr(img, STR_UPGRADE) != b"upgrade" or cstr(img, STR_BOOT_COMMAND) != b"boot-command":
        raise SystemExit("string anchors moved")
    table = struct.unpack_from("<II", img, off(BOOT_TABLE))
    if table != (STR_UPGRADE, UPGRADE_HANDLER | 1):
        raise SystemExit(f"boot-command table is {table[0]:#x}/{table[1]:#x}, not upgrade")
    # the dispatcher's literal must be its own, or the patch would leak
    refs = ldr_lit_refs(img, LIT_CMD)
    if refs != [SITE_CMD_LDR]:
        raise SystemExit(f"literal {LIT_CMD:#x} is loaded from {[hex(r) for r in refs]}, not only {SITE_CMD_LDR:#x}")
    out = [
        site(img, "autoboot", SITE_AUTOBOOT, thumb_bcond(SITE_AUTOBOOT, 0x1, 0x9FF00D24), NOP16),
        site(img, "command", SITE_CMD_LDR, struct.pack("<H", 0x4800 | ((LIT_CMD - ((SITE_CMD_LDR + 4) & ~3)) >> 2)),
             struct.pack("<H", 0x4800 | ((LIT_CMD - ((SITE_CMD_LDR + 4) & ~3)) >> 2))),
        site(img, "  literal", LIT_CMD, struct.pack("<I", STR_BOOT_COMMAND), struct.pack("<I", STR_UPGRADE)),
        site(img, "  getenv", SITE_CMD_GETENV, thumb_bl(SITE_CMD_GETENV, GETENV), NOP32),
        site(img, "partition", NAME_UPDATE, "Update\0".encode("utf-16-le"), "System\0".encode("utf-16-le")),
        site(img, "no-saveenv", SITE_SAVEENV, thumb_bl(SITE_SAVEENV, SAVEENV), NOP32),
        site(img, "no-record", SITE_RECORD, thumb_bl(SITE_RECORD, RECORD), NOP32),
    ]
    return out


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        sys.stderr.write(__doc__)
        return 2
    img = bytearray(Path(argv[0]).read_bytes())
    for line in patch(img):
        print("  " + line)
    Path(argv[1]).write_bytes(img)
    print(f"wrote {argv[1]}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
