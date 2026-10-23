#!/usr/bin/env python3
"""Execute audited accessory LDO functions in fake RAM/PMU registers only.

No hardware, network or device interface. Requires the pinned official 12H321
kernelcache and Unicorn, as available in the existing Cascadia build image.
This validates firmware transactions, not voltage, wiring or VBUS operation.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct

from unicorn import UC_HOOK_CODE, UcError
from unicorn.arm_const import (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,
                               UC_ARM_REG_R3, UC_ARM_REG_SP, UC_ARM_REG_LR,
                               UC_ARM_REG_PC)

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("d1946_emulator", ROOT / "tools/power/emulate-d1946.py")
backend = importlib.util.module_from_spec(spec)
spec.loader.exec_module(backend)
SET_LDO = 0x80ECE79C
LOCK_STUB, UNLOCK_STUB = 0x80ED1CDC, 0x80ED1CFC


def experiment(data, index, enable, voltage=0, options=0, fail_transfer=None):
    if hashlib.sha256(data).hexdigest() != backend.EXPECTED_SHA256:
        raise ValueError("kernelcache SHA256 mismatch; fixed firmware addresses refused")
    if index not in (5, 22):
        raise ValueError("only the two audited dock accessory LDO indices accepted")
    if enable not in (0, 1) or voltage < 0 or voltage > 0xffff or options not in (0, 1):
        raise ValueError("invalid offline experiment parameters")
    e = backend.Experiment(data, fail_transfer=fail_transfer)
    # Deliberately varied unrelated bits, entirely synthetic.
    e.registers[0x21] = 0xa2 if enable else 0xa6
    e.registers[0x26] = 0x59 if enable else 0x5d
    e.registers[0x34] = 0xd5
    e.registers[0x7c] = 0xa9
    initial = bytes(e.registers)

    def lock_stub(mu, address, size, _):
        if address in (LOCK_STUB, UNLOCK_STUB):
            # Firmware's recursive lock has no hardware effect; this isolated
            # single-threaded experiment substitutes only lock/unlock stubs.
            mu.reg_write(UC_ARM_REG_R0, 0)
            mu.reg_write(UC_ARM_REG_PC, mu.reg_read(UC_ARM_REG_LR))
    e.mu.hook_add(UC_HOOK_CODE, lock_stub, begin=LOCK_STUB, end=UNLOCK_STUB)
    stack = backend.STACK + 0xff00
    e.w32(stack, enable)
    e.mu.reg_write(UC_ARM_REG_SP, stack)
    e.mu.reg_write(UC_ARM_REG_LR, backend.END | 1)
    for register, value in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3),
                               (backend.PMU, index, voltage, options)):
        e.mu.reg_write(register, value)
    e.mu.emu_start(SET_LDO | 1, backend.END, count=20000)
    if e.mu.reg_read(UC_ARM_REG_PC) != backend.END:
        raise RuntimeError("firmware did not return within the instruction budget")
    return {"ldo_index": index, "requested_enable": enable,
            "requested_voltage_firmware_units": voltage, "options": options,
            "return_code": hex(e.mu.reg_read(UC_ARM_REG_R0)),
            "transactions": e.transactions,
            "changed_synthetic_registers": {hex(i): {"before": hex(initial[i]), "after": hex(e.registers[i])}
                                             for i in range(256) if initial[i] != e.registers[i]},
            "hardware_access": False, "vbus_verified": False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kernelcache", type=Path)
    args = parser.parse_args()
    try:
        data = args.kernelcache.read_bytes()
        experiments = [experiment(data, index, enable) for index in (5, 22) for enable in (0, 1)]
        experiments += [experiment(data, 5, 1, voltage=3300),
                        experiment(data, 5, 1, voltage=5000),
                        experiment(data, 5, 1, options=1),
                        experiment(data, 5, 1, fail_transfer=0),
                        experiment(data, 5, 1, fail_transfer=1)]
    except (OSError, ValueError, RuntimeError, UcError) as error:
        parser.exit(1, f"error: {error}\n")
    print(json.dumps({"kernelcache_sha256": backend.EXPECTED_SHA256,
                      "hardware_access": False, "voltage_or_vbus_measured": False,
                      "limitations": ["register contents and transfer failures are synthetic",
                                      "no electrical model, accessory identification, analog protection or IRQs"],
                      "experiments": experiments}, indent=2))


if __name__ == "__main__":
    main()
