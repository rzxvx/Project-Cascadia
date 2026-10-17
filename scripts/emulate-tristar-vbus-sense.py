#!/usr/bin/env python3
"""Run actual 12H321 VBUS presence code with synthetic I2C/voltage replies.

This is a measurement experiment, not a power-source or boost implementation.
There is no USB/I2C/MMIO/device backend and no register-write provider. The
firmware hash, actual constructors and measurement dispatch are checked first.
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
OBJECT, THS, PROVIDER, PROVIDER_VTABLE = 0x10000000, 0x10001000, 0x10002000, 0x10003000
VOLTAGE, VOLTAGE_VTABLE = 0x10004000, 0x10005000
STACK, READ_TRAP, VOLTAGE_TRAP, END = 0x20000000, 0x30000000, 0x30000004, 0x30000FFC


def integer(value, maximum, label):
    if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= maximum:
        raise ValueError(f'{label} must be an integer between 0 and {maximum}')


def experiment(data, status, read_error=0, voltage=None, voltage_error=0):
    if hashlib.sha256(data).hexdigest() != evidence.KC_SHA256:
        raise ValueError('kernelcache SHA256 mismatch; fixed addresses refused')
    integer(status, 0xFF, 'synthetic THS status')
    integer(read_error, 0xFFFFFFFF, 'synthetic I2C result')
    integer(voltage_error, 0xFFFFFFFF, 'synthetic voltage result')
    if voltage is not None:
        integer(voltage, 0xFFFFFFFF, 'synthetic raw voltage reply')
    builtin = evidence.pic_literal_pointer(data, 0x80885368, 0x8088536A, 0) + 8
    ths = evidence.pic_literal_pointer(data, 0x808869FA, 0x808869FC, 0) + 8
    if builtin != 0x80888998 or ths != 0x80889298:
        raise ValueError('audited BuiltIn/THS instance vtable changed')
    for vtable, slot, target in ((builtin, 0x3C4, 0x808859C1), (ths, 0x38C, 0x80886CE5)):
        if struct.unpack_from('<I', data, evidence.address_offset(data, vtable + slot))[0] != target:
            raise ValueError('audited VBUS measurement dispatch changed')
    base_global = evidence.pic_literal_pointer(data, 0x808859E2, 0x808859E4, 0)
    if base_global != 0x808880D8:
        raise ValueError('audited fallback global changed')
    base = struct.unpack_from('<I', data, evidence.address_offset(data, base_global))[0]
    if base != 0x80888140 or struct.unpack_from('<I', data, evidence.address_offset(data, base + 0x3CC))[0] != 0x80884EA9:
        raise ValueError('audited fallback dispatch changed')
    machine = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
    start, end = 0x80884000, 0x8088D000
    first, last = evidence.address_offset(data, start), evidence.address_offset(data, end - 1)
    if last - first != end - start - 1:
        raise ValueError('firmware mapping not contiguous')
    machine.mem_map(start, end - start)
    machine.mem_write(start, data[first:last + 1])
    for address, size in ((OBJECT, 0x10000), (STACK, 0x10000), (READ_TRAP, 0x1000)):
        machine.mem_map(address, size)
    for address, value in ((OBJECT, builtin), (OBJECT + 0xF4, THS),
                           (OBJECT + 0x130, VOLTAGE if voltage is not None else 0),
                           (THS, ths), (THS + 0x6C, PROVIDER),
                           (PROVIDER, PROVIDER_VTABLE),
                           (PROVIDER_VTABLE + 0x3D4, READ_TRAP | 1),
                           (VOLTAGE, VOLTAGE_VTABLE),
                           (VOLTAGE_VTABLE + 0x3C, VOLTAGE_TRAP | 1)):
        machine.mem_write(address, struct.pack('<I', value))
    reads, voltage_calls, entries = [], [], []

    def code(mu, address, size, _):
        if address in (0x80884EA8, 0x80886CE4):
            entries.append({'address': hex(address), 'argument': mu.reg_read(UC_ARM_REG_R1)})
            return
        owner, first, second, last = [mu.reg_read(r) for r in
                                     (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)]
        if address == READ_TRAP:
            if owner != PROVIDER or first != 0x0C or last != 1 or not STACK <= second < STACK + 0x10000:
                raise RuntimeError('unexpected synthetic read request')
            mu.mem_write(second, bytes([status]))
            reads.append({'register': first, 'length': last, 'synthetic_status': status,
                          'synthetic_result': read_error})
            mu.reg_write(UC_ARM_REG_R0, read_error)
        elif address == VOLTAGE_TRAP:
            if owner != VOLTAGE or second != 0 or last != 0 or not STACK <= first <= STACK + 0x10000 - 4:
                raise RuntimeError('unexpected synthetic voltage function arguments')
            mu.mem_write(first, struct.pack('<I', voltage))
            voltage_calls.append({'synthetic_raw_reply': voltage, 'synthetic_result': voltage_error})
            mu.reg_write(UC_ARM_REG_R0, voltage_error)
        else:
            return
        mu.reg_write(UC_ARM_REG_PC, mu.reg_read(UC_ARM_REG_LR))

    machine.hook_add(UC_HOOK_CODE, code)
    for register, value in ((UC_ARM_REG_R0, OBJECT), (UC_ARM_REG_SP, STACK + 0xFF00),
                           (UC_ARM_REG_LR, END | 1)):
        machine.reg_write(register, value)
    machine.emu_start(0x808859C1, END, count=10000)
    if machine.reg_read(UC_ARM_REG_PC) != END:
        raise RuntimeError('firmware did not return within instruction budget')
    return {'synthetic_status': status, 'presence_result': machine.reg_read(UC_ARM_REG_R0),
            'synthetic_i2c_reads': reads, 'synthetic_voltage_calls': voltage_calls,
            'actual_fallback_entries': entries, 'hardware_access': False,
            'power_source_verified': False,
            'limitations': ['status bits have no assigned physical net names',
                            'raw threshold is not a proven calibrated voltage unit',
                            'no write provider, power-source control or board wiring modeled']}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kernelcache', type=Path)
    args = parser.parse_args()
    try:
        data = args.kernelcache.read_bytes()
        results = [experiment(data, status) for status in (0, 1, 2, 3, 0x80, 0x81, 0x82, 0x83)]
    except (OSError, ValueError, RuntimeError, UcError) as error:
        parser.exit(1, f'error: {error}\n')
    print(json.dumps({'kernelcache_sha256': evidence.KC_SHA256, 'hardware_access': False,
                      'experiments': results}, indent=2))


if __name__ == '__main__':
    main()
