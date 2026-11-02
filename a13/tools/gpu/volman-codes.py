#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Read-only: the voltage codes XNU would hand VOLMAN on our boot
(a13/docs/research/gpu.md, G4), next to what the channels and the PMU hold.

    M1N1DEVICE=/dev/cu.usbmodemXXXX volman-codes.py

XNU (AppleT8030PMGR 20E247 fn 0xfffffff009463e6c) clears bit 0 (manual) of
VOLMAN's controls +0x4..+0x1c and then writes each channel the code of the
domain's current perf state; this script computes those codes the same way
and writes nothing. Sources (channel offsets are XNU's w23 = VOLMAN + 4 plus its constants),
PMGR = 0x23b000000:
  +0x34  cluster 0: s = [0x210e20020] & 0xf, code = [0x210e70000 + 0x20 s] >> 56
  +0x30  cluster 1: the same at 0x211e20020 / 0x211e70000
         (vtable +0xa60, fn 0xfffffff009466b4c: cluster-relative addresses)
  +0x28  v = [PMGR+0x58000] & 0xf, code = ([PMGR+0x58018 + 20 v] >> 8) & 0xff
  +0x40  v = [PMGR+0x60000] & 7,   code = [PMGR+0x60008 + 8 v] >> 24
  +0x3c  v = [PMGR+0x64000] & 7,   code = [PMGR+0x64008 + 8 v] >> 24  (GPU perf)
  +0x2c  v = [PMGR+0x5c000] & 7,   code = [PMGR+0x5c008 + 8 v] >> 24
  +0x38  not written by XNU: the GPU's power request drives it (buck 0x4100)
PMU (spmi0 slave 0xf) regulator blocks: +0 on, +1 voltage code; iOS pairs
+0x30/0x4000, +0x34/0x4b00, +0x3c/0x4500, +0x28/0x4200, +0x38/0x4100.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "../../build/m1n1/proxyclient"))
from m1n1.setup import *  # noqa: E402,F401,F403  (p, u)
from m1n1.hw.spmi import SPMI  # noqa: E402

PMGR = 0x23b000000
VOLMAN = PMGR + 0xa0000


def cluster(base):
    s = p.read64(base + 0xe20020) & 0xf
    return s, p.read64(base + 0xe70000 + 0x20 * s) >> 56


def table(state_reg, entries, mask):
    v = p.read32(PMGR + state_reg) & mask
    return v, p.read32(PMGR + entries + 8 * v) >> 24


codes = {}
s, codes[0x34] = cluster(0x210000000)
print(f"+0x34 cluster 0 state {s}: code {codes[0x34]:#x}")
s, codes[0x30] = cluster(0x211000000)
print(f"+0x30 cluster 1 state {s}: code {codes[0x30]:#x}")
v = p.read32(PMGR + 0x58000) & 0xf
codes[0x28] = (p.read32(PMGR + 0x58018 + 20 * v) >> 8) & 0xff
print(f"+0x28 state {v}: code {codes[0x28]:#x}")
for ch, (st, ent) in ((0x40, (0x60000, 0x60008)), (0x3c, (0x64000, 0x64008)),
                      (0x2c, (0x5c000, 0x5c008))):
    v, codes[ch] = table(st, ent, 7)
    print(f"+{ch:#x} state {v}: code {codes[ch]:#x}")

print("VOLMAN now: " + " ".join(f"+{o:#x}={p.read32(VOLMAN + o):#x}"
                                for o in range(0, 0x48, 4)))
spmi = SPMI(u, "/arm-io/spmi0")
pmu = {}
for b in (0x4000, 0x4100, 0x4200, 0x4300, 0x4400, 0x4500, 0x4b00):
    d = spmi.read_extl(0xf, b, 8)
    pmu[b] = d
    print(f"PMU {b:#x}: on {d[0]:#x} code {d[1]:#x}")
for ch in sorted(codes):
    print(f"+{ch:#x}: XNU would write {codes[ch]:#x}, the channel holds "
          f"{p.read32(VOLMAN + ch) & 0xff:#x}")
