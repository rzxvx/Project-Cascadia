#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Print the PMP firmware's device table (a13/docs/research/gpu.md, G4).

    pmp-devtable.py a13/build/gpu/pmp/t8030pmp.bin

The table is not in the image: the firmware's static initialiser (20E247:
fn 0x102630c) builds it in __DATA. This runs that function in unicorn (venv
a13/build/venv-gpu) and reads the config struct it fills (VA 0x1044de8:
+0x16c count, +0x170 table of 0x88-byte entries, name pointer at +0, device
id u16 at +0xe, u16 at +0x28 the doorbell of the CPU clusters and the GPU).
The device ids are what PM commands (e.g. 0xc, register a doorbell) take as
arg1.
"""
import struct
import sys

from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn.arm64_const import UC_ARM64_REG_SP, UC_ARM64_REG_LR, UC_ARM64_REG_CPACR_EL1

TEXT, DATA = 0x1000000, 0x102a000     # file offsets 0x1000 / 0x2b000
INIT, CONFIG = 0x102630c, 0x1044de8
RET = 0x8000000

img = open(sys.argv[1], "rb").read()
mu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
mu.mem_map(TEXT, 0x50000)
mu.mem_write(TEXT, img[0x1000:0x2b000])
mu.mem_write(DATA, img[0x2b000:0x2b000 + 0x1d000])
mu.mem_map(RET, 0x10000)
mu.reg_write(UC_ARM64_REG_SP, RET + 0xf000)
mu.reg_write(UC_ARM64_REG_LR, RET)
mu.reg_write(UC_ARM64_REG_CPACR_EL1, 3 << 20)   # FP/SIMD on
mu.emu_start(INIT, RET, count=200000)


def rd(va, n):
    return bytes(mu.mem_read(va, n))


def q(va):
    return struct.unpack("<Q", rd(va, 8))[0]


count = struct.unpack("<I", rd(CONFIG + 0x16c, 4))[0]
table = q(CONFIG + 0x170)
print(f"{count} devices at {table:#x}")
for i in range(count):
    e = rd(table + i * 0x88, 0x88)
    name = rd(struct.unpack_from("<Q", e)[0], 32).split(b"\0")[0].decode()
    dev_id = struct.unpack_from("<H", e, 0xe)[0]
    db = struct.unpack_from("<H", e, 0x28)[0]
    print(f"{i:2} {name:8} id {dev_id:#04x} doorbell {db:#x}  {e[8:0x30].hex()}")
