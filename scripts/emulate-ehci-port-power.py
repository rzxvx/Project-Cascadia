#!/usr/bin/env python3
"""Execute pinned 12H321 EHCI port power callbacks against synthetic RAM only.

No device, USB, I2C, network or MMIO backend exists. The modeled register is
ordinary Unicorn memory; PORT_POWER request does not establish a 5 V provider.
IOKit's asynchronous power-request machinery and electrical behavior are not
modeled. Only actual gated state dispatch and port register callbacks execute.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct

from unicorn import Uc, UcError, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_CODE, UC_HOOK_MEM_WRITE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('usb_phy_evidence', ROOT / 'scripts/usb-phy-plan.py')
evidence = importlib.util.module_from_spec(spec)
spec.loader.exec_module(evidence)
OBJECT, REGISTER, STACK, END = 0x10000000, 0x10001000, 0x20000000, 0x30000FFC
VTABLE, FUNCTION = 0x80C92608, 0x8082B4F8


def experiment(data, requested, current, portsc):
    if hashlib.sha256(data).hexdigest() != evidence.KC_SHA256:
        raise ValueError('kernelcache SHA256 mismatch; fixed addresses refused')
    # Only the direct on/off routes audited here are exposed; no generic
    # arbitrary firmware entrypoint, register selector or hardware address.
    if (requested, current) not in ((2, 0), (0, 2)):
        raise ValueError('only audited off-to-on and on-to-off transitions supported')
    if not isinstance(portsc, int) or isinstance(portsc, bool) or not 0 <= portsc <= 0xFFFFFFFF:
        raise ValueError('synthetic PORTSC must be a 32-bit integer')
    actual = evidence.pic_literal_pointer(data, 0x80C90AF4, 0x80C90AF6, 0) + 8
    expected = {0x374: FUNCTION | 1, 0x380: 0x80848759, 0x384: 0x808487F9,
                0x3D8: 0x808497A1, 0x3DC: 0x808497AD}
    if actual != VTABLE:
        raise ValueError('audited removable-port instance vtable changed')
    for slot, target in expected.items():
        if struct.unpack_from('<I', data, evidence.address_offset(data, actual + slot))[0] != target:
            raise ValueError('audited port power/register dispatch changed')
    for pointer, symbol in ((0x8084D01C, '_IOSleep'), (0x8084D024, '_OSSynchronizeIO')):
        imported = struct.unpack_from('<I', data, evidence.address_offset(data, pointer))[0] & ~1
        if imported != evidence.symbol_address(data, symbol) & ~1:
            raise ValueError('audited delay/write-barrier import changed')
    mu = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
    for start, end in ((0x80816000, 0x80852000), (0x80C8F000, 0x80C93000)):
        mu.mem_map(start, end - start)
        first, last = evidence.address_offset(data, start), evidence.address_offset(data, end - 1)
        if last - first != end - start - 1:
            raise ValueError('firmware mapping not contiguous')
        mu.mem_write(start, data[first:last + 1])
    for address, size in ((OBJECT, 0x2000), (STACK, 0x10000), (END & ~0xFFF, 0x1000)):
        mu.mem_map(address, size)

    def write32(address, value):
        mu.mem_write(address, struct.pack('<I', value))

    write32(OBJECT, actual)
    write32(OBJECT + 0x68, current)
    write32(OBJECT + 0xD8, REGISTER)
    write32(REGISTER, portsc)
    writes, external_calls = [], []

    def memory_write(machine, access, address, size, value, _):
        if REGISTER <= address < REGISTER + 4:
            if address != REGISTER or size != 4:
                raise RuntimeError('unexpected synthetic port register access width')
            writes.append(value & 0xFFFFFFFF)

    def code(machine, address, size, _):
        # External delay and write-barrier imports are excluded from actual
        # execution. No logging, register access or IOKit method is mocked.
        if address not in (0x8084AB7C, 0x8084AB9C):
            return
        external_calls.append({'stub': hex(address), 'r0': hex(machine.reg_read(UC_ARM_REG_R0))})
        machine.reg_write(UC_ARM_REG_R0, 0)
        machine.reg_write(UC_ARM_REG_PC, machine.reg_read(UC_ARM_REG_LR))

    mu.hook_add(UC_HOOK_MEM_WRITE, memory_write)
    mu.hook_add(UC_HOOK_CODE, code)
    mu.reg_write(UC_ARM_REG_R0, OBJECT)
    mu.reg_write(UC_ARM_REG_R1, requested)
    mu.reg_write(UC_ARM_REG_SP, STACK + 0xFF00)
    mu.reg_write(UC_ARM_REG_LR, END | 1)
    mu.emu_start(FUNCTION | 1, END, count=20000)
    if mu.reg_read(UC_ARM_REG_PC) != END:
        raise RuntimeError('firmware did not return within instruction budget')
    return {'requested': requested, 'current': current, 'initial_portsc': hex(portsc),
            'register_writes': [hex(x) for x in writes],
            'final_portsc': hex(struct.unpack('<I', mu.mem_read(REGISTER, 4))[0]),
            'return_code': hex(mu.reg_read(UC_ARM_REG_R0)), 'external_calls': external_calls,
            'instance_vtable': hex(actual), 'hardware_access': False,
            'vbussource_or_voltage_verified': False,
            'limitations': ['PORTSC is plain synthetic RAM, no hardware RW1C behavior',
                            'IOKit async transition/completion and PHY power not modeled',
                            'only delay and write-barrier imports intercepted']}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kernelcache', type=Path)
    args = parser.parse_args()
    try:
        data = args.kernelcache.read_bytes()
        results = [experiment(data, request, current, register) for request, current, register in
                   ((2, 0, 0x2B), (2, 0, 0x80500001), (0, 2, 0x30102F))]
    except (OSError, ValueError, RuntimeError, UcError) as error:
        parser.exit(1, f'error: {error}\n')
    print(json.dumps({'kernelcache_sha256': evidence.KC_SHA256,
                      'hardware_access': False, 'experiments': results}, indent=2))


if __name__ == '__main__':
    main()
