#!/usr/bin/env python3
"""Offline D1946 source-event callback and generic notification experiment.

Runs actual 12H321 callback, status reader and notification adapters. PMU I/O
uses fake RAM; the generic charger notification consumer is mocked. Neither
source-event names nor complete physical fault protection are assumed.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct

from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_LR, UC_ARM_REG_PC

spec = importlib.util.spec_from_file_location('d1946', Path(__file__).with_name('emulate-d1946.py'))
d = importlib.util.module_from_spec(spec)
spec.loader.exec_module(d)


def load_input(path):
    data = Path(path).read_bytes()
    if hashlib.sha256(data).hexdigest() != d.EXPECTED_SHA256:
        raise ValueError('kernelcache hash mismatch; refusing fixed addresses')
    return data


class SourceEvents(d.Experiment):
    REFERENCE, OWNER = d.SOURCE + 0x7000, d.SOURCE + 0x8000

    def __init__(self, data, owner_present=True, fail_transfer=None):
        super().__init__(data, fail_transfer=fail_transfer)
        lo, hi = 0x804a9000, 0x804f1000
        self.mu.mem_map(lo, hi - lo)
        for va, _, fileoff, filesize in d.segments(data):
            start, end = max(va, lo), min(va + filesize, hi)
            if start < end:
                self.mu.mem_write(start, data[fileoff + start - va:fileoff + end - va])
        self.w32(d.SOURCE + 0x14, self.REFERENCE)
        self.w32(self.REFERENCE, self.OWNER if owner_present else 0)
        self.notifications = []
        self.mu.hook_add(UC_HOOK_CODE, self.notification, begin=0x804c2cfc, end=0x804c2cfc)

    def notification(self, mu, address, size, user):
        if mu.reg_read(UC_ARM_REG_R0) != self.OWNER or mu.reg_read(UC_ARM_REG_R1) != d.SOURCE:
            raise RuntimeError('unexpected generic charger notification owner')
        self.notifications.append({'mocked': 'generic charger notification consumer',
                                   'mask': mu.reg_read(UC_ARM_REG_R2)})
        mu.reg_write(UC_ARM_REG_R0, 0)
        mu.reg_write(UC_ARM_REG_PC, mu.reg_read(UC_ARM_REG_LR))

    def set_input_fixture(self, current_ma):
        if not 0 <= current_ma <= 10000:
            raise ValueError('fixture input request outside bounded research range')
        self.call(0x80ed086c, current_ma, d.SOURCE + 0x3000)
        return self.mu.mem_read(d.SOURCE + 0x2a, 1)[0]

    def event(self, packed_event, status_bytes=bytes(6), live_input_code=2):
        if not 0 <= packed_event <= 0x8ff:
            raise ValueError('event must have a status-byte index 0..8 and an 8-bit mask')
        if len(status_bytes) != 6 or not 0 <= live_input_code <= 255:
            raise ValueError('invalid synthetic PMU status/input code')
        self.registers[0x0a:0x10] = status_bytes
        self.registers[0x3d] = live_input_code
        self.transactions = []
        self.notifications = []
        result = self.call(0x80ed06a4, packed_event)
        return {'packed_event': packed_event, 'return_code': result,
                'cached_input_code': self.mu.mem_read(d.SOURCE + 0x2a, 1)[0],
                'notifications': list(self.notifications), 'transactions': list(self.transactions),
                'hardware_access': False}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('kernelcache', type=Path)
    args = p.parse_args()
    try:
        data = load_input(args.kernelcache)
        out = []
        for request in (100, 200, 500, 1000):
            for event in (0x8, 0x10, 0x610):
                e = SourceEvents(data)
                e.set_input_fixture(request)
                out.append(e.event(event, bytes.fromhex('000100000000')))
        print(json.dumps({'format': 'p105-source-events-v1', 'hardware_access': False,
                          'kernelcache_sha256': d.EXPECTED_SHA256,
                          'limitations': ['event/status values synthetic', 'generic notification consumer mocked',
                                          'no analog meaning or fault-shutdown assertion'],
                          'events': out}, indent=2))
    except (ValueError, RuntimeError) as error:
        p.error(str(error))


if __name__ == '__main__':
    main()
