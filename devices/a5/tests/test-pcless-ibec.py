#!/usr/bin/env python3
"""make-pcless's kernelcache through iBEC's own code, under Unicorn.

    python3 tests/test-pcless-ibec.py build/firmware/iBEC.patched [BUNDLE]

Runs, from the decrypted iBEC (yours, from your IPSW; never in the repository):
the LZSS decoder 0x9ff245cc, adler32 0x9ff23a10, the Mach-O check 0x9ff1bf60 and
the segment loader 0x9ff1bf74, with the arguments 0x9ff1b874 passes them.  It
needs python3-unicorn (in the cascadia-build image).  It says the format is what
iBEC's code reads; it does not run the boot itself.
"""
import struct
import sys
from pathlib import Path

from unicorn import UC_ARCH_ARM, UC_MODE_THUMB, Uc, UcError
from unicorn.arm_const import UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_SP

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scripts"))
import importlib.util
spec = importlib.util.spec_from_file_location("mk", Path(__file__).resolve().parent.parent / "scripts/make-pcless.py")
mk = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mk)

BASE = 0x9FF00000
DECODE, ADLER, MACHO_OK, SEGMENTS = 0x9FF245CC, 0x9FF23A10, 0x9FF1BF60, 0x9FF1BF74
KSTATE = 0x9FF47EB0              # +0x14 virtual base, +0x18 physical base
STOP = 0x9FE00000
SP = 0x9FFE0000
SRC, DST, KERN = 0x84000000, 0x8C000000, 0x80000000


def machine(ibec):
    uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
    # enable VFP: not needed by these, and iBEC's own memcpy is integer
    uc.mem_map(BASE, 0x100000)
    uc.mem_write(BASE, ibec)
    uc.mem_map(STOP, 0x1000)
    uc.mem_map(KERN, 0x4000000)
    uc.mem_map(SRC, 0x4000000)
    uc.mem_map(DST, 0x4000000)
    return uc


def call(uc, fn, *args, stack=()):
    sp = SP - 0x100
    for i, v in enumerate(stack):
        uc.mem_write(sp + 4 * i, struct.pack("<I", v))
    for reg, v in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3), args):
        uc.reg_write(reg, v)
    uc.reg_write(UC_ARM_REG_SP, sp)
    uc.reg_write(UC_ARM_REG_LR, STOP | 1)
    try:
        uc.emu_start(fn | 1, STOP, timeout=600 * 1000000)
    except UcError as e:
        raise SystemExit(f"{fn:#x} faulted at {uc.reg_read(UC_ARM_REG_PC):#x}: {e}")
    return uc.reg_read(UC_ARM_REG_R0)


def main():
    ibec = Path(sys.argv[1]).read_bytes()
    bundle = Path(sys.argv[2]).read_bytes() if len(sys.argv) > 2 else bytes(range(256)) * 8000
    image = mk.macho(bundle)
    comp = mk.complzss(image)
    usize, csize = struct.unpack_from(">II", comp, 12)
    adler = struct.unpack_from(">I", comp, 8)[0]
    uc = machine(ibec)

    uc.mem_write(SRC, comp[0x180:])
    n = call(uc, DECODE, DST, 0x3F00000, SRC, csize)
    assert n == usize, f"decoder returned {n}, want {usize}"
    assert bytes(uc.mem_read(DST, usize)) == image, "decoded bytes differ"
    got = call(uc, ADLER, DST, usize)
    assert got == adler, f"adler {got:#x}, want {adler:#x}"
    print(f"iBEC decoder + adler32: {usize} bytes, {adler:#010x}: PASS")

    assert call(uc, MACHO_OK, DST) == 1, "not a Mach-O to iBEC"
    # as 0x9ff1b874 sets it up: virtual base from the first segment
    uc.mem_write(KSTATE + 0x14, struct.pack("<II", KERN & ~0xFFFFF, KERN))
    out = SP - 0x40
    r = call(uc, SEGMENTS, DST, KERN, out + 8, out + 4, stack=(out, 1))
    entry, top, base = struct.unpack("<III", bytes(uc.mem_read(out, 12)))
    print(f"segment loader: returned {r}, entry {entry:#x}, {base:#x}..{top:#x}")
    assert base == KERN and top == KERN + ((len(bundle) + 0xFFF) & ~0xFFF), "segment range"
    assert bytes(uc.mem_read(KERN, len(bundle))) == bundle, "the segment is not at 0x80000000"
    assert entry == KERN, f"entry {entry:#x}"
    print(f"the bundle is at {KERN:#x}, entry {entry:#x}: PASS")


if __name__ == "__main__":
    main()
