#!/usr/bin/env python3
"""
Phase 4a: graft 32-bit Apple S5L support onto a pristine kernel tree.

Two kinds of change:

  1. Whole new files, copied verbatim from patches/files/ into the tree.
  2. One-line insertions into existing Kconfig/Makefile glue.  These are
     described declaratively below and applied only if not already present, so
     running this twice is a no-op and running it after a "make mrproper" still
     works.

Afterwards, unified diffs of the touched glue files are written to patches/ so
the change set is reviewable without a git checkout.

Usage:
    ./apply-kernel-patches.py --tree build/linux [--revert]
"""

from __future__ import annotations

import argparse
import difflib
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
FILES = os.path.join(ROOT, "patches", "files")
PATCHDIR = os.path.join(ROOT, "patches")

# New files: (path relative to both patches/files/ and the kernel tree)
NEW_FILES = [
    "arch/arm/include/asm/apple_boot.h",
    "arch/arm/mach-apple/Kconfig",
    "arch/arm/mach-apple/Makefile",
    "arch/arm/mach-apple/apple.c",
    "arch/arm/mach-apple/platsmp.c",
    "arch/arm/mach-apple/pmccntr.c",
    "arch/arm/mach-apple/apple_wdt_clkevt.c",
    "arch/arm/mach-apple/apple_pmu_clkevt.c",
    "arch/arm/mach-apple/apple_sof_clkevt.c",
    "arch/arm/mach-apple/p105_fb_dbg.c",
    "arch/arm/mach-apple/p105_fb_dbg.h",
    "arch/arm/mach-apple/font8x8.h",
    "drivers/irqchip/irq-apple-aic1.c",
    "drivers/phy/phy-apple-s5l-usb.c",
    "Documentation/devicetree/bindings/interrupt-controller/apple,aic1.yaml",
]

# Glue edits: (file, anchor substring, text to insert after that line)
#
# Anchors are matched as substrings rather than whole lines so that whitespace
# churn between kernel releases does not break them.  Each insertion keeps the
# surrounding file's alphabetical ordering.
GLUE = [
    (
        "arch/arm/Kconfig",
        'source "arch/arm/mach-alpine/Kconfig"',
        '\nsource "arch/arm/mach-apple/Kconfig"\n',
    ),
    (
        "arch/arm/Makefile",
        "machine-$(CONFIG_ARCH_ALPINE)",
        "machine-$(CONFIG_ARCH_APPLE_S5L)\t+= apple\n",
    ),
    (
        "drivers/irqchip/Makefile",
        "irq-owl-sirq.o",
        "obj-$(CONFIG_APPLE_AIC1)\t\t+= irq-apple-aic1.o\n",
    ),
    (
        "drivers/phy/Makefile",
        "obj-$(CONFIG_GENERIC_PHY)",
        "obj-$(CONFIG_PHY_APPLE_S5L_USB)\t+= phy-apple-s5l-usb.o\n",
    ),
]

# Substring replacements: (file, old, new, description)
#
# Both of these teach the existing Samsung serial driver -- which already
# carries the apple,s5l-uart binding, added for Apple silicon Macs -- that the
# same UART exists on 32-bit S5L.  No driver logic changes.
REPLACEMENTS = [
    (
        "drivers/irqchip/Makefile",
        "obj-$(CONFIG_APPLE_S5L_AIC)\t\t+= irq-apple-s5l-aic.o\n",
        "obj-$(CONFIG_APPLE_AIC1)\t\t+= irq-apple-aic1.o\n",
        "Makefile: upgrade APPLE_S5L_AIC object to irq-apple-aic1",
    ),
    (
        "drivers/tty/serial/Kconfig",
        "\tdepends on PLAT_SAMSUNG || ARCH_S5PV210 || ARCH_EXYNOS || ARCH_APPLE"
        " || ARCH_ARTPEC || COMPILE_TEST",
        "\tdepends on PLAT_SAMSUNG || ARCH_S5PV210 || ARCH_EXYNOS || ARCH_APPLE"
        " || ARCH_APPLE_S5L || ARCH_ARTPEC || COMPILE_TEST",
        "allow SERIAL_SAMSUNG on 32-bit Apple S5L",
    ),
    (
        "drivers/tty/serial/samsung_tty.c",
        "#ifdef CONFIG_ARCH_APPLE\n",
        "#if defined(CONFIG_ARCH_APPLE) || defined(CONFIG_ARCH_APPLE_S5L)\n",
        "build the s5l driver data on 32-bit Apple S5L",
    ),
]

# Appended verbatim to drivers/irqchip/Kconfig if APPLE_AIC1 is not defined.
IRQCHIP_KCONFIG = """
config APPLE_AIC1
\tbool "Apple aic,1 interrupt controller (32-bit S5L)"
\tdepends on ARM && (ARCH_APPLE_S5L || COMPILE_TEST)
\tdefault ARCH_APPLE_S5L
\tselect IRQ_DOMAIN
\tselect GENERIC_IRQ_CHIP
\thelp
\t  Support for the Apple Interrupt Controller revision 1 ("aic,1") found
\t  on 32-bit S5L SoCs (Apple A4 through A6X).  This is a separate driver
\t  from APPLE_AIC (arm64 Apple Silicon): different DT binding, EVENT format,
\t  and no integrated FIQ/IPI support.  Required to boot Linux on A5 devices.
"""

# Appended verbatim to drivers/phy/Kconfig if PHY_APPLE_S5L_USB is not defined.
#
# Appended after the file's endmenu rather than inside it.  Kconfig accepts a
# symbol defined outside a menu -- it just does not show up under "PHY
# Subsystem" in menuconfig -- and anchoring inside the menu would mean guessing
# at a source line that moves between releases.  config/p105ap.config sets the
# symbol directly, so menu placement does not matter.
PHY_KCONFIG = """
config PHY_APPLE_S5L_USB
\tbool "Apple S5L USB OTG PHY (32-bit)"
\tdepends on ARCH_APPLE_S5L || COMPILE_TEST
\tdefault ARCH_APPLE_S5L
\tselect GENERIC_PHY
\thelp
\t  USB 2.0 OTG PHY found on 32-bit Apple S5L SoCs (A4 through A6X).
\t  Required for the dwc2 controller to come out of reset.  On A5 this is
\t  also what the system tick depends on -- see arch/arm/mach-apple.
"""


def fail(msg: str) -> None:
    print(f"error: {msg}", file=sys.stderr)
    raise SystemExit(1)


def read(path: str) -> str:
    with open(path, encoding="utf-8") as fh:
        return fh.read()


def read_bytes(path: str) -> bytes:
    with open(path, "rb") as fh:
        return fh.read()


def write(path: str, text: str) -> None:
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(text)


def emit_patch(name: str, before: str, after: str, path: str) -> None:
    diff = difflib.unified_diff(
        before.splitlines(keepends=True),
        after.splitlines(keepends=True),
        fromfile=f"a/{path}",
        tofile=f"b/{path}",
    )
    body = "".join(diff)
    if body:
        write(os.path.join(PATCHDIR, name), body)


def copy_new_files(tree: str, revert: bool) -> int:
    changed = 0
    for rel in NEW_FILES:
        dst = os.path.join(tree, rel)
        if revert:
            if os.path.exists(dst):
                os.remove(dst)
                changed += 1
                print(f"  removed {rel}")
            continue
        src = os.path.join(FILES, rel)
        if not os.path.exists(src):
            fail(f"missing source file {src}")
        # Compare bytes, not text.  A text-mode compare treats a CRLF copy in
        # the tree as identical to an LF source, and Kconfig rejects CRLF.
        if os.path.exists(dst) and read_bytes(dst) == read_bytes(src):
            continue
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(src, dst)
        changed += 1
        print(f"  installed {rel}")
    if revert:
        d = os.path.join(tree, "arch/arm/mach-apple")
        if os.path.isdir(d) and not os.listdir(d):
            os.rmdir(d)
    return changed


def apply_glue(tree: str, revert: bool) -> int:
    changed = 0
    for rel, anchor, insertion in GLUE:
        path = os.path.join(tree, rel)
        if not os.path.exists(path):
            fail(f"{rel} not found in {tree}; is this a kernel tree?")
        original = read(path)

        if revert:
            if insertion not in original:
                continue
            write(path, original.replace(insertion, "", 1))
            changed += 1
            print(f"  reverted {rel}")
            continue

        if insertion in original:
            continue

        lines = original.splitlines(keepends=True)
        idx = next((i for i, ln in enumerate(lines) if anchor in ln), None)
        if idx is None:
            fail(
                f"anchor not found in {rel}:\n  {anchor!r}\n"
                "The kernel version probably moved it; update GLUE in "
                "scripts/apply-kernel-patches.py."
            )
        lines.insert(idx + 1, insertion)
        new = "".join(lines)
        write(path, new)
        emit_patch(f"{rel.replace('/', '_')}.patch", original, new, rel)
        changed += 1
        print(f"  patched {rel}")
    return changed


def apply_replacements(tree: str, revert: bool) -> int:
    changed = 0
    for rel, old, new, desc in REPLACEMENTS:
        path = os.path.join(tree, rel)
        if not os.path.exists(path):
            fail(f"{rel} not found in {tree}")
        original = read(path)

        want_from, want_to = (new, old) if revert else (old, new)

        if want_to in original:
            continue
        if want_from not in original:
            fail(
                f"cannot {desc}: expected text not found in {rel}.\n"
                f"  looking for: {want_from!r}\n"
                "Update REPLACEMENTS in scripts/apply-kernel-patches.py for "
                "this kernel version."
            )
        updated = original.replace(want_from, want_to, 1)
        write(path, updated)
        if not revert:
            emit_patch(f"{rel.replace('/', '_')}.patch", original, updated, rel)
        changed += 1
        print(f"  {'reverted' if revert else 'patched'} {rel} ({desc})")
    return changed


def apply_irqchip_kconfig(tree: str, revert: bool) -> int:
    path = os.path.join(tree, "drivers/irqchip/Kconfig")
    original = read(path)

    old_block = """
config APPLE_S5L_AIC
\tbool "Apple S5L AIC (32-bit)"
\tdepends on ARM && (ARCH_APPLE_S5L || COMPILE_TEST)
\tdefault ARCH_APPLE_S5L
\tselect IRQ_DOMAIN
\tselect GENERIC_IRQ_CHIP
\thelp
\t  Support for the Apple Interrupt Controller found on 32-bit S5L SoCs
\t  (A4 through A6X).  This is the predecessor of the AIC on Apple silicon
\t  Macs that APPLE_AIC drives, but 32-bit and without IPI or FIQ support.
"""

    if revert:
        if "config APPLE_AIC1" not in original and "config APPLE_S5L_AIC" not in original:
            return 0
        text = original.replace(IRQCHIP_KCONFIG, "")
        text = text.replace(old_block, "")
        write(path, text)
        print("  reverted drivers/irqchip/Kconfig")
        return 1

    if "config APPLE_AIC1" in original:
        if old_block in original:
            write(path, original.replace(old_block, IRQCHIP_KCONFIG))
            print("  upgraded drivers/irqchip/Kconfig (APPLE_S5L_AIC -> APPLE_AIC1)")
            return 1
        return 0

    if "config APPLE_S5L_AIC" in original:
        write(path, original.replace(old_block, IRQCHIP_KCONFIG))
        print("  upgraded drivers/irqchip/Kconfig (APPLE_S5L_AIC -> APPLE_AIC1)")
        return 1

    new = original.rstrip("\n") + "\n" + IRQCHIP_KCONFIG
    write(path, new)
    emit_patch("drivers_irqchip_Kconfig.patch", original, new,
               "drivers/irqchip/Kconfig")
    print("  patched drivers/irqchip/Kconfig")
    return 1


def apply_phy_kconfig(tree: str, revert: bool) -> int:
    path = os.path.join(tree, "drivers/phy/Kconfig")
    original = read(path)
    if revert:
        if PHY_KCONFIG not in original:
            return 0
        write(path, original.replace(PHY_KCONFIG, ""))
        print("  reverted drivers/phy/Kconfig")
        return 1
    if PHY_KCONFIG in original:
        return 0
    if "PHY_APPLE_S5L_USB" in original:
        fail("drivers/phy/Kconfig already defines PHY_APPLE_S5L_USB differently; "
             "remove the stale block by hand")
    write(path, original.rstrip("\n") + "\n" + PHY_KCONFIG)
    print("  appended PHY_APPLE_S5L_USB to drivers/phy/Kconfig")
    return 1


def main() -> int:
    ap = argparse.ArgumentParser(description="Install Apple S5L kernel support")
    ap.add_argument("--tree", default=os.path.join(ROOT, "build", "linux"))
    ap.add_argument("--revert", action="store_true")
    # The glue edits moved to patches/tree/0001-cascadia.patch, applied by
    # scripts/apply-kernel-edits.sh, because anchor matching skips a stale
    # anchor silently and the first one it skipped was the apple_aic1_rearm()
    # call.  Copying whole new files has no such failure mode, so that stays
    # here.  --files-only is what build-kernel.sh uses.
    ap.add_argument("--files-only", action="store_true",
                    help="copy patches/files/ only; skip the legacy anchor edits")
    args = ap.parse_args()

    tree = os.path.abspath(args.tree)
    if not os.path.exists(os.path.join(tree, "Makefile")):
        fail(f"{tree} does not look like a kernel tree")

    verb = "Reverting" if args.revert else "Applying"
    print(f"{verb} Apple S5L support in {tree}")

    changed = 0
    changed += copy_new_files(tree, args.revert)
    if not args.files_only:
        changed += apply_glue(tree, args.revert)
        changed += apply_replacements(tree, args.revert)
        changed += apply_irqchip_kconfig(tree, args.revert)
        changed += apply_phy_kconfig(tree, args.revert)

    print(f"{changed} change(s)" if changed else "already up to date")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
