#!/usr/bin/env python3
"""Bounded offline actual 12H321 zero-input-budget apply.

Executes 0x804c479c and the real D1946 input setter against fake PMU RAM.
Effective budget, previous current and all clocks are fixtures. This is not
proof of physical source removal or a charger activation implementation.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import struct
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,
                               UC_ARM_REG_R3, UC_ARM_REG_SP, UC_ARM_REG_LR,
                               UC_ARM_REG_PC)

spec = importlib.util.spec_from_file_location('events', Path(__file__).with_name('emulate-source-events.py'))
events = importlib.util.module_from_spec(spec)
spec.loader.exec_module(events)
d = events.d


def integer(value, low, high, name):
    if type(value) is not int or not low <= value <= high:
        raise ValueError(f'{name} must be an integer in {low}..{high}')
    return value


class ZeroInput(events.SourceEvents):
    OWNER_VTABLE = events.SourceEvents.OWNER + 0x1000
    NAME, ERROR_NAME, SOURCE50 = (d.TRAPS + offset for offset in (0x60, 0x64, 0x68))
    MOCKS = {0x804c9d34: 'current absolute time',
             0x804c9bf4: 'elapsed absolute-to-nanoseconds conversion',
             0x804c9c74: 'clock_get_uptime',
             0x804c9c84: 'interval conversion',
             0x804c9394: 'logging', NAME: 'owner name',
             ERROR_NAME: 'error name', SOURCE50: 'source virtual slot +0x50'}

    def __init__(self, data, failed_transfers=()):
        if not isinstance(failed_transfers, (tuple, list)):
            raise ValueError('failed_transfers must be a tuple or list')
        self.failed_transfers = frozenset(integer(v, 0, 7, 'transfer ordinal')
                                         for v in failed_transfers)
        super().__init__(data)
        self.calls = []
        self.w32(self.OWNER, self.OWNER_VTABLE)
        self.w32(self.OWNER_VTABLE + 0xf0, self.NAME | 1)
        self.w32(self.OWNER_VTABLE + 0x24c, self.ERROR_NAME | 1)
        # Only this fake source object's slot is replaced. It is observed,
        # never assumed to have an analog/hardware implementation.
        self.w32(0x80ed3c68 + 0x50, self.SOURCE50 | 1)
        self.mu.mem_write(self.NAME, b'\x70\x47' * 8)
        for address in self.MOCKS:
            self.mu.hook_add(UC_HOOK_CODE, self.mock, begin=address, end=address)
        for address in (0x80ed086c, 0x804c4814, 0x804c4a8c):
            self.mu.hook_add(UC_HOOK_CODE, self.observe, begin=address, end=address)

    def io(self, mu, address, size, user):
        self.fail_transfer = len(self.transactions) if len(self.transactions) in self.failed_transfers else None
        super().io(mu, address, size, user)

    def observe(self, mu, address, size, user):
        if address == 0x80ed086c:
            if mu.reg_read(UC_ARM_REG_R0) != d.SOURCE:
                raise RuntimeError('unexpected actual input setter owner')
            self.calls.append({'actual': 'D1946 input setter',
                               'requested_ma': mu.reg_read(UC_ARM_REG_R1)})
        else:
            self.calls.append({'actual': 'apply setter return', 'site': hex(address),
                               'return_code': mu.reg_read(UC_ARM_REG_R0)})

    def mock(self, mu, address, size, user):
        args = [mu.reg_read(r) for r in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)]
        entry = {'mocked': self.MOCKS[address]}
        if address == 0x804c9d34:
            mu.reg_write(UC_ARM_REG_R1, 0)
        elif address == 0x804c9bf4:
            if args[:2] != [0, 0]:
                raise RuntimeError('unexpected elapsed clock input')
            mu.mem_write(args[2], struct.pack('<Q', 0))
        elif address == 0x804c9c74:
            mu.mem_write(args[0], struct.pack('<Q', 100))
        elif address == 0x804c9c84:
            if args[:2] != [500, 1000000]:
                raise RuntimeError('unexpected interval request')
            mu.mem_write(args[2], struct.pack('<Q', 500))
            entry.update(interval=args[0], scale=args[1], fixture_ticks=500)
        elif address == self.SOURCE50:
            if args[:2] != [d.SOURCE, 0]:
                raise RuntimeError('unexpected source slot +0x50 call')
            entry['argument'] = args[1]
        self.calls.append(entry)
        mu.reg_write(UC_ARM_REG_R0, 0)
        mu.reg_write(UC_ARM_REG_PC, mu.reg_read(UC_ARM_REG_LR))

    def apply(self, previous_ma, effective_ma=0):
        integer(previous_ma, 0, 3262, 'previous current fixture')
        integer(effective_ma, 0, 0, 'effective budget fixture')
        if hasattr(self, '_used'):
            raise RuntimeError('use a fresh experiment for each fixture')
        self._used = True
        self.w32(self.REFERENCE + 0xc, effective_ma)
        self.w32(self.REFERENCE + 0x10, previous_ma)
        # Remaining source reference fields, log flag and owner state zero.
        for reg, value in ((UC_ARM_REG_SP, d.STACK + 0xff00), (UC_ARM_REG_LR, d.END | 1),
                           (UC_ARM_REG_R0, self.OWNER), (UC_ARM_REG_R1, d.SOURCE),
                           (UC_ARM_REG_R2, d.SOURCE + 0xc000)):
            self.mu.reg_write(reg, value)
        self.mu.emu_start(0x804c479d, d.END, count=20000)
        if self.mu.reg_read(UC_ARM_REG_PC) != d.END:
            raise RuntimeError('apply instruction budget exhausted')
        raw = bytes(self.mu.mem_read(self.REFERENCE + 0x10, 0x1c))
        return {'previous_ma_fixture': previous_ma, 'effective_ma_fixture': effective_ma,
                'failed_transfer_ordinals': sorted(self.failed_transfers),
                'stored_current_ma': struct.unpack_from('<I', raw)[0],
                'stored_delta_ma': struct.unpack_from('<i', raw, 4)[0],
                'source_cached_input_code': self.mu.mem_read(d.SOURCE + 0x2a, 1)[0],
                'stored_now_ticks': struct.unpack_from('<Q', raw, 0xc)[0],
                'stored_deadline_ticks': struct.unpack_from('<Q', raw, 0x14)[0],
                'final_fake_registers': {hex(r): self.registers[r] for r in (0x1b, 0x1c, 0x3d)},
                'calls': self.calls, 'transactions': self.transactions,
                'hardware_access': False}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('kernelcache', type=Path)
    args = p.parse_args()
    try:
        data = events.load_input(args.kernelcache)
        runs = [ZeroInput(data, failed).apply(old)
                for old, failed in ((0, ()), (500, ()), (500, (3,)), (500, (7,)), (500, (3, 7)))]
        print(json.dumps({'format': 'p105-zero-input-v1', 'hardware_access': False,
                          'kernelcache_sha256': d.EXPECTED_SHA256,
                          'limitations': ['source classification, physical detach and workloop not executed',
                                          'clocks and source slot +0x50 mocked',
                                          'PMU byte RAM has no analog or read-to-clear behavior',
                                          'charge-current setter and voltage/termination protection not covered'],
                          'experiments': runs}, indent=2))
    except (ValueError, RuntimeError) as error:
        p.error(str(error))


if __name__ == '__main__':
    main()
