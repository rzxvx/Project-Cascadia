#!/usr/bin/env python3
"""Execute the 12H321 status consumer offline with restart callbacks blocked.

Runs the actual D1946 status getter and ARMPlatform consumer against fake
RAM. External-connected reporting, the monotonic-clock sysctl and both
backend recovery methods are mocked. No device or physical charger access.
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

spec = importlib.util.spec_from_file_location('source_events', Path(__file__).with_name('emulate-source-events.py'))
events = importlib.util.module_from_spec(spec)
spec.loader.exec_module(events)
d = events.d


class Status(events.SourceEvents):
    OUTPUT = d.SOURCE + 0xc000
    MOCKED = {0x804c9744: 'IOPMPowerSource externalConnected',
              0x804c9ed4: 'kernel_sysctlbyname kern.monotonicclock',
              0x80ed0c94: 'backend slot +0x74 recovery blocked',
              0x80ed0ce0: 'backend slot +0x78 low-current action blocked'}

    def __init__(self, data, fail_transfer=None):
        super().__init__(data, fail_transfer=fail_transfer)
        self.calls = []
        self.external = True
        self.now = 0
        self.clock_error = 0
        for address in self.MOCKED:
            self.mu.hook_add(UC_HOOK_CODE, self.helper, begin=address, end=address)

    def helper(self, mu, address, size, user):
        args = [mu.reg_read(r) for r in (UC_ARM_REG_R0, UC_ARM_REG_R1,
                                       UC_ARM_REG_R2, UC_ARM_REG_R3)]
        entry = {'mocked': self.MOCKED[address]}
        result = 0
        if address == 0x804c9744:
            if args[0] != self.OWNER:
                raise RuntimeError('unexpected externalConnected owner')
            result = int(self.external)
        elif address == 0x804c9ed4:
            if args[0] != 0x804cf3dc or args[3] != 0 or bytes(mu.mem_read(args[2], 4)) != b'\x04\0\0\0':
                raise RuntimeError('unexpected clock sysctl arguments')
            if struct.unpack('<I', mu.mem_read(mu.reg_read(UC_ARM_REG_SP), 4))[0] != 0:
                raise RuntimeError('clock sysctl attempted a new-value operation')
            self.w32(args[1], self.now)
            result = self.clock_error
        elif args[0] != d.SOURCE:
            raise RuntimeError('unexpected recovery source')
        self.calls.append(entry)
        mu.reg_write(UC_ARM_REG_R0, result)
        mu.reg_write(UC_ARM_REG_PC, mu.reg_read(UC_ARM_REG_LR))

    def consume(self, status_byte, now, current_capacity=80, maximum_capacity=100,
                first_timeout=0, attached_at=1, external=True, cached_flag=False,
                inhibit=False, clock_error=0, prior_reason=0):
        values = (now, first_timeout, attached_at, prior_reason)
        if not all(isinstance(v, int) and 0 <= v <= 0xffffffff for v in values):
            raise ValueError('time/reason fixtures must be unsigned 32-bit')
        if not isinstance(status_byte, int) or not 0 <= status_byte <= 255:
            raise ValueError('synthetic status byte must be 0..255')
        # Bounded capacity avoids accidental signed multiply overflow. These
        # numbers are fixtures in the firmware's own capacity units.
        if not (0 <= current_capacity <= 10000 and 1 <= maximum_capacity <= 10000):
            raise ValueError('capacity fixture outside bounded research range')
        if not 0 <= clock_error <= 0xffffffff:
            raise ValueError('sysctl error must be unsigned 32-bit')
        self.now, self.external, self.clock_error = now, bool(external), clock_error
        self.transactions, self.calls = [], []
        self.registers[0x0a:0x10] = bytes((0, status_byte, 0, 0, 0, 0))
        self.w32(self.REFERENCE + 0x2c, first_timeout)
        self.w32(self.OWNER + 0x1c4, prior_reason)
        self.w32(self.OWNER + 0x1c8, attached_at)
        self.w32(self.OWNER + 0x1e0, current_capacity)
        self.w32(self.OWNER + 0x1e4, maximum_capacity)
        for offset, value in ((0x1c0, cached_flag), (0x1cd, inhibit), (0x13c, False)):
            self.mu.mem_write(self.OWNER + offset, bytes((int(bool(value)),)))
        self.mu.mem_write(self.OUTPUT, b'\0')
        # Clear stack between independent trials. The original D1946 getter
        # ignores read failures; this prevents stale stack bytes from being
        # accidentally mistaken for safe hardware status in the experiment.
        self.mu.mem_write(d.STACK, bytes(0x10000))
        for reg, value in ((UC_ARM_REG_SP, d.STACK + 0xff00), (UC_ARM_REG_LR, d.END | 1),
                           (UC_ARM_REG_R0, self.OWNER), (UC_ARM_REG_R1, d.SOURCE),
                           (UC_ARM_REG_R2, self.OUTPUT)):
            self.mu.reg_write(reg, value)
        self.mu.emu_start(0x804c5691, d.END, count=20000)
        if self.mu.reg_read(UC_ARM_REG_PC) != d.END:
            raise RuntimeError('status consumer instruction budget exhausted')
        reason = struct.unpack('<I', self.mu.mem_read(self.OWNER + 0x1c4, 4))[0]
        if reason not in (0, prior_reason, 0x80ed2be6):
            raise RuntimeError('unexpected synthetic reason pointer')
        return {'status_0b': status_byte, 'aggregate_boolean': self.mu.mem_read(self.OUTPUT, 1)[0],
                'first_timeout': struct.unpack('<I', self.mu.mem_read(self.REFERENCE + 0x2c, 4))[0],
                'reason': 'HighOrLowTemperature' if reason == 0x80ed2be6 else reason or None,
                'calls': list(self.calls), 'transactions': list(self.transactions),
                'hardware_access': False}

    def clock_counter(self, raw_counter):
        """Run the actual stable counter reader, using two equal fake reads.

        This proves integer provenance, not the physical counter frequency.
        Both PMU transactions read emulator RAM; no real register is touched.
        """
        if not isinstance(raw_counter, int) or not 0 <= raw_counter <= 0xffffffff:
            raise ValueError('raw counter must be unsigned 32-bit')
        self.transactions, self.calls = [], []
        self.registers[0x5d:0x61] = struct.pack('<I', raw_counter)
        for reg, value in ((UC_ARM_REG_SP, d.STACK + 0xff00), (UC_ARM_REG_LR, d.END | 1),
                           (UC_ARM_REG_R0, d.PMU)):
            self.mu.reg_write(reg, value)
        self.mu.emu_start(0x80ecd9b9, d.END, count=20000)
        if self.mu.reg_read(UC_ARM_REG_PC) != d.END:
            raise RuntimeError('clock reader instruction budget exhausted')
        return {'returned_counter': self.mu.reg_read(UC_ARM_REG_R0),
                'transactions': list(self.transactions), 'hardware_access': False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kernelcache', type=Path)
    args = parser.parse_args()
    try:
        data = events.load_input(args.kernelcache)
        print(json.dumps({'format': 'p105-charge-status-v1', 'hardware_access': False,
                          'kernelcache_sha256': d.EXPECTED_SHA256,
                          'limitations': ['status/capacity/time and external connection are synthetic',
                                          'recovery callbacks blocked; no PMU writes',
                                          'no complete fault or physical charging validation'],
                          'experiments': [Status(data).consume(bit, 1000, first_timeout=699)
                                          for bit in (0, 2, 4, 8, 16, 32)]}, indent=2))
    except (ValueError, RuntimeError) as error:
        parser.error(str(error))


if __name__ == '__main__':
    main()
