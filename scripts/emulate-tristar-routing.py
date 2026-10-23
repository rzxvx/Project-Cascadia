#!/usr/bin/env python3
"""Run pinned 12H321 THS7383 routing code with synthetic I2C and orientation.

No USB, I2C, network or device backend exists. This proves firmware branches,
not physical pin routing, an adapter ID or a safe VBUS source. Only the
orientation-property getter and the fake I2C provider are intercepted.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct

from unicorn import Uc, UcError, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_CODE
from unicorn.arm_const import (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,
                               UC_ARM_REG_R3, UC_ARM_REG_PC, UC_ARM_REG_SP, UC_ARM_REG_LR)

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('usb_phy_evidence', ROOT / 'scripts/usb-phy-plan.py')
evidence = importlib.util.module_from_spec(spec)
spec.loader.exec_module(evidence)
OBJECT, PROVIDER, PROVIDER_VTABLE = 0x10000000, 0x10001000, 0x10002000
STACK, TRAP, END = 0x20000000, 0x30000000, 0x30000FFC
SELECTORS = (None, 'urt0', 'urt1', 'apwr', 'apwq', 'idbs', 'jtag', 'usb0')


def experiment(data, first, second, orientation):
    if hashlib.sha256(data).hexdigest() != evidence.KC_SHA256:
        raise ValueError('kernelcache SHA256 mismatch; fixed addresses refused')
    if first not in SELECTORS or second not in SELECTORS:
        raise ValueError('only documented routing selectors supported')
    if isinstance(orientation, bool) or not isinstance(orientation, int) or orientation not in (0, 1, 2):
        raise ValueError('synthetic orientation must be 0, 1 or 2')
    actual = evidence.pic_literal_pointer(data, 0x808869FA, 0x808869FC, 0) + 8
    if actual != 0x80889298:
        raise ValueError('audited THS7383 instance vtable changed')
    target = struct.unpack_from('<I', data, evidence.address_offset(data, actual + 0x394))[0]
    if target != 0x80886F6D:
        raise ValueError('audited routing dispatch changed')
    getter = struct.unpack_from('<I', data, evidence.address_offset(data, 0x80888090))[0]
    if getter != 0x80713945:
        raise ValueError('audited orientation import changed')
    machine = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
    start, end = 0x80884000, 0x8088D000
    offset, last = evidence.address_offset(data, start), evidence.address_offset(data, end - 1)
    if last - offset != end - start - 1:
        raise ValueError('firmware mapping not contiguous')
    machine.mem_map(start, end - start)
    machine.mem_write(start, data[offset:last + 1])
    for address, size in ((OBJECT, 0x10000), (STACK, 0x10000), (TRAP, 0x1000)):
        machine.mem_map(address, size)
    for address, value in ((OBJECT, actual), (OBJECT + 0x6C, PROVIDER),
                           (PROVIDER, PROVIDER_VTABLE), (PROVIDER_VTABLE + 0x3D0, TRAP | 1)):
        machine.mem_write(address, struct.pack('<I', value))
    writes, getter_calls = [], []

    def code(mu, address, size, _):
        if address == TRAP:
            owner, register, buffer, length = [mu.reg_read(r) for r in
                (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)]
            if owner != PROVIDER or register != 2 or length != 1:
                raise RuntimeError('unexpected synthetic I2C provider request')
            writes.append({'register': register, 'value': bytes(mu.mem_read(buffer, length)).hex()})
            mu.reg_write(UC_ARM_REG_R0, 0)
        elif address == 0x80887ACC:
            getter_calls.append({'stub': hex(address), 'synthetic_return': orientation})
            mu.reg_write(UC_ARM_REG_R0, orientation)
        else:
            return
        mu.reg_write(UC_ARM_REG_PC, mu.reg_read(UC_ARM_REG_LR))

    machine.hook_add(UC_HOOK_CODE, code)
    for register, value in ((UC_ARM_REG_R0, OBJECT),
                           (UC_ARM_REG_R1, int.from_bytes(first.encode(), 'big') if first else 0),
                           (UC_ARM_REG_R2, int.from_bytes(second.encode(), 'big') if second else 0),
                           (UC_ARM_REG_SP, STACK + 0xFF00), (UC_ARM_REG_LR, END | 1)):
        machine.reg_write(register, value)
    machine.emu_start(target, END, count=10000)
    if machine.reg_read(UC_ARM_REG_PC) != END:
        raise RuntimeError('firmware did not return within instruction budget')
    return {'first_selector': first, 'second_selector': second, 'orientation': orientation,
            'return_code': hex(machine.reg_read(UC_ARM_REG_R0)), 'i2c_writes': writes,
            'orientation_getter_calls': getter_calls, 'hardware_access': False,
            'vbus_source_or_voltage_verified': False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kernelcache', type=Path)
    args = parser.parse_args()
    pairs = [(None, None), ('urt0', None), ('urt1', None), ('apwr', None), ('apwq', None),
             ('idbs', None), ('jtag', None), ('usb0', None), (None, 'idbs'), ('idbs', 'urt0')]
    try:
        data = args.kernelcache.read_bytes()
        results = [experiment(data, a, b, orientation) for a, b in pairs for orientation in (0, 1, 2)]
    except (OSError, ValueError, RuntimeError, UcError) as error:
        parser.exit(1, f'error: {error}\n')
    print(json.dumps({'kernelcache_sha256': evidence.KC_SHA256, 'hardware_access': False,
                      'experiments': results,
                      'limitations': ['orientation property supplied synthetically',
                                      'register 2 electrical wiring and VBUS not modeled']}, indent=2))


if __name__ == '__main__':
    main()
