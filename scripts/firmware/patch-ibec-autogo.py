#!/usr/bin/env python3
"""
Patch iBEC to jump after a Recovery bulk file is *fully* received.

The ep4 callback at 0x9ff237f4 runs on every 0x8000 USB chunk, not only EOF.
Jumping there immediately (memprobe 64KB) worked because the payload fit in
the first chunk. staging-bundle is 11MB: that same jump ran the loader after
32KB, before zImage landed at 0x80008000 — magenta screen, kernel never ran.

Only jump when this completion's byte count [r4,#0x10] < 0x8000 (short packet
or ZLP). Mid-chunks fall through to the stock USB path.

Payload entry uses blx (not bx): if the payload returns, USB stays alive and
we resume the stock Recovery path. Non-returning payloads (Linux, WFI labs)
behave as before. aic1-lab returns after publishing LAB: on the USB serial.

Offsets: iBEC iBoot-2261 P105AP iOS 8.4.1 (image base 0x9ff00000).
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from capstone import CS_ARCH_ARM, CS_MODE_THUMB, Cs

IBOOT_BASE = 0x9FF00000
HOOK_OFF = 0x2384C
HOOK_LEN = 4
HOOK_ORIG = bytes.fromhex("1a480121")  # ldr r0,[pc,#0x68]; movs r1,#1
TRAMPOLINE_OFF = 0x43E0E
TRAMPOLINE_MAX = 128
CACHE_FLUSH = 0x9FF2134C
DONE_FLAG = 0x9FF48BB0
USB_CONTINUE = 0x9FF201A4
PKT = 0x8000


def iboot_base(data: bytes) -> int:
    import struct

    return struct.unpack_from("<I", data, 0x20)[0] & ~0xFFFFF


def _link_bytes(ld: str, asm: str) -> bytes:
    with tempfile.TemporaryDirectory() as td:
        root = Path(td)
        (root / "patch.S").write_text(asm, encoding="ascii")
        (root / "patch.ld").write_text(ld, encoding="ascii")
        elf = root / "patch.elf"
        subprocess.run(
            [
                "arm-linux-gnueabihf-gcc",
                "-nostdlib",
                "-mthumb",
                "-T",
                str(root / "patch.ld"),
                "-o",
                str(elf),
                str(root / "patch.S"),
            ],
            check=True,
            capture_output=True,
        )
        return subprocess.run(
            [
                "arm-linux-gnueabihf-objcopy",
                "-O",
                "binary",
                "-j",
                ".text",
                str(elf),
                "/dev/stdout",
            ],
            check=True,
            capture_output=True,
        ).stdout


def build_hook(base: int) -> bytes:
    hook_va = base + HOOK_OFF
    tramp_va = base + TRAMPOLINE_OFF
    ld = f"SECTIONS {{ . = {hook_va:#x}; .text : {{ *(.text*) }} }}\n"
    asm = f"""\
.syntax unified
.thumb
.global _hook
_hook:
    b.w {tramp_va:#x}
"""
    raw = _link_bytes(ld, asm)
    if len(raw) != 4:
        raise SystemExit(f"hook size {len(raw)}, expected 4")
    return raw


def build_trampoline(base: int) -> bytes:
    tramp_va = base + TRAMPOLINE_OFF
    ld = f"SECTIONS {{ . = {tramp_va:#x}; .text : {{ *(.text*) }} }}\n"
    asm = f"""\
.syntax unified
.thumb
.global autogo_eof
autogo_eof:
    ldr r1, [r4, #0x10]
    cmp r1, #{PKT}
    bhs not_eof

    ldr r6, [r5, #4]
    bl {CACHE_FLUSH:#x}
    mov r1, r6
    blx r1

not_eof:
    movw r0, #{DONE_FLAG & 0xffff}
    movt r0, #{DONE_FLAG >> 16}
    movs r1, #1
    strb r1, [r0]
    add.w r0, r5, #8
    pop.w {{r4, r5, r7, lr}}
    b.w {USB_CONTINUE:#x}
"""
    raw = _link_bytes(ld, asm)
    if len(raw) > TRAMPOLINE_MAX:
        raise SystemExit(f"trampoline size {len(raw)} > {TRAMPOLINE_MAX}")
    return raw


def verify_patch(data: bytes, base: int) -> None:
    cs = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
    insns = list(cs.disasm(data[HOOK_OFF : HOOK_OFF + 4], base + HOOK_OFF))
    if len(insns) != 1 or insns[0].mnemonic != "b.w":
        raise SystemExit(f"hook not b.w: {insns!r}")
    m = re.search(r"#(0x[0-9a-f]+)", insns[0].op_str)
    if not m or int(m.group(1), 16) != base + TRAMPOLINE_OFF:
        raise SystemExit(f"hook target wrong: {insns[0].op_str}")

    tramp = list(
        cs.disasm(
            data[TRAMPOLINE_OFF : TRAMPOLINE_OFF + TRAMPOLINE_MAX],
            base + TRAMPOLINE_OFF,
        )
    )
    print("  eof trampoline:")
    for insn in tramp[:18]:
        print(f"    {insn.address:#010x}  {insn.mnemonic} {insn.op_str}")
    bls = []
    bxs = []
    for insn in tramp:
        if insn.mnemonic == "bl":
            m2 = re.search(r"#(0x[0-9a-f]+)", insn.op_str)
            if m2:
                bls.append(int(m2.group(1), 16))
        if insn.mnemonic == "bx":
            bxs.append(insn.op_str)
    if CACHE_FLUSH not in bls:
        raise SystemExit(f"trampoline missing cache flush: {bls!r}")
    blxs = [insn.op_str for insn in tramp if insn.mnemonic == "blx"]
    if not any("r1" in x for x in blxs) and not any("r1" in x for x in bxs):
        raise SystemExit(f"trampoline missing blx/bx r1: blx={blxs!r} bx={bxs!r}")


def patch_autogo(data: bytearray, base: int = IBOOT_BASE) -> dict:
    tramp = build_trampoline(base)
    hook = build_hook(base)

    cur = bytes(data[HOOK_OFF : HOOK_OFF + HOOK_LEN])
    if cur != HOOK_ORIG and cur != hook:
        raise SystemExit(
            f"unexpected bytes @ {HOOK_OFF:#x}: {cur.hex()} "
            f"(expected {HOOK_ORIG.hex()})"
        )

    # Allow refresh of an already-patched cave (bx → blx call-return).
    cave = data[TRAMPOLINE_OFF : TRAMPOLINE_OFF + len(tramp)]
    if any(b not in (0, 0xFF) for b in cave) and cur == HOOK_ORIG:
        raise SystemExit(f"cave @ {TRAMPOLINE_OFF:#x} is not empty")

    data[TRAMPOLINE_OFF : TRAMPOLINE_OFF + len(tramp)] = tramp
    data[HOOK_OFF : HOOK_OFF + HOOK_LEN] = hook
    verify_patch(data, base)
    return {"hook": HOOK_OFF, "trampoline": TRAMPOLINE_OFF, "tramp_size": len(tramp)}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("input", type=Path)
    ap.add_argument("output", type=Path)
    args = ap.parse_args()

    data = bytearray(args.input.read_bytes())
    base = iboot_base(data)
    info = patch_autogo(data, base)
    args.output.write_bytes(data)

    print(f"wrote {args.output}")
    print(f"  hook: {info['hook']:#x} -> tramp {info['trampoline']:#x} ({info['tramp_size']} bytes)")
    print("  EOF: blx payload; on return → USB_CONTINUE (call-return)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
