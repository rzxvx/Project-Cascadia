#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Dump the register blocks of pmgr-regions.txt through m1n1's proxy, in
`kmem mmio`'s format (PA = value, nonzero 32-bit words), for diffing with
the same dump from the live iOS (a13/docs/research/gpu.md, G4).

    M1N1DEVICE=/dev/cu.usbmodemXXXX pmgr-dump.py > proxy.txt
    ssh iphone11-ios 'for each region: kmem mmio PA LEN' > ios.txt
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.environ.get("M1N1_PROXYCLIENT", os.path.join(HERE, "../../build/m1n1/proxyclient")))

from m1n1.setup import *  # noqa: E402,F401,F403  (p, u)

for line in open(os.path.join(HERE, "pmgr-regions.txt")):
    line = line.split("#")[0].split()
    if not line:
        continue
    pa, ln = int(line[0], 0), int(line[1], 0)
    for o in range(0, ln, 4):
        v = p.read32(pa + o)
        if v:
            print(f"0x{pa + o:x} = 0x{v:08x}")
