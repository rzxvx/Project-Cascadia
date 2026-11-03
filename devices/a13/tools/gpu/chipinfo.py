#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Builds m1n1's CHIP_INFO entry for T8030 (A13, G12) from what iOS 16.4 gave
its GPU firmware (dumps from kmem, parsed by agxparse.py).

    chipinfo.py DUMPDIR HWDATA_A HWDATA_B REGION_C

Each field is read back from where m1n1's agx/initdata.py writes it, so the
entry reproduces iOS's values.  Fields the V13_3 structures do not carry (m1n1
uses them only for older firmware) are filled from what is known and marked.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from agxparse import GpuMem, mismatches, parse  # noqa: E402

if len(sys.argv) != 5:
    sys.exit(__doc__)
mem = GpuMem(sys.argv[1])
mem.recurse = False
a = parse(mem, "AGXHWDataA", int(sys.argv[2], 0))
b = parse(mem, "AGXHWDataB", int(sys.argv[3], 0))
c = parse(mem, "InitData_RegionC", int(sys.argv[4], 0))


missing = []


def g(obj, name, default=0):
    """A field that V13_3/G13 may not have: default, and say so."""
    if name in obj:
        return obj[name]
    missing.append(name)
    return default


def rows(x):
    return [list(r) for r in x]


sram = [v for row in b.voltages_sram for v in row if v]
info = [
    ("chip_id", hex(b.chip_id), "HwDataB.chip_id"),
    ("min_sram_volt", min(sram), "lowest HwDataB.voltages_sram (field is pre-13.0 only)"),
    ("max_power", int(a.max_power_1), "HwDataA.max_power_1"),
    ("max_freq_mhz", a.max_freq_mhz, "HwDataA.max_freq_mhz"),
    ("unk_87c", a.unk_87c, "HwDataA.unk_87c"),
    ("unk_8cc", a.unk_8cc, "HwDataA.unk_8cc"),
    ("unk_924", rows(a.unk_924), "HwDataA.unk_924"),
    ("unk_e48", rows(a.unk_e48), "HwDataA.unk_e48"),
    ("unk_e24", a.unk_e24, "HwDataA.unk_e24"),
    ("gpu_fast_die0_sensor_mask64", hex(a.fast_die0_sensor_mask64), "HwDataA"),
    ("gpu_fast_die1_sensor_mask64", hex(g(a, "fast_die1_sensor_mask64")), "HwDataA"),
    ("gpu_fast_die0_sensor_mask64_alt", hex(g(a, "fast_die0_sensor_mask64_alt")), "HwDataA"),
    ("gpu_fast_die1_sensor_mask64_alt", hex(g(a, "fast_die1_sensor_mask64_alt")), "HwDataA"),
    ("gpu_fast_die0_sensor_present", hex(a.get("fast_die0_sensor_present") or 1),
     "pre-13.0 only; 1 = die 0 has sensors"),
    ("shared1_tab", [hex(v) if v >= 0 else v for v in a.hws1.table], "HwDataA.hws1.table"),
    ("shared1_a4", hex(a.hws1.unk_a4), "HwDataA.hws1.unk_a4"),
    ("shared2_tab", [hex(v) if v >= 0 else v for v in a.hws2.table], "HwDataA.hws2.table"),
    ("shared2_unk_508", hex(a.hws2.unk_508), "HwDataA.hws2.unk_508"),
    ("unk_3cf4", list(a.unk_3cf4), "HwDataA.unk_3cf4"),
    ("unk_3d14", list(a.unk_3d14), "HwDataA.unk_3d14"),
    ("unk_3d34_0", list(a.unk_3d34_0), "HwDataA.unk_3d34_0"),
    ("unk_118ec", list(c.unk_118ec) if c.unk_118e8 else None, "RegionC.unk_118ec if unk_118e8"),
    ("hwdb_4e0", hex(b.unk_4e0), "HwDataB.unk_4e0"),
    ("hwdb_534", hex(b.unk_534), "HwDataB.unk_534"),
    ("num_cores", b.num_cores, "HwDataB.num_cores"),
    ("gpu_core", b.gpu_core, "HwDataB.gpu_core"),
    ("gpu_rev", b.gpu_rev, "HwDataB.gpu_rev"),
    ("hwdb_ab8", hex(b.get("unk_ab8") or 0), "pre-13.0 only"),
    ("hwdb_abc", hex(b.get("unk_abc") or 0), "pre-13.0 only"),
    ("hwdb_b30", hex(b.unk_b30), "HwDataB.unk_b30"),
    ("rel_max_powers", [v for v in b.rel_max_powers][:b.max_pstate + 1], "HwDataB.rel_max_powers"),
    ("shared2_t1_coef", None, "no T8112-style curves (HwDataA.hws2.unk_38 left as m1n1 builds it)"),
    ("shared3_tab", [hex(v) for v in a.hws3.table], "HwDataA.hws3.table"),
    ("shared3_unk", hex(a.hws3.unk_8), "HwDataA.hws3.unk_8"),
    ("sram_base", hex(b.sgx_sram_ptr), "HwDataB.sgx_sram_ptr (0: none mapped)"),
    ("sram_size", 0, ""),
    ("rc_unk_54", hex(c.unk_54), "RegionC.unk_54"),
    ("unk_hws2_0", c.unk_hws2_0, "RegionC.unk_hws2_0"),
    ("unk_hws2_4", list(c.unk_hws2_4), "RegionC.unk_hws2_4"),
    ("unk_hws2_24", c.unk_hws2_24, "RegionC.unk_hws2_24"),
]

print("    0x8030: Container(")
for name, val, src in info:
    v = str(val).replace("'", "")
    print(f"        {name} = {v},  # {src}")
print("    ),")
for name in missing:
    print(f"# not in the V13_3 structures, left 0: {name}")
for path, pos, want, got in mismatches:
    print(f"# CONST MISMATCH {path} @ {pos:#x}: m1n1 {want!r}, here {got!r}")
