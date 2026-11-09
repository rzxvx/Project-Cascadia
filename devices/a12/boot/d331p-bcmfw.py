#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0+ OR MIT
# Copyright (c) 2026 kiki <kieran@lavato.net>
"""Rename the BCM4377 wifi and Bluetooth firmware from an iOS root filesystem
to the names brcmfmac and hci_bcm4377 ask for.

usage: bcmfw.py <ios root> <output dir>

The wifi files are the genie island's for chip revision B2, which is what
/arm-io/wlan module-instance and the PCI revision say this phone has. Each
file keeps its region, module, vendor and version in its new name, so the
driver picks the right one from the OTP and the antenna SKU m1n1 passes on.
"""

import pathlib
import re
import shutil
import sys

ISLAND = "geniek"
WLAN = "System/Library/DriverExtensions/com.apple.DriverKit-AppleBCMWLAN.dext/Firmware/C-4377__s-B2"
BT = "usr/share/firmware/bluetooth"

if len(sys.argv) != 3:
    sys.exit("usage: bcmfw.py <ios root> <output dir>")

root = pathlib.Path(sys.argv[1])
out = pathlib.Path(sys.argv[2]) / "lib/firmware/brcm"
out.mkdir(parents=True, exist_ok=True)
wifi = "brcmfmac4377b3-pcie.apple," + ISLAND
bt = "brcmbt4377b2-apple," + ISLAND
ext = {"trx": "bin", "clmb": "clm_blob", "txcb": "txcap_blob"}

for f in (root / WLAN).iterdir():
    # geniek-NA.clmb -> brcmfmac4377b3-pcie.apple,geniek-NA.clm_blob
    m = re.fullmatch(ISLAND + r"(-\w+)?\.(trx|clmb|txcb)", f.name)
    if m:
        shutil.copy(f, out / f"{wifi}{m[1] or ''}.{ext[m[2]]}")
        continue
    # P-geniek-NA_M-DTCK_V-m__m-5.3.txt -> ...apple,geniek-DTCK-m-5.3-NA.txt
    m = re.fullmatch(r"P-" + ISLAND + r"(-\w+)?_M-(\w+)_V-(\w)__m-([\d.]+)\.txt", f.name)
    if m:
        shutil.copy(f, out / f"{wifi}-{m[2]}-{m[3]}-{m[4]}{m[1] or ''}.txt")

for f in (root / BT).iterdir():
    # BCM4377B2_..._PCIE_Genie_OS_MUR_....bin -> brcmbt4377b2-apple,geniek-m.bin
    m = re.fullmatch(r"BCM4377B2_.*PCIE_Genie_OS_(MUR|USI)_\d+\.bin", f.name)
    if m:
        shutil.copy(f, out / f"{bt}-{m[1][0].lower()}.bin")
        continue
    # The patch tables come per region too. The driver asks for one per vendor.
    m = re.fullmatch(r"BCM4377B2_CRBv1_Genie_OS_(MUR|USI)_K_NA_\d+\.ptb", f.name)
    if m:
        shutil.copy(f, out / f"{bt}-{m[1][0].lower()}.ptb")
