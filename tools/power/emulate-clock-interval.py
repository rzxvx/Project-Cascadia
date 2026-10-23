#!/usr/bin/env python3
"""Compare actual pinned ARM clock conversions offline in fake RAM.

Frequency and current time are explicit fixtures. Integer division and the
three clock functions execute actual instructions; no physical clock, PMU,
ADC, USB or device interface is accessed. No Apple code bytes are distributed.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
from unicorn import Uc, UcError, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_CODE
from unicorn.arm_const import (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,
                               UC_ARM_REG_R3, UC_ARM_REG_SP, UC_ARM_REG_LR,
                               UC_ARM_REG_PC)

EXPECTED_SHA256 = 'db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c'
UINT32_MAX, UINT64_MAX, NSEC_PER_SEC = (1 << 32) - 1, (1 << 64) - 1, 1000000000
FUNCTIONS = {'interval': 0x800b5adc, 'nanoseconds': 0x800b5a6c, 'deadline': 0x8001dc88}
OUTPUT, STACK, END = 0x10000000, 0x20000000, 0x30000000
NOW_HELPER, FREQUENCY_IMPORT, FREQUENCY_VALUE = 0x800b2b28, 0x803920d4, 0x803ab1b8
INSTRUCTION_BUDGET = 20000


def bounded_integer(value, maximum, label, minimum=0):
    if type(value) is not int or not minimum <= value <= maximum:
        raise ValueError(f'{label} must be an integer in {minimum}..{maximum}')
    return value


def load_input(path):
    data = Path(path).read_bytes()
    if hashlib.sha256(data).hexdigest() != EXPECTED_SHA256:
        raise ValueError('kernelcache hash mismatch; only pinned p105 12H321 is accepted')
    return data


class Clock:
    def __init__(self, data, frequency=24000000, now=123456789):
        if not isinstance(data, bytes) or hashlib.sha256(data).hexdigest() != EXPECTED_SHA256:
            raise ValueError('kernelcache hash mismatch; only pinned p105 12H321 is accepted')
        self.frequency = bounded_integer(frequency, UINT32_MAX, 'fake frequency', 1)
        self.now = bounded_integer(now, UINT64_MAX, 'fake current absolute time')
        self.data, self.segments = data, []
        offset = 28
        for _ in range(struct.unpack_from('<I', data, 16)[0]):
            command, size = struct.unpack_from('<II', data, offset)
            if command == 1:
                va, _, file_offset, file_size = struct.unpack_from('<IIII', data, offset + 24)
                if va >= 0x80000000 and va + file_size <= 0x80410000:
                    self.segments.append((va, file_offset, file_size))
            offset += size

    def ticks_reference(self, nanoseconds):
        n = bounded_integer(nanoseconds, UINT64_MAX, 'nanoseconds')
        quotient, remainder = divmod(n, NSEC_PER_SEC)
        ticks = quotient * self.frequency + remainder * self.frequency // NSEC_PER_SEC
        if ticks > UINT64_MAX:
            raise ValueError('conversion would overflow 64-bit absolute ticks')
        return ticks

    def _run(self, kind, first, second):
        if kind not in FUNCTIONS:
            raise ValueError('unknown bounded clock entry')
        mu = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
        mu.mem_map(0x80000000, 0x410000)
        for va, offset, size in self.segments:
            mu.mem_write(va, self.data[offset:offset + size])
        for address in (OUTPUT, STACK):
            mu.mem_map(address, 0x10000)
        mu.mem_map(END, 0x1000)
        if struct.unpack('<I', mu.mem_read(FREQUENCY_IMPORT, 4))[0] != FREQUENCY_VALUE:
            raise RuntimeError('unexpected pinned frequency import')
        mu.mem_write(FREQUENCY_VALUE, struct.pack('<I', self.frequency))
        calls = []

        def fake_now(machine, address, size, unused):
            calls.append(address)
            machine.reg_write(UC_ARM_REG_R0, self.now & UINT32_MAX)
            machine.reg_write(UC_ARM_REG_R1, self.now >> 32)
            machine.reg_write(UC_ARM_REG_PC, machine.reg_read(UC_ARM_REG_LR))

        mu.hook_add(UC_HOOK_CODE, fake_now, begin=NOW_HELPER, end=NOW_HELPER)
        for register, value in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3),
                                   (first, second, OUTPUT, 0)):
            mu.reg_write(register, value)
        mu.reg_write(UC_ARM_REG_SP, STACK + 0xff00)
        mu.reg_write(UC_ARM_REG_LR, END | 1)
        try:
            mu.emu_start(FUNCTIONS[kind] | 1, END, count=INSTRUCTION_BUDGET)
        except UcError as error:
            raise RuntimeError(f'bounded clock emulation failed: {error}') from error
        if mu.reg_read(UC_ARM_REG_PC) != END:
            raise RuntimeError('clock instruction budget exhausted')
        if len(calls) != (1 if kind == 'deadline' else 0):
            raise RuntimeError('unexpected current-time helper call count')
        return struct.unpack('<Q', mu.mem_read(OUTPUT, 8))[0]

    def convert_nanoseconds(self, nanoseconds):
        expected = self.ticks_reference(nanoseconds)
        value = self._run('nanoseconds', nanoseconds & UINT32_MAX, nanoseconds >> 32)
        if value != expected:
            raise RuntimeError('actual nanosecond conversion differs from bounded arithmetic reference')
        return value

    def compare(self, interval, scale):
        bounded_integer(interval, UINT32_MAX, 'interval')
        bounded_integer(scale, UINT32_MAX, 'scale')
        nanoseconds = interval * scale
        expected = self.ticks_reference(nanoseconds)
        if self.now + expected > UINT64_MAX:
            raise ValueError('deadline would overflow 64-bit absolute time')
        interval_ticks = self._run('interval', interval, scale)
        nanosecond_ticks = self.convert_nanoseconds(nanoseconds)
        deadline = self._run('deadline', interval, scale)
        if interval_ticks != expected or deadline != self.now + expected:
            raise RuntimeError('actual clock functions disagree with bounded arithmetic reference')
        return {'interval': interval, 'scale': scale, 'scaled_nanoseconds': nanoseconds,
                'frequency_fixture': self.frequency, 'now_fixture': self.now,
                'interval_ticks': interval_ticks, 'nanosecond_ticks': nanosecond_ticks,
                'deadline': deadline, 'hardware_access': False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kernelcache', type=Path)
    parser.add_argument('--interval', type=lambda v: int(v, 0), default=80)
    parser.add_argument('--scale', type=lambda v: int(v, 0), default=1000000)
    parser.add_argument('--frequency', type=lambda v: int(v, 0), default=24000000,
                        help='explicit synthetic ticks per second; not a device measurement')
    parser.add_argument('--now', type=lambda v: int(v, 0), default=123456789,
                        help='explicit synthetic current absolute time')
    args = parser.parse_args()
    try:
        result = Clock(load_input(args.kernelcache), args.frequency, args.now).compare(args.interval, args.scale)
        print(json.dumps({'format': 'p105-clock-interval-v1', 'kernelcache_sha256': EXPECTED_SHA256,
                          'hardware_access': False, 'functions': FUNCTIONS,
                          'mocked': ['frequency variable 0x803ab1b8', 'current time helper 0x800b2b28'],
                          'instruction_budget_per_function': INSTRUCTION_BUDGET,
                          'limitations': ['no physical clock or ADC timing measured', 'overflowing fixtures refused',
                                          'division helpers execute actual code; no external I/O'],
                          'experiment': result}, indent=2))
    except (OSError, ValueError, RuntimeError) as error:
        parser.error(str(error))


if __name__ == '__main__':
    main()
