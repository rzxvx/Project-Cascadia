#!/usr/bin/env python3
"""Actual bounded startup floor/cap slices, fake provider/OSData/boot arguments.

Runs selected original basic blocks, not full startup, provider lifetimes,
PMU initialization or a real boot-argument read. All inputs are synthetic.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import struct
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
                               UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R8, UC_ARM_REG_SP,
                               UC_ARM_REG_PC, UC_ARM_REG_LR)
ROOT = Path(os.environ.get('CASCADIA_RESEARCH_REPO', Path(__file__).resolve().parents[2]))
spec = importlib.util.spec_from_file_location('events', ROOT / 'tools/power/emulate-source-events.py')
events = importlib.util.module_from_spec(spec)
spec.loader.exec_module(events)
d = events.d


class ClampOwner(events.SourceEvents):
    PROVIDER, PROVIDER_VT, DATA, DATA_VT, BYTES = (d.SOURCE + n for n in (0xa000, 0xa100, 0xb000, 0xb100, 0xc000))
    GET, LENGTH, POINTER = (d.TRAPS + n for n in (0x80, 0x84, 0x88))
    def __init__(self, data):
        super().__init__(data)
        self.calls = []
        self.mu.mem_map(0x80398000, 0x1000)
        for va, _, offset, size in d.segments(data):
            start, end = max(va, 0x80398000), min(va + size, 0x80399000)
            if start < end: self.mu.mem_write(start, data[offset + start - va:offset + end - va])
        self.data_metaclass_fixture = struct.unpack('<I', self.mu.mem_read(0x80398450, 4))[0]
        self.w32(self.OWNER + 0xa8, self.PROVIDER)
        self.w32(self.PROVIDER, self.PROVIDER_VT)
        self.w32(self.PROVIDER_VT + 0x88, self.GET | 1)
        self.w32(self.DATA, self.DATA_VT)
        self.w32(self.DATA_VT + 0x4c, self.LENGTH | 1)
        self.w32(self.DATA_VT + 0x68, self.POINTER | 1)
        self.mu.mem_write(self.GET, b'\x70\x47' * 6)
        for a in (self.GET, self.LENGTH, self.POINTER, 0x804c9494, 0x804c9884):
            self.mu.hook_add(UC_HOOK_CODE, self.mock, begin=a, end=a)
        for a in (0x804c628c, 0x804c62d4, 0x804c634a):
            self.mu.hook_add(UC_HOOK_CODE, self.stop, begin=a, end=a)

    def stop(self, mu, address, size, user): mu.reg_write(UC_ARM_REG_PC, d.END)

    def mock(self, mu, address, size, user):
        args = [mu.reg_read(r) for r in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)]
        result = 0
        if address == 0x804c9494:
            if args[0] != 0x804cf5c4 or args[2] != 4: raise RuntimeError('unexpected boot argument')
            result = int(self.boot is not None)
            if result: self.w32(args[1], self.boot)
            self.calls.append({'mocked': 'PE_parse_boot_argn', 'present': bool(result)})
        elif address == self.GET:
            key = {0x804cf5c4: 'force-usb-power', 0x804cf5f7: 'usb-input-limit-max'}.get(args[1])
            if args[0] != self.PROVIDER or key is None: raise RuntimeError('unexpected provider/key')
            result = self.DATA if self.kind != 'absent' else 0
            self.calls.append({'mocked': 'provider property', 'key': key})
        elif address == 0x804c9884:
            if args[1] != self.data_metaclass_fixture: raise RuntimeError('unexpected OSData metaclass context')
            if args[0] not in (0, self.DATA): raise RuntimeError('unexpected OSData cast object')
            result = self.DATA if args[0] and self.kind == 'data' else 0
            self.calls.append({'mocked': 'OSData cast', 'accepted': bool(result)})
        elif address in (self.LENGTH, self.POINTER):
            if args[0] != self.DATA: raise RuntimeError('unexpected OSData owner')
            result = self.length if address == self.LENGTH else self.BYTES
            self.calls.append({'mocked': 'OSData length' if address == self.LENGTH else 'OSData bytes'})
        mu.reg_write(UC_ARM_REG_R0, result)
        mu.reg_write(UC_ARM_REG_PC, mu.reg_read(UC_ARM_REG_LR))

    def run_slice(self, address, data_metaclass=0):
        for reg, value in ((UC_ARM_REG_R4, self.OWNER), (UC_ARM_REG_R5, data_metaclass),
                           (UC_ARM_REG_R8, self.PROVIDER), (UC_ARM_REG_SP, d.STACK + 0xfe00),
                           (UC_ARM_REG_LR, d.END | 1)):
            self.mu.reg_write(reg, value)
        self.mu.emu_start(address | 1, d.END, count=20000)
        if self.mu.reg_read(UC_ARM_REG_PC) != d.END: raise RuntimeError('slice budget exhausted')

    def experiment(self, target='floor', boot=None, kind='absent', value=0, length=4):
        if target not in ('floor', 'cap') or kind not in ('absent', 'data', 'wrong_type'): raise ValueError('invalid target/type')
        for name, v in (('boot', boot), ('value', value), ('length', length)):
            if name == 'boot' and v is None: continue
            if type(v) is not int or not 0 <= v <= 0xffffffff: raise ValueError(f'{name} must be uint32')
        if target == 'cap' and boot is not None: raise ValueError('cap slice has no boot-argument consumer')
        if hasattr(self, '_used'): raise RuntimeError('fresh experiment required')
        self._used = True
        self.boot, self.kind, self.length = boot, kind, length
        self.w32(self.BYTES, value)  # four backed bytes even with malformed reported length
        if target == 'floor': self.run_slice(0x804c622e)
        else:
            self.run_slice(0x804c62cc)  # actual default1000 store, stop at62d4
            self.run_slice(0x804c6326, self.data_metaclass_fixture)  # actual OSData import context
        address = self.OWNER + (0x138 if target == 'floor' else 0x14c)
        if self.transactions: raise RuntimeError('unexpected PMU I/O')
        return {'target': target, 'boot_fixture': boot, 'property_kind': kind,
                'property_value_fixture': value, 'reported_length_fixture': length,
                'stored_value': struct.unpack('<I', self.mu.mem_read(address, 4))[0],
                'calls': self.calls, 'transactions': self.transactions, 'hardware_access': False,
                'limitations': ['isolated startup blocks and synthetic context',
                                'provider lifetime and full startup not executed',
                                'malformed-length fixture still has four backed bytes']}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kernelcache', type=Path)
    args = parser.parse_args()
    try:
        data = events.load_input(args.kernelcache)
        rows = [ClampOwner(data).experiment(boot=n) for n in (None, 0, 1, 100)]
        rows += [ClampOwner(data).experiment(target='cap', kind='data', value=2100)]
        print(json.dumps({'format': 'p105-input-clamp-owner-v1', 'kernelcache_sha256': d.EXPECTED_SHA256,
                          'hardware_access': False, 'experiments': rows}, indent=2))
    except (ValueError, RuntimeError) as error: parser.error(str(error))


if __name__ == '__main__': main()
