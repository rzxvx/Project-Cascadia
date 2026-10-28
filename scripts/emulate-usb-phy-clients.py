#!/usr/bin/env python3
"""Run pinned Apple USB PHY client arbitration with synthetic collections.

Actual 12H321 client-update/selection instructions execute in Unicorn. The
collection methods, lock imports and power/mode callbacks are fake providers.
No register, device, network, deploy or electrical power backend exists.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct

from unicorn import Uc, UcError, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_CODE
from unicorn.arm_const import (
    UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_PC,
    UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_C1_C0_2, UC_ARM_REG_FPEXC)

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('usb_phy_evidence', ROOT / 'scripts/usb-phy-plan.py')
EVIDENCE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(EVIDENCE)
OBJECT, VTABLE, COLLECTIONS = 0x10000000, 0x10002000, 0x10003000
STACK, TRAP, END = 0x20000000, 0x30000000, 0x30000FFC


def validate_requests(requests):
    if not isinstance(requests, (list, tuple)) or not 1 <= len(requests) <= 64:
        raise ValueError('requires 1..64 synthetic client requests')
    for request in requests:
        if not isinstance(request, (list, tuple)) or len(request) != 2:
            raise ValueError('each request is a synthetic client ID and five-bit mask')
        client, mask = request
        if isinstance(client, bool) or not isinstance(client, int) or not 1 <= client <= 65535:
            raise ValueError('synthetic client ID must be 1..65535')
        if isinstance(mask, bool) or not isinstance(mask, int) or not 0 <= mask <= 31:
            raise ValueError('only the audited five-bit client mask is supported')


def experiment(data, requests):
    if hashlib.sha256(data).hexdigest() != EVIDENCE.KC_SHA256:
        raise ValueError('kernelcache SHA256 mismatch; fixed addresses refused')
    validate_requests(requests)

    def firmware_u32(address):
        return struct.unpack_from('<I', data, EVIDENCE.address_offset(data, address))[0]

    if firmware_u32(EVIDENCE.VTABLE_VA + 0x350) != 0x80861C5D:
        raise ValueError('audited PHY client-update target changed')
    for slot, target in ((0x344, 0x80CABEA9), (0x348, 0x80CAC14D), (0x34C, 0x80CAC201)):
        if firmware_u32(EVIDENCE.VTABLE_VA + slot) != target:
            raise ValueError('audited PHY power/mode callback changed')

    machine = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
    start, end = 0x80861000, 0x80867000
    offset, last = (EVIDENCE.address_offset(data, address) for address in (start, end - 1))
    if last - offset != end - start - 1:
        raise ValueError('firmware mapping is not contiguous')
    for address, size in ((start, end - start), (OBJECT, 0x10000),
                          (STACK, 0x10000), (TRAP, 0x1000)):
        machine.mem_map(address, size)
    machine.mem_write(start, data[offset:last + 1])

    def write_u32(address, value):
        machine.mem_write(address, struct.pack('<I', value))

    # Real stack-canary load, pointing at a newly allocated fake RAM page.
    imported = EVIDENCE.pic_literal_pointer(data, 0x80861C68, 0x80861C6E, 0)
    guard = firmware_u32(imported)
    machine.mem_map(guard & ~0xFFF, 0x1000)
    write_u32(guard, 0xA1B2C3D4)
    write_u32(OBJECT, VTABLE)
    write_u32(OBJECT + 0x58, 0x11111111)
    collections = [[] for _ in range(5)]
    for index in range(5):
        address = COLLECTIONS + 0x100 * index
        write_u32(OBJECT + 0x64 + 4 * index, address)
        write_u32(address, VTABLE + 0x800)
    for slot, trap in ((0x344, TRAP), (0x348, TRAP + 4), (0x34C, TRAP + 8)):
        write_u32(VTABLE + slot, trap | 1)
    for slot, trap in ((0x44, TRAP + 12), (0x88, TRAP + 16),
                       (0x80, TRAP + 20), (0x70, TRAP + 24)):
        write_u32(VTABLE + 0x800 + slot, trap | 1)
    callbacks, lock_depth = [], 0

    def code(mu, address, size, _):
        nonlocal lock_depth
        owner, argument = (mu.reg_read(reg) for reg in (UC_ARM_REG_R0, UC_ARM_REG_R1))
        if address in (0x80864E40, 0x80864E50):
            if owner != 0x11111111:
                raise RuntimeError('unexpected synthetic lock object')
            lock_depth += 1 if address == 0x80864E40 else -1
            if lock_depth not in (0, 1):
                raise RuntimeError('unbalanced synthetic lock')
        elif address in (TRAP, TRAP + 4, TRAP + 8):
            if owner != OBJECT or lock_depth != 1:
                raise RuntimeError('unexpected synthetic PHY callback')
            kind = {TRAP: 'power', TRAP + 4: 'device', TRAP + 8: 'host'}[address]
            callbacks.append({'callback': kind, 'argument': argument})
        elif address in (TRAP + 12, TRAP + 16, TRAP + 20, TRAP + 24):
            index = (owner - COLLECTIONS) // 0x100
            if index not in range(5) or owner != COLLECTIONS + 0x100 * index:
                raise RuntimeError('unexpected synthetic collection object')
            if address == TRAP + 12:
                mu.reg_write(UC_ARM_REG_R0, len(collections[index]))
            elif address in (TRAP + 16, TRAP + 20):
                if argument >= len(collections[index]):
                    raise RuntimeError('synthetic collection index out of bounds')
                if address == TRAP + 16:
                    mu.reg_write(UC_ARM_REG_R0, collections[index][argument])
                else:
                    collections[index].pop(argument)
            else:
                collections[index].append(argument)
                mu.reg_write(UC_ARM_REG_R0, 1)
        else:
            return
        mu.reg_write(UC_ARM_REG_PC, mu.reg_read(UC_ARM_REG_LR))

    machine.hook_add(UC_HOOK_CODE, code)
    # The actual client-selector zeroes stack scratch with a NEON instruction.
    machine.reg_write(UC_ARM_REG_C1_C0_2, 0xF00000)
    machine.reg_write(UC_ARM_REG_FPEXC, 0x40000000)
    states = []
    for client, mask in requests:
        callbacks.clear()
        for register, value in ((UC_ARM_REG_R0, OBJECT), (UC_ARM_REG_R1, client),
                                (UC_ARM_REG_R2, mask), (UC_ARM_REG_SP, STACK + 0xFF00),
                                (UC_ARM_REG_LR, END | 1)):
            machine.reg_write(register, value)
        machine.emu_start(0x80861C5D, END, count=10000)
        if machine.reg_read(UC_ARM_REG_PC) != END or lock_depth:
            raise RuntimeError('firmware failed to return within the locked instruction budget')
        states.append({'synthetic_client_id': client, 'client_mask': hex(mask),
                       'collections': [list(items) for items in collections],
                       'callbacks': list(callbacks), 'return_code': hex(machine.reg_read(UC_ARM_REG_R0))})
    return {'states': states, 'hardware_access': False,
            'power_and_mode_callbacks_stubbed': True,
            'wlan_continuity_or_electrical_power_verified': False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kernelcache', type=Path)
    args = parser.parse_args()
    examples = [[(1, 4), (2, 4), (1, 0), (2, 0)],
                [(1, 4), (1, 8), (1, 0)],
                [(1, 16), (2, 4), (2, 0), (1, 0)]]
    try:
        data = args.kernelcache.read_bytes()
        results = [experiment(data, requests) for requests in examples]
    except (OSError, ValueError, RuntimeError, UcError) as error:
        parser.exit(1, f'error: {error}\n')
    print(json.dumps({'kernelcache_sha256': EVIDENCE.KC_SHA256,
                      'hardware_access': False, 'experiments': results,
                      'limitations': ['IOKit collection methods and locks simulated',
                                      'power and mode callbacks recorded, never executed',
                                      'no controller, GPIO, regulator or electrical model']}, indent=2))


if __name__ == '__main__':
    main()
