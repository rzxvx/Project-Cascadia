#!/usr/bin/env python3
"""Offline actual iBEC PMU byte write/readback helpers; fake I2C only."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC
ROOT = Path(__file__).resolve().parents[2]
DEFAULT_IBEC = ROOT / 'build/firmware/iBEC.dec'
SHA256 = '30c040052c444222edc8646316f95a355a9e338d6aadc7168f8a7acc7c4578ec'
BASE, STACK, TRAPS, END = 0x9ff00000, 0x20000000, 0x30000000, 0x30000ffc


def load_input(path):
    data = Path(path).read_bytes()
    if hashlib.sha256(data).hexdigest() != SHA256: raise ValueError('iBEC hash mismatch')
    return data


class WriteExperiment:
    def __init__(self, data):
        if hashlib.sha256(data).hexdigest() != SHA256: raise ValueError('iBEC hash mismatch')
        self.mu = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
        self.mu.mem_map(BASE, 0x70000); self.mu.mem_write(BASE, data)
        self.mu.mem_map(STACK, 0x10000); self.mu.mem_map(TRAPS, 0x1000)
        self.registers = bytearray(256); self.registers[0x3f] = 0xbf
        self.transactions = []
        for address in (BASE + 0xbc08, BASE + 0xbae0):
            self.mu.hook_add(UC_HOOK_CODE, self.io, begin=address, end=address)

    @staticmethod
    def require_stack(pointer, size):
        if not STACK <= pointer <= STACK + 0x10000 - size:
            raise RuntimeError('unexpected fake stack pointer')

    def io(self, mu, address, size, user):
        args = [mu.reg_read(r) for r in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)]
        if args[0] != 0: raise RuntimeError('unexpected I2C port')
        if address == BASE + 0xbc08:
            if args[1] != 0x78 or args[3] != 2: raise RuntimeError('unexpected PMU write')
            self.require_stack(args[2], 2)
            reg, value = bytes(mu.mem_read(args[2], 2))
            result = self.write_error
            if not result: self.registers[reg] = value
            self.transactions.append({'mocked': 'I2C write', 'address_byte': args[1],
                                      'register': reg, 'value': value, 'return_code': result})
        else:
            if args[1] != 0x79 or args[3] != 1: raise RuntimeError('unexpected PMU read')
            stack_pointer = mu.reg_read(UC_ARM_REG_SP)
            self.require_stack(stack_pointer, 12)
            output, count, flag = struct.unpack('<III', mu.mem_read(stack_pointer, 12))
            self.require_stack(args[2], 1)
            self.require_stack(output, 1)
            if count != 1 or flag != 1: raise RuntimeError('unexpected read context')
            reg = mu.mem_read(args[2], 1)[0]
            value = self.readback if self.readback is not None else self.registers[reg]
            result = self.read_error
            if not result: mu.mem_write(output, bytes((value,)))
            self.transactions.append({'mocked': 'I2C read', 'address_byte': args[1],
                                      'register': reg, 'value_if_success': value,
                                      'return_code': result, 'output_written': not bool(result)})
        mu.reg_write(UC_ARM_REG_R0, result)
        mu.reg_write(UC_ARM_REG_PC, mu.reg_read(UC_ARM_REG_LR))

    def call(self, helper, value=0x40, verify=True, write_error=0, read_error=0, readback=None):
        if helper not in ('charger', 'generic'): raise ValueError('unknown helper')
        if type(value) is not int or not 0 <= value <= 255: raise ValueError('value must be byte')
        if type(verify) is not bool: raise ValueError('verify must be boolean')
        for status in (write_error, read_error):
            if type(status) is not int or not 0 <= status <= 0xffffffff: raise ValueError('error must be uint32')
        if readback is not None and (type(readback) is not int or not 0 <= readback <= 255): raise ValueError('readback must be byte or None')
        if hasattr(self, '_used'): raise RuntimeError('fresh experiment required')
        self._used = True
        self.write_error, self.read_error, self.readback = write_error, read_error, readback
        args = (0x3f, value, int(verify), 0) if helper == 'charger' else (0, 0x3f, value, int(verify))
        for reg, v in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3), args): self.mu.reg_write(reg, v)
        self.mu.reg_write(UC_ARM_REG_SP, STACK + 0xff00); self.mu.reg_write(UC_ARM_REG_LR, END | 1)
        address = BASE + (0xc6fc if helper == 'charger' else 0xbe6c)
        self.mu.emu_start(address | 1, END, count=20000)
        if self.mu.reg_read(UC_ARM_REG_PC) != END: raise RuntimeError('instruction budget exhausted')
        return {'helper': helper, 'entry': hex(address), 'return_code': self.mu.reg_read(UC_ARM_REG_R0),
                'final_fake_register_3f': self.registers[0x3f], 'transactions': self.transactions,
                'hardware_access': False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('ibec', nargs='?', type=Path, default=DEFAULT_IBEC,
                        help='hash-pinned local iBEC (default: build/firmware/iBEC.dec relative to repository)')
    args = parser.parse_args()
    try:
        data = load_input(args.ibec)
        rows = [WriteExperiment(data).call(helper, **params) for helper in ('charger', 'generic')
                for params in ({}, {'readback': 0xbf}, {'write_error': 0xffffffff},
                               {'value': 0, 'read_error': 0xffffffff})]
        print(json.dumps({'format': 'p105-ibec-pmu-readback-v1', 'ibec_sha256': SHA256,
                          'hardware_access': False, 'limitations': ['I2C transactions and register bytes synthetic',
                                          'no analog effects or initialization safety proof'], 'experiments': rows}, indent=2))
    except (OSError, ValueError, RuntimeError) as error: parser.error(str(error))


if __name__ == '__main__': main()
