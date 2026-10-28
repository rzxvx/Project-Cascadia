#!/usr/bin/env python3
"""Offline 12H321 property owner and voltage cutoff; downstream writes blocked."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import struct
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC
ROOT = Path(os.environ.get('CASCADIA_RESEARCH_REPO', Path(__file__).resolve().parents[2]))
spec = importlib.util.spec_from_file_location('events', ROOT / 'tools/power/emulate-source-events.py')
events = importlib.util.module_from_spec(spec)
spec.loader.exec_module(events)
d = events.d


class VoltageOwner(events.SourceEvents):
    DICT, DICT_VT, NUMBER, NUMBER_VT, OWNER_VT = (d.SOURCE + n for n in (0xa000, 0xa100, 0xb000, 0xb100, 0x9000))
    GET, NUMBER_READ, FORWARD, NAME = (d.TRAPS + n for n in (0x60, 0x64, 0x68, 0x6c))
    def __init__(self, data):
        super().__init__(data)
        self.mu.mem_map(0x80398000, 0x1000)
        for va, _, fileoff, filesize in d.segments(data):
            start, end = max(va, 0x80398000), min(va + filesize, 0x80399000)
            if start < end: self.mu.mem_write(start, data[fileoff + start - va:fileoff + end - va])
        self.calls = []
        self.number_metaclass_fixture = self.u32(0x803988d0)
        self.w32(self.OWNER, self.OWNER_VT)
        self.w32(self.OWNER_VT + 0xf0, self.NAME | 1)
        self.w32(self.DICT, self.DICT_VT)
        self.w32(self.DICT_VT + 0x98, self.GET | 1)
        self.w32(self.NUMBER, self.NUMBER_VT)
        self.w32(self.NUMBER_VT + 0x50, self.NUMBER_READ | 1)
        self.w32(0x80ed3c68 + 0x9c, self.FORWARD | 1)
        self.mu.mem_write(self.GET, b'\x70\x47' * 12)
        for a in (self.GET, self.NUMBER_READ, self.FORWARD, self.NAME, 0x804c9884, 0x804c9394):
            self.mu.hook_add(UC_HOOK_CODE, self.mock, begin=a, end=a)

    def mock(self, mu, address, size, user):
        args = [mu.reg_read(r) for r in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)]
        result = 0
        if address == self.GET:
            if args[0] != self.DICT or args[1] not in (0x804cee48, 0x804cef47):
                raise RuntimeError('unexpected property lookup owner/key')
            key = 'ChargeLimitVoltage' if args[1] == 0x804cef47 else 'ChargeLimitDisplayOn'
            self.calls.append({'mocked': 'dictionary lookup', 'key': key})
            if key == 'ChargeLimitVoltage' and self.kind != 'absent': result = self.NUMBER
        elif address == 0x804c9884:
            if args[1] != self.number_metaclass_fixture: raise RuntimeError('unexpected OSNumber metaclass import')
            if args[0] not in (0, self.NUMBER): raise RuntimeError('unexpected cast object')
            result = self.NUMBER if args[0] and self.kind == 'number' else 0
            self.calls.append({'mocked': 'OSNumber metacast', 'accepted': bool(result)})
        elif address == self.NUMBER_READ:
            if args[0] != self.NUMBER: raise RuntimeError('unexpected numeric object')
            result = self.value
            self.calls.append({'mocked': 'OSNumber value', 'firmware_units': result})
        elif address == self.FORWARD:
            if args[:2] != [d.SOURCE, self.DICT]: raise RuntimeError('unexpected backend owner/dictionary')
            result = self.forward_error
            self.calls.append({'mocked': 'backend slot +0x9c', 'return_code': result,
                               'owner_limit_at_call': self.u32(self.OWNER + 0x24c)})
        elif address == self.NAME:
            if args[0] != self.OWNER: raise RuntimeError('unexpected name owner')
            self.calls.append({'mocked': 'owner name'})
        else: self.calls.append({'mocked': 'logging'})
        mu.reg_write(UC_ARM_REG_R0, result)
        mu.reg_write(UC_ARM_REG_PC, mu.reg_read(UC_ARM_REG_LR))

    def u32(self, address): return struct.unpack('<I', self.mu.mem_read(address, 4))[0]

    def call_owner(self, address, r1=0, r2=0):
        for reg, value in ((UC_ARM_REG_R0, self.OWNER), (UC_ARM_REG_R1, r1), (UC_ARM_REG_R2, r2),
                           (UC_ARM_REG_SP, d.STACK + 0xff00), (UC_ARM_REG_LR, d.END | 1)):
            self.mu.reg_write(reg, value)
        self.mu.emu_start(address | 1, d.END, count=20000)
        if self.mu.reg_read(UC_ARM_REG_PC) != d.END: raise RuntimeError('instruction budget exhausted')
        return self.mu.reg_read(UC_ARM_REG_R0)

    def experiment(self, kind='number', value=4000, prior_limit=0, sample=4000, forward_error=0):
        if kind not in ('number', 'absent', 'wrong_type'): raise ValueError('invalid property kind')
        for name, v in (('value', value), ('prior_limit', prior_limit), ('sample', sample), ('forward_error', forward_error)):
            if type(v) is not int or not 0 <= v <= 0xffffffff: raise ValueError(f'{name} must be uint32')
        if hasattr(self, '_used'): raise RuntimeError('fresh experiment required')
        self._used = True
        self.kind, self.value, self.forward_error = kind, value, forward_error
        self.w32(self.OWNER + 0x24c, prior_limit)
        self.w32(self.OWNER + 0x22c, sample)
        # No table: the actual current selector uses its fallback -1 unless
        # voltage cutoff selects zero. These are numeric sentinels, not grants.
        result = self.call_owner(0x804c3224, d.SOURCE, self.DICT)
        ceiling = self.call_owner(0x804c3ef4)
        if self.transactions: raise RuntimeError('unexpected PMU I/O')
        return {'property_kind': kind, 'property_value_fixture': value, 'prior_limit_fixture': prior_limit,
                'sample_fixture': sample, 'units': 'firmware numeric units; physical units unverified',
                'stored_limit': self.u32(self.OWNER + 0x24c), 'callback_return': result,
                'current_ceiling_raw': ceiling, 'calls': self.calls,
                'transactions': self.transactions, 'hardware_access': False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kernelcache', type=Path)
    args = parser.parse_args()
    try:
        data = events.load_input(args.kernelcache)
        rows = [VoltageOwner(data).experiment(kind=kind, prior_limit=4000) for kind in ('number', 'absent', 'wrong_type')]
        rows.append(VoltageOwner(data).experiment(value=4000, prior_limit=4200, forward_error=0xe00002c0))
        print(json.dumps({'format': 'p105-voltage-owner-v1', 'kernelcache_sha256': d.EXPECTED_SHA256,
                          'hardware_access': False,
                          'limitations': ['OSDictionary, OSNumber access/cast and logging mocked',
                                          'backend slot +0x9c blocked; no PMU I/O',
                                          'sample and limit numeric units lack physical calibration',
                                          'no-table fallback -1 is a sentinel, not a charging grant'],
                          'experiments': rows}, indent=2))
    except (ValueError, RuntimeError) as error: parser.error(str(error))


if __name__ == '__main__': main()
