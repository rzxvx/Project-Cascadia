#!/usr/bin/env python3
"""Decode synthetic accessory IDs with actual 12H321 firmware, offline only.

Runs two small IOAccessoryManager functions in fake RAM. Results are requested
internal routing FourCCs, not electrical pin names, AppleCableType or VBUS.
The opaque platform predicate is explicitly supplied as a synthetic fixture.
There is no device, network, I2C, MMIO, deploy or authentication backend.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct

from unicorn import Uc, UcError, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_CODE
from unicorn.arm_const import (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,
                               UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC)

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("usb_evidence", ROOT / "scripts/usb-phy-plan.py")
evidence = importlib.util.module_from_spec(spec)
spec.loader.exec_module(evidence)
KLO, KHI = 0x80710000, 0x80725000
RAM, STACK, END = 0x10000000, 0x20000000, 0x30000FFC
DECODERS = {"data-modes": 0x80713C84, "accessory-modes": 0x80713DC4}


def experiment(data, accessory_id, decoder="data-modes", platform_predicate=0):
    if hashlib.sha256(data).hexdigest() != evidence.KC_SHA256:
        raise ValueError("kernelcache SHA256 mismatch; fixed firmware addresses refused")
    if not isinstance(accessory_id, bytes) or len(accessory_id) != 6:
        raise ValueError("accessory ID must contain exactly six synthetic bytes")
    if decoder not in DECODERS or platform_predicate not in (0, 1):
        raise ValueError("invalid offline decoder or synthetic platform predicate")
    mu = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
    mu.mem_map(KLO, KHI - KLO)
    start, last = (evidence.address_offset(data, a) for a in (KLO, KHI - 1))
    if last - start != KHI - KLO - 1:
        raise ValueError("IOAccessoryManager image mapping is not contiguous")
    mu.mem_write(KLO, data[start:last + 1])
    for address, size in ((RAM, 0x10000), (STACK, 0x10000), (END & ~0xFFF, 0x1000)):
        mu.mem_map(address, size)
    mu.mem_write(RAM, accessory_id)
    mu.mem_write(RAM + 0x100, b"\xA5" * 8)
    # Substitute only the process-local stack canary pointer in fake memory.
    canary_pointer = evidence.pic_literal_pointer(data, 0x80713C90, 0x80713C96, 0)
    mu.mem_write(canary_pointer, struct.pack("<I", RAM + 0x200))
    mu.mem_write(RAM + 0x200, struct.pack("<I", 0x13579BDF))
    calls = []

    def stub(machine, address, size, _):
        if address == 0x8071D56C:
            calls.append({"stub": hex(address), "argument": machine.reg_read(UC_ARM_REG_R0),
                          "synthetic_return": platform_predicate})
            machine.reg_write(UC_ARM_REG_R0, platform_predicate)
            machine.reg_write(UC_ARM_REG_PC, machine.reg_read(UC_ARM_REG_LR))

    mu.hook_add(UC_HOOK_CODE, stub)
    for register, value in ((UC_ARM_REG_R0, RAM), (UC_ARM_REG_R1, RAM + 0x100),
                            (UC_ARM_REG_R2, RAM + 0x104), (UC_ARM_REG_SP, STACK + 0xFF00),
                            (UC_ARM_REG_LR, END | 1)):
        mu.reg_write(register, value)
    mu.emu_start(DECODERS[decoder] | 1, END, count=20000)
    if mu.reg_read(UC_ARM_REG_PC) != END:
        raise RuntimeError("firmware did not return within the instruction budget")
    values = struct.unpack("<2I", mu.mem_read(RAM + 0x100, 8))
    return {"decoder": decoder, "function": hex(DECODERS[decoder]),
            "synthetic_accessory_id": accessory_id.hex(),
            "output_fourcc": [v.to_bytes(4, "big").decode("ascii") if v else None for v in values],
            "raw_outputs": [hex(v) for v in values],
            "firmware_return_r0": hex(mu.reg_read(UC_ARM_REG_R0)),
            "stubbed_platform_predicate": calls,
            "physical_pin_mapping_verified": False, "apple_cable_type_known": False,
            "hardware_access": False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kernelcache", type=Path)
    args = parser.parse_args()
    try:
        data = args.kernelcache.read_bytes()
        # A normal cable ID from an independent first-person research report,
        # and deliberately synthetic edge cases. No camera-adapter fixture.
        results = [experiment(data, bytes.fromhex(value), decoder, predicate)
                   for value in ("100c00000000", "000000000000", "400000000000",
                                 "800000000000", "c00000000000", "240000000000")
                   for decoder in DECODERS for predicate in (0, 1)]
    except (OSError, ValueError, RuntimeError, UnicodeError, UcError) as error:
        parser.exit(1, f"error: {error}\n")
    print(json.dumps({"kernelcache_sha256": evidence.KC_SHA256,
                      "hardware_access": False,
                      "limitations": ["IDs and platform predicate are synthetic",
                                      "routing selection is not AppleCableType, authentication or power"],
                      "experiments": results}, indent=2))


if __name__ == "__main__":
    main()
