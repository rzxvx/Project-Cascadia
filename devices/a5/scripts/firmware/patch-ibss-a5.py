#!/usr/bin/env python3
"""
Patch a signature-patched iBSS for 4smulti_kloader in-iOS boot (legacy).

NOT used by the default checkm8 boot path — host irecovery upload needs stock
iBSS USB receive behaviour. Kept for reference and manual experiments only.

Three patches, all inside iBSS's initialisation function:

  1. load address   the literal pool word iBSS uses both as the DFU receive
                    buffer and as the address it later jumps to. Stock iOS 8.4.1
                    P105 uses 0x8FE00000; 4smulti_kloader copies iBEC to
                    0xBFD00000 (0x7FD00000 for stock multi_kloader).
  2. usb_wait       the call to usb_wait_for_image, so iBSS does not sit in DFU
                    waiting for a host upload that is never coming. Its return
                    value is the received length, which the image parser then
                    consumes, so rather than NOP the call outright this replaces
                    it with the same MOV.W that set up the buffer size. That
                    leaves a sane length in r0 instead of a stale register.
                    --usb-wait-nop restores the literal guide behaviour.
  3. BLT            the "retry" branch immediately after that call, which would
                    otherwise spin forever on a negative length.
  4. buffer wipe    movs r0, #1 before memclr_image_buffer (BL @ init+0x8fc).
                    With r0==1 that call memset's 2 MiB at the load address,
                    erasing the iBEC 4smulti_kloader just copied to 0xBFD00000.
                    Patch it to movs r0, #0 so the wipe path is skipped.

iOS 6/7 iBSS builds hold the load address in MOVT.W R4; iOS 8 and 9 load it
from a literal pool, which is why this patches a pool word rather than an
instruction. Offsets are located by pattern, not hardcoded, so a different
build of the same bootloader still patches correctly.

Reference: https://nyansatan.github.io/dualboot/patchingbootchain.html
"""
from __future__ import annotations

import argparse
import re
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

from capstone import CS_ARCH_ARM, CS_MODE_THUMB, Cs

READY_STRING = b"iBSS ready"
NOP2 = bytes.fromhex("00bf")
NOP4 = NOP2 * 2

# 4smulti_kloader copies the second image here; stock multi_kloader uses
# 0x7FD00000. Both verified by disassembling the shipped binaries
# (scripts/kloader-constants.py).
LOAD_ADDR_4S = 0xBFD00000
LOAD_ADDR_MULTI = 0x7FD00000

DRAM_LO, DRAM_HI = 0x80000000, 0xE0000000


@dataclass
class Sites:
    base: int
    load_pool: int
    load_value: int
    usb_wait_bl: int
    retry_branch: int | None
    jump_bl: int | None
    buffer_wipe_mov: int | None = None
    maxlen_mov: int | None = None
    maxlen: int | None = None


def thumb_movw_movt(reg: int, value: int) -> bytes:
    """Assemble movw/movt for a 32-bit immediate (needs arm-linux-gnueabihf-as)."""
    import subprocess
    import tempfile

    asm = f"movw r{reg}, #{value & 0xffff}\nmovt r{reg}, #{value >> 16}\n"
    with tempfile.TemporaryDirectory() as td:
        src = Path(td) / "x.S"
        obj = Path(td) / "x.o"
        src.write_text(f".thumb\n.text\n{asm}", encoding="ascii")
        subprocess.run(
            ["arm-linux-gnueabihf-as", "-mthumb", "-o", str(obj), str(src)],
            check=True,
            capture_output=True,
        )
        out = subprocess.run(
            ["arm-linux-gnueabihf-objcopy", "-O", "binary", "-j", ".text", str(obj), "/dev/stdout"],
            check=True,
            capture_output=True,
        )
        return out.stdout[:8]


def find_base(data: bytes) -> tuple[int, int]:
    """Return (image base, file offset of the 'iBSS ready' pool word)."""
    stroff = data.find(READY_STRING)
    if stroff < 0:
        raise SystemExit("no 'iBSS ready' string: is this really an iBSS?")

    for off in range(0, len(data) - 3, 4):
        value = struct.unpack_from("<I", data, off)[0]
        base = value - stroff
        if base and base & 0xFFFF == 0 and 0x20000000 <= base <= 0xA0000000:
            return base, off

    raise SystemExit("could not locate the pool reference to 'iBSS ready'")


def pool_target(insn) -> int | None:
    if not insn.mnemonic.startswith("ldr") or "[pc" not in insn.op_str:
        return None
    m = re.search(r"#(0x[0-9a-f]+)", insn.op_str)
    if not m:
        return None
    return ((insn.address + 4) & ~3) + int(m.group(1), 16)


def branch_target(insn) -> int | None:
    m = re.search(r"#(0x[0-9a-f]+)", insn.op_str)
    return int(m.group(1), 16) if m else None


def find_load_ldr(data: bytes, base: int, ready_pool: int) -> tuple[int, int, int]:
    """Find the LDR that loads the DFU buffer / jump address.

    Probing each halfword independently rather than disassembling a range: the
    start of the function is not known ahead of time, and a linear sweep that
    begins mid-instruction desynchronises and hides the very LDR we are after.
    """
    md = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
    # Its pool word neighbours the 'iBSS ready' pointer in the same pool block.
    pool_lo, pool_hi = max(0, ready_pool - 0x40), ready_pool

    for at in range(0, min(len(data), ready_pool), 2):
        insns = list(md.disasm(data[at : at + 4], at + 1))
        if not insns:
            continue
        # Disassembly is addressed by file offset, so this is already one.
        off = pool_target(insns[0])
        if off is None:
            continue
        if not (pool_lo <= off < pool_hi) or off + 4 > len(data):
            continue
        value = struct.unpack_from("<I", data, off)[0]
        # The DFU receive buffer is a megabyte-aligned DRAM address.
        if DRAM_LO <= value < DRAM_HI and value & 0xFFFFF == 0:
            return at, off, value

    raise SystemExit("could not locate the load-address LDR in the init function")


def locate(data: bytes) -> Sites:
    """Identify every site the patches need to touch."""
    base, ready_pool = find_base(data)
    ldr_at, load_pool, load_value = find_load_ldr(data, base, ready_pool)

    # From a known instruction boundary a linear sweep stays in sync.
    md = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
    scan_start = max(0, ready_pool - 0x120)
    pre_insns = list(md.disasm(data[scan_start:ready_pool], scan_start + 1))
    insns = list(md.disasm(data[ldr_at:ready_pool], ldr_at + 1))

    usb_wait = retry = jump = buffer_wipe = None
    maxlen_mov = maxlen = None

    for insn in pre_insns:
        if insn.mnemonic == "movs" and insn.op_str == "r0, #1":
            nxt = [x for x in pre_insns if x.address > insn.address and x.mnemonic == "bl"]
            if nxt and nxt[0].address - insn.address <= 6:
                buffer_wipe = insn.address - 1
                break

    for insn in insns:
        if usb_wait is None:
            # The buffer-size argument set up just before the call.
            m = re.match(r"^r\d+, #(0x[0-9a-f]+|\d+)$", insn.op_str)
            if insn.mnemonic == "mov.w" and m and insn.size == 4:
                maxlen_mov, maxlen = insn.address - 1, int(m.group(1), 0)
            if insn.mnemonic == "bl":
                usb_wait = insn.address - 1
            continue
        if retry is None and insn.mnemonic in ("blt", "bmi"):
            retry = insn.address - 1

    # The final call in the function is execute-image; NOPing it is a brick.
    for insn in reversed(insns):
        if insn.mnemonic == "bl":
            jump = insn.address - 1
            break

    if usb_wait is None:
        raise SystemExit("found the load address but no usb_wait_for_image call")

    return Sites(
        base,
        load_pool,
        load_value,
        usb_wait,
        retry,
        jump,
        buffer_wipe,
        maxlen_mov,
        maxlen,
    )


def patch(
    data: bytearray,
    sites: Sites,
    load_addr: int,
    *,
    image_size: int | None = None,
    nop_usb_wait: bool = True,
    nop_retry: bool = True,
    force_nop: bool = False,
) -> list[str]:
    notes: list[str] = []

    old = struct.unpack_from("<I", data, sites.load_pool)[0]
    struct.pack_into("<I", data, sites.load_pool, load_addr)
    notes.append(f"load address  0x{sites.load_pool:04x}  0x{old:08x} -> 0x{load_addr:08x}")

    if nop_usb_wait:
        off = sites.usb_wait_bl
        was = bytes(data[off : off + 8])
        if force_nop:
            data[off : off + 4] = NOP4
            how = "BL -> NOP NOP"
        elif image_size is not None:
            enc = thumb_movw_movt(5, image_size)
            data[off : off + 8] = enc
            how = f"BL+setup -> movw/movt r5, #{image_size} (#0x{image_size:x})"
        elif sites.maxlen_mov is not None:
            enc = thumb_movw_movt(5, sites.maxlen)
            data[off : off + 8] = enc
            how = f"BL+setup -> movw/movt r5, #{sites.maxlen:#x}"
        else:
            data[off : off + 4] = NOP4
            how = "BL -> NOP NOP"
        notes.append(f"usb_wait      0x{off:04x}  {was.hex()} -> {bytes(data[off:off + 8]).hex()}  ({how})")

    if nop_retry and sites.retry_branch is not None:
        off = sites.retry_branch
        was = bytes(data[off : off + 2])
        data[off : off + 2] = NOP2
        notes.append(f"retry branch  0x{off:04x}  {was.hex()} -> {NOP2.hex()}  (BLT -> NOP)")

    if sites.buffer_wipe_mov is not None:
        off = sites.buffer_wipe_mov
        was = bytes(data[off : off + 2])
        if was == bytes.fromhex("0120"):
            data[off : off + 2] = bytes.fromhex("0020")
            notes.append(f"buffer wipe   0x{off:04x}  {was.hex()} -> 0020  (movs r0,#1 -> movs r0,#0)")
        elif was != bytes.fromhex("0020"):
            raise SystemExit(f"unexpected buffer wipe mov @ 0x{off:x}: {was.hex()}")

    return notes


def verify(data: bytes, sites: Sites) -> None:
    """Prove the execute-image call survived; NOPing it is a silent brick."""
    if sites.jump_bl is None:
        return
    md = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
    insns = list(md.disasm(data[sites.jump_bl : sites.jump_bl + 4], sites.jump_bl + 1))
    if not insns or insns[0].mnemonic != "bl":
        raise SystemExit(
            f"execute-image call at 0x{sites.jump_bl:x} is no longer a BL "
            f"({data[sites.jump_bl:sites.jump_bl + 4].hex()}) - iBSS would never "
            "reach iBEC. Re-run against a clean iBSS.prepatched."
        )
    print(f"verify        0x{sites.jump_bl:04x}  execute-image BL intact")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    ap.add_argument("input", type=Path, help="iBSS.prepatched (iBoot32Patcher output)")
    ap.add_argument("output", type=Path, help="patched iBSS")
    ap.add_argument(
        "--loader",
        choices=("4smulti", "multi"),
        default="4smulti",
        help="which kloader will place iBEC (default: 4smulti, for A5)",
    )
    ap.add_argument("--load-addr", type=lambda s: int(s, 0), help="override the jump address")
    ap.add_argument("--keep-usb-wait", action="store_true", help="leave usb_wait_for_image intact")
    ap.add_argument("--keep-retry", action="store_true", help="do not NOP the retry branch")
    ap.add_argument(
        "--usb-wait-nop",
        action="store_true",
        help="NOP the usb_wait call outright instead of leaving a length in r0",
    )
    ap.add_argument(
        "--ibec-img3",
        type=Path,
        help="IMG3-wrapped iBEC; its file size is written into r5 instead of usb_wait",
    )
    ap.add_argument("--show", action="store_true", help="report the patch sites and exit")
    args = ap.parse_args()

    data = bytearray(args.input.read_bytes())
    sites = locate(bytes(data))

    print(f"image base    0x{sites.base:08x}")
    print(f"load pool     0x{sites.load_pool:04x} = 0x{sites.load_value:08x}")
    print(f"usb_wait BL   0x{sites.usb_wait_bl:04x}")
    print(
        f"maxlen mov    {f'0x{sites.maxlen_mov:04x} = {sites.maxlen:#x}' if sites.maxlen_mov else 'not found'}"
    )
    print(f"retry branch  {f'0x{sites.retry_branch:04x}' if sites.retry_branch else 'not found'}")
    print(f"buffer wipe   {f'0x{sites.buffer_wipe_mov:04x}' if sites.buffer_wipe_mov else 'not found'}")
    print(f"execute BL    {f'0x{sites.jump_bl:04x}' if sites.jump_bl else 'not found'}")
    print()

    if args.show:
        return 0

    load_addr = args.load_addr
    if load_addr is None:
        load_addr = LOAD_ADDR_4S if args.loader == "4smulti" else LOAD_ADDR_MULTI

    image_size = None
    if args.ibec_img3:
        if not args.ibec_img3.is_file():
            raise SystemExit(f"missing --ibec-img3: {args.ibec_img3}")
        image_size = args.ibec_img3.stat().st_size
        print(f"ibec img3     {args.ibec_img3.name} = {image_size} bytes (#0x{image_size:x})")

    notes = patch(
        data,
        sites,
        load_addr,
        image_size=image_size,
        nop_usb_wait=not args.keep_usb_wait,
        nop_retry=not args.keep_retry,
        force_nop=args.usb_wait_nop,
    )
    for n in notes:
        print(n)

    verify(bytes(data), sites)

    args.output.write_bytes(bytes(data))
    print(f"\nwrote {args.output} ({len(data)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
