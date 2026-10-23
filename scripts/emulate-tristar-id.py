#!/usr/bin/env python3
"""Run the pinned 12H321 THS7383 ID readers in fake RAM and fake I2C.

No hardware, network, file-device or USB interface exists in this program.
Synthetic register replies validate byte order and error propagation, not
accessory authenticity, camera-adapter recognition, electrical routing or VBUS.
Unicorn is already available in the existing Cascadia build image.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct

from unicorn import Uc, UcError, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_CODE
from unicorn.arm_const import (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,
                               UC_ARM_REG_R3, UC_ARM_REG_SP, UC_ARM_REG_LR,
                               UC_ARM_REG_PC)

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("usb_phy_evidence", ROOT / "scripts/usb-phy-plan.py")
evidence = importlib.util.module_from_spec(spec)
spec.loader.exec_module(evidence)
KLO, KHI = 0x80884000, 0x8088D000
OBJECT, OUTPUT, PROVIDER, PROVIDER_VTABLE = (0x10000000, 0x10001000, 0x10002000, 0x10003000)
STACK, TRAPS, END = 0x20000000, 0x30000000, 0x30000FFC
READ, CLASS_NAME = TRAPS, TRAPS + 4
INSTANCE_VTABLE = 0x80889298
FUNCTIONS = {"system-id": (0x80886BA8, 0x37C, 3, 2),
             "id-response": (0x80886BE8, 0x380, 7, 6)}


def experiment(data, reader, reply, status=0x80, fail_transfer=None):
    if hashlib.sha256(data).hexdigest() != evidence.KC_SHA256:
        raise ValueError("kernelcache SHA256 mismatch; fixed firmware addresses refused")
    if reader not in FUNCTIONS:
        raise ValueError("only the two audited THS7383 ID readers accepted")
    function, slot, reply_size, output_size = FUNCTIONS[reader]
    if not isinstance(reply, bytes) or len(reply) != reply_size:
        raise ValueError(f"{reader} requires exactly {reply_size} synthetic reply bytes")
    if not 0 <= status <= 255 or fail_transfer not in (None, 0, 1):
        raise ValueError("invalid synthetic status or transfer failure")
    # Verify class constructor and dispatch rather than confusing the metaclass
    # table (0x80889658) with the actual instance table.
    actual = evidence.pic_literal_pointer(data, 0x808869FA, 0x808869FC, 0) + 8
    target = struct.unpack_from("<I", data, evidence.address_offset(data, actual + slot))[0]
    if actual != INSTANCE_VTABLE or target != function | 1:
        raise ValueError("audited THS7383 instance dispatch changed")
    mu = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
    mu.mem_map(KLO, KHI - KLO)
    start = evidence.address_offset(data, KLO)
    last = evidence.address_offset(data, KHI - 1)
    if last - start != KHI - KLO - 1:
        raise ValueError("THS7383 image mapping is not contiguous")
    mu.mem_write(KLO, data[start:last + 1])
    for address, size in ((OBJECT, 0x10000), (STACK, 0x10000), (TRAPS, 0x1000)):
        mu.mem_map(address, size)

    def w32(address, value):
        mu.mem_write(address, struct.pack("<I", value))

    w32(OBJECT, INSTANCE_VTABLE)
    w32(OBJECT + 0x6C, PROVIDER)
    w32(PROVIDER, PROVIDER_VTABLE)
    w32(PROVIDER_VTABLE + 0x3D4, READ | 1)
    # Invalid marker path logs the class name, with no semantic effect on the ID.
    w32(INSTANCE_VTABLE + 0xF0, CLASS_NAME | 1)
    mu.mem_write(OUTPUT, b"\xA5" * output_size)
    mu.mem_write(OBJECT + 0x5000, b"synthetic-THS7383\0")
    mu.mem_write(TRAPS, b"\x70\x47" * 4)
    transactions, logs = [], []

    def hook(machine, address, size, _):
        if address == READ:
            obj, register, buffer, count = [machine.reg_read(r) for r in
                                           (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)]
            expected = [(0x20, 3)] if reader == "system-id" else [(0x39, 7), (0x0C, 1)]
            index = len(transactions)
            if obj != PROVIDER or index >= len(expected) or (register, count) != expected[index]:
                raise RuntimeError("unexpected firmware I2C request")
            if not STACK <= buffer < STACK + 0x10000 - count:
                raise RuntimeError("firmware read buffer outside synthetic stack")
            result = 0xE00002C0 if index == fail_transfer else 0
            value = reply if index == 0 else bytes([status])
            transactions.append({"direction": "read", "register": hex(register),
                                 "count": count, "reply": value.hex(), "return_code": hex(result)})
            if not result:
                machine.mem_write(buffer, value)
            machine.reg_write(UC_ARM_REG_R0, result)
        elif address == CLASS_NAME:
            machine.reg_write(UC_ARM_REG_R0, OBJECT + 0x5000)
        elif address == 0x808878DC:
            logs.append("firmware invalid-marker log stubbed")
            machine.reg_write(UC_ARM_REG_R0, 0)
        else:
            return
        machine.reg_write(UC_ARM_REG_PC, machine.reg_read(UC_ARM_REG_LR))

    mu.hook_add(UC_HOOK_CODE, hook)
    mu.reg_write(UC_ARM_REG_R0, OBJECT)
    mu.reg_write(UC_ARM_REG_R1, OUTPUT)
    mu.reg_write(UC_ARM_REG_SP, STACK + 0xFF00)
    mu.reg_write(UC_ARM_REG_LR, END | 1)
    mu.emu_start(function | 1, END, count=20000)
    if mu.reg_read(UC_ARM_REG_PC) != END:
        raise RuntimeError("firmware did not return within the instruction budget")
    return {"reader": reader, "function": hex(function), "instance_vtable": hex(actual),
            "return_code": hex(mu.reg_read(UC_ARM_REG_R0)),
            "output": bytes(mu.mem_read(OUTPUT, output_size)).hex(),
            "transactions": transactions, "stubbed_logs": logs,
            "hardware_access": False, "role_or_authentication_verified": False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kernelcache", type=Path)
    args = parser.parse_args()
    try:
        data = args.kernelcache.read_bytes()
        # Arbitrary asymmetric bytes make firmware byte order observable.
        reply = bytes.fromhex("66554433221175")
        results = [experiment(data, "id-response", reply, status=x) for x in (0x80, 0xC0, 0x7F)]
        results += [experiment(data, "id-response", reply[:-1] + b"\x74"),
                    experiment(data, "id-response", reply, fail_transfer=0),
                    experiment(data, "id-response", reply, fail_transfer=1),
                    experiment(data, "system-id", bytes.fromhex("743f01")),
                    experiment(data, "system-id", bytes.fromhex("753f01"))]
    except (OSError, ValueError, RuntimeError, UcError) as error:
        parser.exit(1, f"error: {error}\n")
    print(json.dumps({"kernelcache_sha256": evidence.KC_SHA256,
                      "hardware_access": False,
                      "limitations": ["all register responses are synthetic",
                                      "no CRC/wire decoder, authentication, adapter role or voltage model"],
                      "experiments": results}, indent=2))


if __name__ == "__main__":
    main()
