#!/usr/bin/env python3
"""Run actual 12H321 GPIOFunction initialization/output in synthetic RAM.

Only the audited output-mode flag shapes are exposed. No GPIO/MMIO/device
backend exists. Software line 132 is not a proven connector pin or VBUS boost
switch. Default mode mocks the GPIO provider; optional provider modeling runs
its real register methods against fake shadow/MMIO with IRQ/locks intercepted.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct

from unicorn import Uc, UcError, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_CODE, UC_HOOK_MEM_WRITE
from unicorn.arm_const import (UC_ARM_REG_C1_C0_2, UC_ARM_REG_FPEXC,
                               UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,
                               UC_ARM_REG_R3, UC_ARM_REG_PC, UC_ARM_REG_SP, UC_ARM_REG_LR)

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('usb_phy_evidence', ROOT / 'scripts/usb-phy-plan.py')
evidence = importlib.util.module_from_spec(spec)
spec.loader.exec_module(evidence)
OBJECT, GPIO, DATA, DATA_VTABLE = 0x10000000, 0x10001000, 0x10002000, 0x10003000
ARGUMENTS, GPIO_VTABLE, STACK, TRAP, END = 0x10004000, 0x10005000, 0x20000000, 0x30000000, 0x30000FFC
SHADOW, REGISTERS, LOCK = 0x10006000, 0x10007000, 0x10009000


def experiment(data, pinport, flags, requested, model_provider=False, initial_shadow=0x290):
    if hashlib.sha256(data).hexdigest() != evidence.KC_SHA256:
        raise ValueError('kernelcache SHA256 mismatch; fixed addresses refused')
    if isinstance(pinport, bool) or not isinstance(pinport, int) or not 0 <= pinport <= 0xFFFF:
        raise ValueError('synthetic pinport must be a 16-bit integer')
    if isinstance(flags, bool) or not isinstance(flags, int) or flags not in (0x101, 0x1):
        raise ValueError('only audited output-mode flags 0x101 and 0x1 supported')
    if isinstance(requested, bool) or not isinstance(requested, int) or requested not in (0, 1):
        raise ValueError('synthetic output request must be 0 or 1')
    if type(model_provider) is not bool:
        raise ValueError('provider-model flag must be boolean')
    if isinstance(initial_shadow, bool) or not isinstance(initial_shadow, int) or not 0 <= initial_shadow <= 0xFFFF:
        raise ValueError('synthetic initial shadow must be a 16-bit integer')
    actual = evidence.pic_literal_pointer(data, 0x80DED9D0, 0x80DED9D2, 1) + 8
    if actual != 0x80DF1D28:
        raise ValueError('audited GPIOFunction instance vtable changed')
    method = struct.unpack_from('<I', data, evidence.address_offset(data, actual + 0x3C))[0]
    if method != 0x80DEDAA9:
        raise ValueError('audited GPIOFunction output dispatch changed')
    global_pointer = evidence.pic_literal_pointer(data, 0x80DEDB64, 0x80DEDB68, 0)
    base_vtable = struct.unpack_from('<I', data, evidence.address_offset(data, global_pointer))[0]
    validation = struct.unpack_from('<I', data, evidence.address_offset(data, base_vtable + 0x40))[0] & ~1
    if validation != 0x804AB490:
        raise ValueError('audited base initializer dispatch changed')
    provider_vtable = evidence.pic_literal_pointer(data, 0x80DED0D8, 0x80DED0DA, 1) + 8
    if model_provider:
        if provider_vtable != 0x80DF1938:
            raise ValueError('audited GPIOIC instance vtable changed')
        for slot, target in ((0x378, 0x80DED989), (0x37C, 0x80DED991),
                             (0x380, 0x80DED869), (0x384, 0x80DED939)):
            if struct.unpack_from('<I', data, evidence.address_offset(data, provider_vtable + slot))[0] != target:
                raise ValueError('audited GPIOIC register dispatch changed')
    mu = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
    # Enable NEON only in the emulator: the real initializer copies the 16-byte
    # descriptor using VLD1/VST1. These are virtual CPU registers, not hardware.
    mu.reg_write(UC_ARM_REG_C1_C0_2, 0xF00000)
    mu.reg_write(UC_ARM_REG_FPEXC, 0x40000000)
    for start, end in ((0x804A9000, 0x804F0000), (0x80DEC000, 0x80DF6000)):
        first, last = evidence.address_offset(data, start), evidence.address_offset(data, end - 1)
        if last - first != end - start - 1:
            raise ValueError('firmware mapping not contiguous')
        mu.mem_map(start, end - start)
        mu.mem_write(start, data[first:last + 1])
    for address, size in ((OBJECT, 0x10000), (STACK, 0x10000), (TRAP, 0x1000)):
        mu.mem_map(address, size)

    def write32(address, value):
        mu.mem_write(address, struct.pack('<I', value))

    for address, value in ((OBJECT, actual), (OBJECT + 0xC, DATA), (OBJECT + 8, GPIO),
                           (DATA, DATA_VTABLE), (DATA_VTABLE + 0x68, TRAP | 1),
                           (GPIO, provider_vtable if model_provider else GPIO_VTABLE), (GPIO + 0x70, 28),
                           (GPIO_VTABLE + 0x384, (TRAP + 4) | 1)):
        write32(address, value)
    if model_provider:
        for address, value in ((GPIO + 0x80, SHADOW), (GPIO + 0x64, REGISTERS), (GPIO + 0x68, LOCK)):
            write32(address, value)
        for line in range(28 * 8):
            mu.mem_write(SHADOW + line * 2, struct.pack('<H', initial_shadow))
            write32(REGISTERS + line * 4, initial_shadow)
    # P105 dock descriptor shape from the ADT; flags=1 is an explicitly synthetic
    # polarity-comparison fixture, not another observed P105 rail descriptor.
    mu.mem_write(ARGUMENTS, struct.pack('<4I', 0x0F000000, 0x4750494F, pinport, flags))
    requests, register_writes, external_calls = [], [], []

    def code(machine, address, size, _):
        if model_provider and address == 0x80DED938:
            if machine.reg_read(UC_ARM_REG_R0) != GPIO:
                raise RuntimeError('unexpected actual GPIO provider dispatch')
            requests.append({'software_line': machine.reg_read(UC_ARM_REG_R1),
                             'level': machine.reg_read(UC_ARM_REG_R2)})
            return
        if address == validation:
            machine.reg_write(UC_ARM_REG_R0, 1)
        elif address == TRAP:
            machine.reg_write(UC_ARM_REG_R0, ARGUMENTS)
        elif address == 0x80DF06A4:
            machine.reg_write(UC_ARM_REG_R0, GPIO)
        elif address == TRAP + 4:
            if machine.reg_read(UC_ARM_REG_R0) != GPIO:
                raise RuntimeError('unexpected synthetic GPIO provider')
            requests.append({'software_line': machine.reg_read(UC_ARM_REG_R1),
                             'level': machine.reg_read(UC_ARM_REG_R2)})
            machine.reg_write(UC_ARM_REG_R0, 0)
        elif model_provider and address in (0x80DF0874, 0x80DF05A4, 0x80DF05B4):
            external_calls.append({'stub': hex(address), 'r0': hex(machine.reg_read(UC_ARM_REG_R0))})
            machine.reg_write(UC_ARM_REG_R0, 0)
        else:
            return
        machine.reg_write(UC_ARM_REG_PC, machine.reg_read(UC_ARM_REG_LR))

    def memory_write(machine, access, address, size, value, _):
        if REGISTERS <= address < REGISTERS + 28 * 8 * 4:
            if size != 4 or (address - REGISTERS) % 4:
                raise RuntimeError('unexpected synthetic GPIO register write width')
            register_writes.append({'offset': hex(address - REGISTERS), 'value': hex(value & 0xFFFFFFFF)})

    mu.hook_add(UC_HOOK_CODE, code)
    mu.hook_add(UC_HOOK_MEM_WRITE, memory_write)
    for register, value in ((UC_ARM_REG_R0, OBJECT), (UC_ARM_REG_SP, STACK + 0xFF00),
                           (UC_ARM_REG_LR, END | 1)):
        mu.reg_write(register, value)
    mu.emu_start(0x80DEDB61, END, count=10000)
    if mu.reg_read(UC_ARM_REG_PC) != END:
        raise RuntimeError('initializer did not return within instruction budget')
    initialized = mu.reg_read(UC_ARM_REG_R0)
    if initialized:
        write32(ARGUMENTS, requested)
        for register, value in ((UC_ARM_REG_R0, OBJECT), (UC_ARM_REG_R1, ARGUMENTS),
                               (UC_ARM_REG_R2, 0), (UC_ARM_REG_R3, 0),
                               (UC_ARM_REG_SP, STACK + 0xFF00), (UC_ARM_REG_LR, END | 1)):
            mu.reg_write(register, value)
        mu.emu_start(method, END, count=10000)
        if mu.reg_read(UC_ARM_REG_PC) != END:
            raise RuntimeError('callback did not return within instruction budget')
    return {'pinport': hex(pinport), 'flags': hex(flags), 'requested': requested,
            'initialized': bool(initialized),
            'retained_selector': struct.unpack('<I', mu.mem_read(OBJECT + 0x20, 4))[0],
            'synthetic_gpio_requests': requests, 'hardware_access': False,
            'provider_model': model_provider, 'initial_shadow': hex(initial_shadow),
            'synthetic_register_writes': register_writes, 'external_calls': external_calls,
            'vbus_source_or_voltage_verified': False,
            'limitations': ['base validation and OSData/cast mocked',
                            'GPIO provider mocked by default; opt-in uses fake shadow/MMIO and IRQ/locks',
                            'physical pin, wiring, provider errors and VBUS boost not modeled']}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kernelcache', type=Path)
    parser.add_argument('--model-provider', action='store_true',
                        help='execute actual GPIOIC methods against synthetic shadow/MMIO only')
    args = parser.parse_args()
    try:
        data = args.kernelcache.read_bytes()
        results = [experiment(data, pin, flags, request, model_provider=args.model_provider) for pin, flags, request in
                   ((0x1004, 0x101, 0), (0x1004, 0x101, 1), (0x1004, 1, 0),
                    (0x1004, 1, 1), (0x1008, 0x101, 1), (0x1C00, 0x101, 1))]
    except (OSError, ValueError, RuntimeError, UcError) as error:
        parser.exit(1, f'error: {error}\n')
    print(json.dumps({'kernelcache_sha256': evidence.KC_SHA256,
                      'hardware_access': False, 'experiments': results}, indent=2))


if __name__ == '__main__':
    main()
