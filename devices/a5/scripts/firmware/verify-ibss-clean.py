#!/usr/bin/env python3
"""Verify iBSS.patched is sig-only (no cpu1 park hooks).

Stock iBSS has many BLs into 0x3400xxxx (same image). Those are NOT park
hooks. We only fail on:
  - Core Reset @0x83ce rewritten to bl (early plant)
  - Known park plant sites (late maxlen hook / cave markers)
  - Diff vs iBSS.prepatched when that file exists
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

from capstone import CS_ARCH_ARM, CS_MODE_THUMB, Cs

CORE_OFF = 0x83CE
# Late park hook sites used by retired patch-ibss-cpu1-park.py
PARK_HOOK_OFFS = (0x8EE0, 0x8EE4, 0x8EE8, 0x8EEC)  # near usb_wait maxlen
PLANT_MAGIC = struct.pack("<I", 0xC0DE0101)
TRAMP_PA = struct.pack("<I", 0x800C3800)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("ibss", type=Path, nargs="?", default=Path("ibec/iBSS.patched"))
    ap.add_argument(
        "--prepatched",
        type=Path,
        default=None,
        help="compare to this file (default: sibling iBSS.prepatched)",
    )
    args = ap.parse_args()
    data = args.ibss.read_bytes()
    base = struct.unpack_from("<I", data, 0x20)[0] & ~0xFFFFF
    cs = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
    failed = False

    print(f"{args.ibss} size={len(data)}")

    core = list(cs.disasm(data[CORE_OFF : CORE_OFF + 4], base + CORE_OFF))
    if core:
        print(f"  @{CORE_OFF:#x}: {core[0].mnemonic} {core[0].op_str}")
    if core and core[0].mnemonic == "bl":
        print("FAIL: Core Reset hooked — restore from iBSS.prepatched")
        failed = True

    # Park plant leaves C0DE0101 / 0x800C3800 literals in a code cave
    if PLANT_MAGIC in data and TRAMP_PA in data:
        # Both appear only if we planted; stock sig-patched may lack both
        pi = data.find(PLANT_MAGIC)
        ti = data.find(TRAMP_PA)
        # Ignore if they only sit far apart as unrelated constants
        if abs(pi - ti) < 0x200:
            print(f"FAIL: park plant literals near each other (@{pi:#x}, @{ti:#x})")
            failed = True

    pre = args.prepatched
    if pre is None:
        cand = args.ibss.with_name("iBSS.prepatched")
        if cand.is_file():
            pre = cand
    if pre is not None and pre.is_file():
        ref = pre.read_bytes()
        if ref == data:
            print(f"OK: identical to {pre}")
            return 0 if not failed else 1
        if len(ref) != len(data):
            print(f"FAIL: size differs from {pre} ({len(data)} vs {len(ref)})")
            failed = True
        else:
            diffs = sum(1 for a, b in zip(data, ref) if a != b)
            print(f"FAIL: {diffs} bytes differ from {pre} — run restore-ibss-clean.sh")
            failed = True
    elif not failed:
        print("OK: no Core Reset hook (no prepatched to diff)")
        return 0

    if failed:
        print("FAIL: not clean — bash scripts/restore-ibss-clean.sh")
        return 1
    print("OK: sig-only iBSS (no park hooks)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
