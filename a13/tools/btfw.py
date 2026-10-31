#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
btfw.py - name iOS's Broadcom Bluetooth firmware the way Linux's hci_bcm4377
asks for it: brcm/brcmbt<chip><stepping>-apple,<board>-<vendor>.{bin,ptb}.

  btfw.py SRC OUTDIR [ANTENNA]     SRC = usr/share/firmware/bluetooth

Follows asahi-installer's asahi_firmware/bluetooth.py (The Asahi Linux
contributors, MIT), for the iOS names:
  BCM4378B1_20.5.512.3427_PCIE_Moana_OS_MUR_20230130.bin   firmware
  BCM4378B1_CRBv3_Moana_OS_MUR_K_R_20190725.ptb             its table, per antenna
Vendors: MUR -> m, USI -> u, AMKOR -> a (the module's OTP picks one at run
time, so every vendor's files are written). The low-power ("LPM") builds are
left out. ANTENNA picks the .ptb (the syscfg's WSKU: R, US; default R).
"""
import os
import re
import sys

VENDORS = {"MUR": "m", "USI": "u", "AMKOR": "a"}


def main():
    if len(sys.argv) not in (3, 4):
        raise SystemExit(__doc__)
    src, out = sys.argv[1], sys.argv[2]
    antenna = sys.argv[3] if len(sys.argv) == 4 else "R"
    picked = {}
    for name in sorted(os.listdir(src)):
        root, ext = os.path.splitext(name)
        if ext not in (".bin", ".ptb") or "_LPM_" in root:
            continue
        parts = root.split("_")
        m = re.fullmatch(r"BCM(43[0-9]{2})([A-Z][0-9])", parts[0])
        if not m or "OS" not in parts:
            continue
        board = parts[parts.index("OS") - 1].lower()
        vendor = next((VENDORS[p] for p in parts if p in VENDORS), None)
        if vendor is None:
            continue
        if ext == ".ptb" and antenna not in parts:
            continue
        key = (m.group(1), m.group(2).lower(), board, vendor, ext)
        if key in picked:
            continue
        picked[key] = os.path.join(src, name)
    if not picked:
        raise SystemExit("no Bluetooth firmware found")
    os.makedirs(os.path.join(out, "brcm"), exist_ok=True)
    for (chip, step, board, vendor, ext), path in sorted(picked.items()):
        dst = f"brcmbt{chip}{step}-apple,{board}-{vendor}{ext}"
        with open(path, "rb") as f, open(os.path.join(out, "brcm", dst), "wb") as g:
            g.write(f.read())
        print(f"{dst:44} <- {os.path.basename(path)}")


if __name__ == "__main__":
    main()
