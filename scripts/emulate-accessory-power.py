#!/usr/bin/env python3
"""Execute audited 12H321 accessory rail callbacks and ID-derived voltage code.

Everything executes in Unicorn RAM with fake ARMFunction providers. There is
no USB, PMU, GPIO, I2C or device backend. Values are firmware units; this does
not prove a 5 V VBUS source, physical wiring, protection or safe enable policy.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct

from unicorn import Uc, UcError, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_CODE
from unicorn.arm_const import (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,
                               UC_ARM_REG_R3, UC_ARM_REG_R4, UC_ARM_REG_PC,
                               UC_ARM_REG_SP, UC_ARM_REG_LR)

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('usb_phy_evidence', ROOT / 'scripts/usb-phy-plan.py')
evidence = importlib.util.module_from_spec(spec)
spec.loader.exec_module(evidence)
OBJECT, PROVIDER, STACK, TRAP, END = 0x10000000, 0x10001000, 0x20000000, 0x30000000, 0x30000FFC
NAMES = ('function-acc_pwr', 'function-acc_sw_en', 'function-acc_sleep_pwr')


def integer(value, maximum, label):
    if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= maximum:
        raise ValueError(f'{label} must be an integer between 0 and {maximum}')


def setup(data, errors=(0, 0, 0), optional=(True, True)):
    if hashlib.sha256(data).hexdigest() != evidence.KC_SHA256:
        raise ValueError('kernelcache SHA256 mismatch; fixed addresses refused')
    if len(errors) != 3:
        raise ValueError('three synthetic provider results required')
    for error in errors:
        integer(error, 0xFFFFFFFF, 'provider result')
    if len(optional) != 2 or any(type(x) is not bool for x in optional):
        raise ValueError('two boolean optional-provider flags required')
    actual = evidence.pic_literal_pointer(data, 0x80885368, 0x8088536A, 0) + 8
    if actual != 0x80888998:
        raise ValueError('audited BuiltInUSBDevice instance vtable changed')
    for slot, target in ((0x368, 0x808857C5), (0x39C, 0x80885829)):
        if struct.unpack_from('<I', data, evidence.address_offset(data, actual + slot))[0] != target:
            raise ValueError('audited accessory power dispatch changed')
    mu = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
    for start, end in ((0x80710000, 0x8072F000), (0x80884000, 0x8088D000)):
        first, last = evidence.address_offset(data, start), evidence.address_offset(data, end - 1)
        if last - first != end - start - 1:
            raise ValueError('firmware mapping not contiguous')
        mu.mem_map(start, end - start)
        mu.mem_write(start, data[first:last + 1])
    for address, size in ((OBJECT, 0x10000), (STACK, 0x10000), (TRAP, 0x1000)):
        mu.mem_map(address, size)
    mu.mem_write(OBJECT, struct.pack('<I', actual))
    calls, replans = [], []
    for index, field in enumerate((0x118, 0x11C, 0x120)):
        owner = PROVIDER + 0x200 * index
        vtable = owner + 0x100
        present = index == 0 or optional[index - 1]
        mu.mem_write(OBJECT + field, struct.pack('<I', owner if present else 0))
        mu.mem_write(owner, struct.pack('<I', vtable))
        mu.mem_write(vtable + 0x3C, struct.pack('<I', (TRAP + 4 * index) | 1))

    def code(machine, address, size, _):
        if address in (TRAP, TRAP + 4, TRAP + 8):
            index = (address - TRAP) // 4
            owner, first, second, last = [machine.reg_read(r) for r in
                (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)]
            if owner != PROVIDER + 0x200 * index or last != 0:
                raise RuntimeError('unexpected synthetic ARMFunction dispatch')
            args = [struct.unpack('<I', machine.mem_read(first, 4))[0]]
            if second:
                args.append(struct.unpack('<I', machine.mem_read(second, 4))[0])
            calls.append({'function': NAMES[index], 'arguments': args})
            machine.reg_write(UC_ARM_REG_R0, errors[index])
        elif address == 0x807122E8:
            replans.append('IOAccessoryManager power replan intercepted')
            machine.reg_write(UC_ARM_REG_R0, 0)
        else:
            return
        machine.reg_write(UC_ARM_REG_PC, machine.reg_read(UC_ARM_REG_LR))

    mu.hook_add(UC_HOOK_CODE, code)
    return mu, calls, replans


def power_callbacks(data, enable, switch_enable, sleep_enable, errors=(0, 0, 0), optional=(True, True)):
    mu, calls, _ = setup(data, errors, optional)
    for value, label in ((enable, 'enable'), (switch_enable, 'switch enable'), (sleep_enable, 'sleep enable')):
        integer(value, 1, label)
    for register, value in ((UC_ARM_REG_R0, OBJECT), (UC_ARM_REG_R1, enable),
                           (UC_ARM_REG_R2, switch_enable), (UC_ARM_REG_R3, sleep_enable),
                           (UC_ARM_REG_SP, STACK + 0xFF00), (UC_ARM_REG_LR, END | 1)):
        mu.reg_write(register, value)
    mu.emu_start(0x808857C5, END, count=10000)
    if mu.reg_read(UC_ARM_REG_PC) != END:
        raise RuntimeError('firmware did not return within instruction budget')
    return {'callbacks': calls, 'return_code': hex(mu.reg_read(UC_ARM_REG_R0)),
            'cached_enable': mu.mem_read(OBJECT + 0x138, 1)[0],
            'hardware_access': False, 'vbus_source_or_voltage_verified': False}


def id_voltage(data, id_byte, cached_class, cached_enable=1):
    mu, calls, replans = setup(data)
    integer(id_byte, 255, 'ID byte')
    integer(cached_class, 15, 'cached class')
    integer(cached_enable, 1, 'cached enable')
    stack = STACK + 0xFF00
    mu.mem_write(stack + 0x25, bytes([id_byte]))
    mu.mem_write(OBJECT + 0xB1, bytes([cached_class]))
    mu.mem_write(OBJECT + 0x138, bytes([cached_enable]))
    mu.reg_write(UC_ARM_REG_R4, OBJECT)
    mu.reg_write(UC_ARM_REG_SP, stack)
    # This audited block is part of the larger ID-event policy. The surrounding
    # authentication, routing and power state machinery is not emulated here.
    mu.emu_start(0x807150E7, 0x80715194, count=10000)
    if mu.reg_read(UC_ARM_REG_PC) != 0x80715194:
        raise RuntimeError('firmware did not reach audited block boundary')
    return {'id_byte': hex(id_byte), 'prior_class': cached_class,
            'new_class': mu.mem_read(OBJECT + 0xB1, 1)[0], 'callbacks': calls,
            'replans': replans, 'hardware_access': False,
            'vbus_source_or_voltage_verified': False,
            'unit': 'firmware voltage units; no measured output'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kernelcache', type=Path)
    args = parser.parse_args()
    try:
        data = args.kernelcache.read_bytes()
        powers = [power_callbacks(data, *values) for values in ((0, 0, 0), (1, 0, 1), (1, 1, 0))]
        voltages = [id_voltage(data, byte, 3) for byte in (0x00, 0x3F, 0x40, 0x4F, 0x50, 0xBF, 0xC0, 0xFF)]
    except (OSError, ValueError, RuntimeError, UcError) as error:
        parser.exit(1, f'error: {error}\n')
    print(json.dumps({'kernelcache_sha256': evidence.KC_SHA256, 'hardware_access': False,
                      'power_callbacks': powers, 'id_voltage': voltages,
                      'limitations': ['fake ARMFunctions; real PMU/GPIO operations excluded',
                                      'surrounding authentication, routing and power state policy excluded',
                                      'VBUS boost/provider/protection and adapter pinout unverified']}, indent=2))


if __name__ == '__main__':
    main()
