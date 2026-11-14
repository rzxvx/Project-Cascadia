#!/usr/bin/env python3
"""Patch iBEC to boot Linux from the NAND by itself: the PC-less route.

    patch-ibec-upgrade.py [--stay] [--stop STAGE] IBEC_PATCHED OUT

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

--stay is for bring-up: a failed boot returns to iBEC's dispatcher instead of
rebooting, so the iPad stays in Recovery where a computer can see it -- the
proof that kloader started iBEC and `upgrade` ran.  Not for the real image: a
failed boot has to end in iOS.

--stop STAGE is for bring-up too: `upgrade` returns to the dispatcher right
after STAGE has succeeded (mount, devicetree, ramdisk, kernelcache), so without
--stay the outcomes differ -- Recovery: everything up to STAGE worked; a reboot
to iOS: a failure exit; neither: a hang before STAGE's end.

Every site is checked against the expected original instruction (or the
patched one, so running it twice is harmless); anything else stops.
Offsets: iBEC iBoot-2261.30.37, iOS 8.4.1 12H321, base 0x9ff00000, for the
P105 and the K93: one iBoot build, so the code sites are the same addresses
on both; the strings, the boot-command table, the "Update" name and three
call targets (getenv, saveenv, reboot) are each board's own (BOARDS).
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

BASE = 0x9FF00000
NOP16 = b"\x00\xbf"
NOP32 = NOP16 * 2

SITE_AUTOBOOT = 0x9FF00CB2        # bne 0x9ff00d24 (auto-boot != true)
SITE_CMD_LDR = 0x9FF00D00         # ldr r0, [pc, #0x12c] -> "boot-command"
LIT_CMD = 0x9FF00E30
SITE_CMD_GETENV = 0x9FF00D02      # bl getenv
SITE_SAVEENV = 0x9FF01486         # bl saveenv
SITE_RECORD = 0x9FF01504          # bl record_upgrade_failure (0x9ff01438)
RECORD = 0x9FF01438
SITE_REBOOT = 0x9FF01510          # bl reboot-ish, end of upgrade's failure path
POP_R3_R4_R7_PC = b"\x98\xbd"     # undoes the handler's push {r3, r4, r7, lr}
UPGRADE_HANDLER = 0x9FF0147C
# (first instruction reached only after that stage succeeded, 16-bit original)
STOPS = {
    "mount": (0x9FF01492, 0x4825),        # ldr r0, =/boot/apticket.der
    "devicetree": (0x9FF014B2, 0x4822),   # ldr r0, =/boot/ramdisk
    "ramdisk": (0x9FF014BC, 0x4822),      # ldr r0, =/boot/kernelcache
    "kernelcache": (0x9FF014CA, 0x4925),  # ldr r1, =boot args, before the jump
}

# Each board's own: the strings, the boot-command table ({"upgrade", handler},
# ...), u"Update\0" (compared over 14 bytes) and the three call targets.
BOARDS = {
    "p105": dict(STR_UPGRADE=0x9FF347E7, STR_BOOT_COMMAND=0x9FF34828,
                 NAME_UPDATE=0x9FF439E4, BOOT_TABLE=0x9FF44040,
                 GETENV=0x9FF17A18, SAVEENV=0x9FF1C358, REBOOT=0x9FF1F224),
    "k93": dict(STR_UPGRADE=0x9FF36A67, STR_BOOT_COMMAND=0x9FF36AA8,
                NAME_UPDATE=0x9FF45814, BOOT_TABLE=0x9FF46040,
                GETENV=0x9FF178DC, SAVEENV=0x9FF1C21C, REBOOT=0x9FF1F160),
}


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


def board_of(img: bytes) -> str:
    for name in BOARDS:
        tag = f"iBEC for {name}".encode()
        if img[0x200:0x200 + len(tag) + 1] in (tag + b",", tag + b"\0", tag + b" "):
            if img[0x280:0x290].startswith(b"iBoot-2261.30.37"):
                return name
    raise SystemExit("not an iBEC this patcher knows (iBEC for p105/k93, iBoot-2261.30.37)")


def patch(img: bytearray, stay: bool = False, stop: str | None = None) -> list[str]:
    b = BOARDS[board_of(img)]
    STR_UPGRADE, STR_BOOT_COMMAND = b["STR_UPGRADE"], b["STR_BOOT_COMMAND"]
    NAME_UPDATE, BOOT_TABLE = b["NAME_UPDATE"], b["BOOT_TABLE"]
    GETENV, SAVEENV, REBOOT = b["GETENV"], b["SAVEENV"], b["REBOOT"]
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
    if stay:
        out.append(site(img, "stay", SITE_REBOOT, thumb_bl(SITE_REBOOT, REBOOT), POP_R3_R4_R7_PC + NOP16))
    if stop:
        va, orig = STOPS[stop]
        out.append(site(img, f"stop:{stop}"[:11], va, struct.pack("<H", orig), POP_R3_R4_R7_PC))
    return out


def main(argv: list[str]) -> int:
    stay = "--stay" in argv
    argv = [a for a in argv if a != "--stay"]
    stop = None
    if "--stop" in argv:
        i = argv.index("--stop")
        stop = argv[i + 1]
        if stop not in STOPS:
            raise SystemExit(f"--stop: one of {', '.join(STOPS)}")
        del argv[i:i + 2]
    if len(argv) != 2:
        sys.stderr.write(__doc__)
        return 2
    img = bytearray(Path(argv[0]).read_bytes())
    for line in patch(img, stay, stop):
        print("  " + line)
    Path(argv[1]).write_bytes(img)
    print(f"wrote {argv[1]}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
