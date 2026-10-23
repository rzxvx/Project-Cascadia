#!/usr/bin/env python3
"""Offline source-notification consumer and D1946 input-limit adaptation.

Actual firmware instructions consume selected notification masks and execute
the actual source adjustment and cap bookkeeping methods. A separate verified
basic block stores current/time/deadline. Clock, active power state, debounce,
logging and timer delivery are mocks. No charger writes or physical faults.
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


class Backoff(events.SourceEvents):
    OWNER_VTABLE, TIMER, TIMER_VTABLE = (d.SOURCE + n for n in (0x9000, 0xa000, 0xb000))
    NAME, TIMER_CALL = d.TRAPS + 0x50, d.TRAPS + 0x54
    MOCKED = {0x804c9424: 'OSBitAndAtomic', 0x804c9a94: 'IOService getPowerState',
              0x804c9c74: 'clock_get_uptime', 0x804c73e8: 'debounce scheduling',
              0x804c9c84: 'clock_interval_to_absolutetime_interval',
              0x804c9394: 'logging', NAME: 'owner name', TIMER_CALL: 'cap timer delivery'}

    def __init__(self, data):
        super().__init__(data)
        self.calls = []
        self.power_state = 1
        self.now = 0
        self.interval_ticks = 0
        self.w32(self.OWNER, self.OWNER_VTABLE)
        self.w32(self.OWNER_VTABLE + 0xf0, self.NAME | 1)
        self.w32(self.OWNER + 0x198, self.TIMER)
        self.w32(self.TIMER, self.TIMER_VTABLE)
        self.w32(self.TIMER_VTABLE + 0x7c, self.TIMER_CALL | 1)
        self.mu.mem_write(self.NAME, b'\x70\x47' * 4)
        for address in self.MOCKED:
            self.mu.hook_add(UC_HOOK_CODE, self.consumer_helper, begin=address, end=address)
        self.mu.hook_add(UC_HOOK_CODE, self.cap_entry, begin=0x804c2bcc, end=0x804c2bcc)
        self.mu.hook_add(UC_HOOK_CODE, self.deadline_end, begin=0x804c4b12, end=0x804c4b12)

    def cap_entry(self, mu, address, size, user):
        if (mu.reg_read(UC_ARM_REG_R0), mu.reg_read(UC_ARM_REG_R1)) != (self.OWNER, d.SOURCE):
            raise RuntimeError('unexpected actual cap helper owner/source')
        self.calls.append({'actual': 'cap helper', 'candidate_ma': mu.reg_read(UC_ARM_REG_R2)})

    def deadline_end(self, mu, address, size, user):
        # The isolated verified block has stored the deadline. Stop before
        # caller-context-dependent timers/driver actions beyond this block.
        mu.reg_write(UC_ARM_REG_PC, d.END)

    def consumer_helper(self, mu, address, size, user):
        args = [mu.reg_read(r) for r in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)]
        entry = {'mocked': self.MOCKED[address]}
        result = 0
        if address == 0x804c9424:
            if args[1] != self.REFERENCE + 4:
                raise RuntimeError('unexpected pending-mask address')
            result = struct.unpack('<I', mu.mem_read(args[1], 4))[0]
            self.w32(args[1], result & args[0])
        elif address == 0x804c9a94:
            if args[0] != self.OWNER:
                raise RuntimeError('unexpected power-state owner')
            result = self.power_state
        elif address == 0x804c9c74:
            mu.mem_write(args[0], struct.pack('<Q', self.now))
        elif address == 0x804c9c84:
            if args[:2] != [500, 1000000]:
                raise RuntimeError('unexpected deadline interval or scale')
            entry.update(interval=args[0], scale=args[1], fixture_ticks=self.interval_ticks)
            mu.mem_write(args[2], struct.pack('<Q', self.interval_ticks))
        elif address == 0x804c73e8:
            if args[0] != self.OWNER:
                raise RuntimeError('unexpected debounce owner')
            entry['argument'] = args[1]
        elif address == self.NAME:
            result = 0  # logging is mocked and never dereferences this name
        elif address == self.TIMER_CALL:
            if args[0] != self.TIMER:
                raise RuntimeError('unexpected cap timer')
            entry.update(interval=args[1], scale=args[2])
        self.calls.append(entry)
        mu.reg_write(UC_ARM_REG_R0, result)
        mu.reg_write(UC_ARM_REG_PC, mu.reg_read(UC_ARM_REG_LR))

    def consume(self, mask, previous_ma, now_ticks, deadline_ticks, power_state=1, prior_cap_ma=0):
        if not 0 <= mask <= 7 or not 0 <= previous_ma <= 10000:
            raise ValueError('bounded experiment permits notification bits 1/2/4 and current 0..10000')
        if not all(isinstance(t, int) and 0 <= t <= 0xffffffffffffffff for t in (now_ticks, deadline_ticks)):
            raise ValueError('timestamps must be unsigned 64-bit fixture ticks')
        if not 0 <= power_state <= 0xffffffff:
            raise ValueError('power state must be unsigned 32-bit')
        if not 0 <= prior_cap_ma <= 10000:
            raise ValueError('bounded prior cap must be 0..10000')
        self.calls = []
        self.transactions = []
        self.now, self.power_state = now_ticks, power_state
        self.w32(self.REFERENCE + 4, mask)
        self.w32(self.REFERENCE + 0x10, previous_ma)
        self.w32(self.REFERENCE + 0x18, prior_cap_ma)
        self.mu.mem_write(self.REFERENCE + 0x24, struct.pack('<Q', deadline_ticks))
        for reg, value in ((UC_ARM_REG_SP, d.STACK + 0xff00), (UC_ARM_REG_LR, d.END | 1),
                           (UC_ARM_REG_R0, self.OWNER), (UC_ARM_REG_R1, d.SOURCE)):
            self.mu.reg_write(reg, value)
        self.mu.emu_start(0x804c2b09, d.END, count=20000)
        if self.mu.reg_read(UC_ARM_REG_PC) != d.END:
            raise RuntimeError('consumer instruction budget exhausted')
        return {'mask': mask, 'previous_ma': previous_ma, 'now_ticks': now_ticks,
                'deadline_ticks': deadline_ticks, 'power_state': power_state,
                'stored_cap_ma': struct.unpack('<I', self.mu.mem_read(self.REFERENCE + 0x18, 4))[0],
                'calls': list(self.calls), 'transactions': list(self.transactions), 'hardware_access': False}

    def deadline(self, current_ma, delta_ma, now_ticks, interval_ticks):
        """Execute only 0x804c4ade..4b10 with explicit successful-set context.

        This does not execute or certify the preceding input setter. The clock
        conversion's returned interval is a fixture, not a frequency estimate.
        """
        if not 0 <= current_ma <= 10000 or not -10000 <= delta_ma <= 10000:
            raise ValueError('current/delta outside bounded research range')
        if not all(isinstance(t, int) and 0 <= t <= 0xffffffffffffffff for t in (now_ticks, interval_ticks)):
            raise ValueError('time fixtures must be unsigned 64-bit')
        self.calls = []
        self.transactions = []
        self.now, self.interval_ticks = now_ticks, interval_ticks
        stack = d.STACK + 0xfe00
        self.w32(stack + 0x1c, self.REFERENCE)
        self.w32(stack + 0x18, self.REFERENCE + 0x1c)
        from unicorn.arm_const import UC_ARM_REG_R4, UC_ARM_REG_R8
        for reg, value in ((UC_ARM_REG_SP, stack), (UC_ARM_REG_LR, d.END | 1),
                           (UC_ARM_REG_R4, current_ma), (UC_ARM_REG_R8, delta_ma & 0xffffffff)):
            self.mu.reg_write(reg, value)
        self.mu.emu_start(0x804c4adf, d.END, count=20000)
        if self.mu.reg_read(UC_ARM_REG_PC) != d.END:
            raise RuntimeError('deadline block instruction budget exhausted')
        return {'stored_current_ma': struct.unpack('<I', self.mu.mem_read(self.REFERENCE + 0x10, 4))[0],
                'stored_delta_ma': struct.unpack('<i', self.mu.mem_read(self.REFERENCE + 0x14, 4))[0],
                'stored_time_ticks': struct.unpack('<Q', self.mu.mem_read(self.REFERENCE + 0x1c, 8))[0],
                'stored_deadline_ticks': struct.unpack('<Q', self.mu.mem_read(self.REFERENCE + 0x24, 8))[0],
                'calls': list(self.calls), 'hardware_access': False}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('kernelcache', type=Path)
    args = p.parse_args()
    try:
        data = events.load_input(args.kernelcache)
        experiments = [Backoff(data).consume(6, old, 100, 200)
                       for old in (100, 101, 199, 200, 500, 975, 1000, 2100)]
        print(json.dumps({'format': 'p105-source-backoff-v2', 'hardware_access': False,
                          'kernelcache_sha256': d.EXPECTED_SHA256,
                          'limitations': ['timestamps/current/power-state synthetic',
                                          'clock, pending-mask atomic operation and scheduling mocked',
                                          'cap bookkeeping actual; no downstream PMU setter executed'],
                          'experiments': experiments,
                          'deadline_block': Backoff(data).deadline(900, -100, 0xfffffffe, 500000000)}, indent=2))
    except (ValueError, RuntimeError) as error:
        p.error(str(error))


if __name__ == '__main__':
    main()
