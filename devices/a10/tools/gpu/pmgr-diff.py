#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Diff PMGR's state under m1n1 against a dump taken under Linux
(devices/a10/docs/gpu.md, "G1 answered from Linux"): the core's registers
answer under Linux and hang under m1n1, so the difference is in what the
Linux kernel sets up.

    M1N1DEVICE=/dev/cu.usbmodemXXXX1 build/proxyvenv/bin/python tools/gpu/pmgr-diff.py [build/gpu/pmgr-linux.txt]

Turns PMGR GFX on (as the Linux dump had it), then reads every address listed in the Linux dump (lines
"0xADDR: 0xVALUE" from tools/gpu/mmio) and prints those that differ, with
the PMGR device name for power-state registers.
"""
import os
import re
import sys

PROXY = os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../build/m1n1/proxyclient")
sys.path.insert(0, os.environ.get("M1N1_PROXYCLIENT", PROXY))

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gpusetup import p, u, iface, GUARD  # noqa: E402  (m1n1 proxy, or GPU_BACKEND=linux)

dump = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(PROXY), "../gpu/pmgr-linux.txt")
adt = u.adt
pm = adt["/arm-io/pmgr"]
names = {}
for d in pm.devices:
    try:
        names[adt.pmgr_dev_get_addr(d)] = d.name
    except Exception:
        pass

# the Linux dump was taken with ps_gfx on: the same here
p.pmgr_adt_power_enable("/arm-io/sgx")
p.pmgr_adt_power_enable("/arm-io/gfx-kf")

linux = {}
for line in open(dump):
    m = re.match(r"(0x[0-9a-f]+): (0x[0-9a-f]+|0+)$", line.strip())
    if m:
        linux[int(m.group(1), 16)] = int(m.group(2), 16)

n = 0
for addr in sorted(linux):
    v = p.read32(addr)
    if v != linux[addr]:
        n += 1
        print(f"{addr:#x} {names.get(addr, ''):16} m1n1 {v:#010x}  linux {linux[addr]:#010x}", flush=True)
print(f"{n} of {len(linux)} words differ")
