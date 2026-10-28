#!/usr/bin/env python3
"""Offline actual source ADC getter and bounded voltage arithmetic blocks.

Command-gate dispatch and raw ADC are fakes. No whole charger routine,
physical conversion, averaging, capacity interpolation or PMU I/O executes.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct

from unicorn import UC_HOOK_CODE
from unicorn.arm_const import (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,
                               UC_ARM_REG_R3, UC_ARM_REG_R4, UC_ARM_REG_R5,
                               UC_ARM_REG_R8, UC_ARM_REG_R10, UC_ARM_REG_R11,
                               UC_ARM_REG_SP, UC_ARM_REG_PC)

ROOT = Path(os.environ.get('CASCADIA_RESEARCH_REPO', Path(__file__).resolve().parents[2]))
spec = importlib.util.spec_from_file_location('adc', ROOT / 'tools/power/emulate-d1946-adc.py')
a = importlib.util.module_from_spec(spec)
spec.loader.exec_module(a)
d = a.d


class Units(a.ADC):
    GATE, GATE_VT = d.SOURCE + 0xc000, d.SOURCE + 0xc100
    GATE_CALL, OWNER = d.TRAPS + 0x98, d.SOURCE + 0x8000

    def __init__(self, data):
        if hashlib.sha256(data).hexdigest() != d.EXPECTED_SHA256:
            raise ValueError('kernelcache hash mismatch; refusing fixed addresses')
        super().__init__(data)
        for lo, hi in ((0x80ec8000, 0x80ecb000), (0x804a9000, 0x804f1000)):
            self.mu.mem_map(lo, hi - lo)
            for va, _, offset, size in d.segments(data):
                start, end = max(va, lo), min(va + size, hi)
                if start < end:
                    self.mu.mem_write(start, data[offset + start - va:offset + end - va])
        self.w32(d.PMU + 0x60, self.GATE)
        self.w32(self.GATE, self.GATE_VT)
        self.w32(self.GATE_VT + 0x70, self.GATE_CALL | 1)
        self.w32(d.PMU_VTABLE + 0x394, 0x80ece491)
        self.mu.mem_write(self.GATE_CALL, b'\x70\x47')  # synthetic Thumb return stub
        self.mu.hook_add(UC_HOOK_CODE, self.gate, begin=self.GATE_CALL, end=self.GATE_CALL)
        for address in (0x804c4e0a, 0x804c5076, 0x804c5306):
            self.mu.hook_add(UC_HOOK_CODE, self.stop, begin=address, end=address)

    def stop(self, mu, address, size, user):
        mu.reg_write(UC_ARM_REG_PC, d.END)

    def gate(self, mu, address, size, user):
        obj, callback, channel, out = [mu.reg_read(r) for r in
                                      (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)]
        if (obj, callback, channel) != (self.GATE, 0x80ece491, 4):
            raise RuntimeError('unexpected actual ADC dispatch')
        if not d.STACK <= out <= d.STACK + 0x10000 - 4:
            raise RuntimeError('unexpected command-gate output pointer')
        self.calls.append({'mocked': 'command gate dispatch',
                           'actual_callback': hex(callback), 'channel': channel})
        # Preserve the original wrapper's link register. Execute the actual
        # scaling callback with fake command-gate owner/argument forwarding.
        for reg, value in ((UC_ARM_REG_R0, d.PMU), (UC_ARM_REG_R1, channel),
                           (UC_ARM_REG_R2, out), (UC_ARM_REG_R3, 0)):
            mu.reg_write(reg, value)
        mu.reg_write(UC_ARM_REG_PC, callback)

    def block(self, start, registers):
        for reg, value in registers:
            self.mu.reg_write(reg, value & 0xffffffff)
        self.mu.reg_write(UC_ARM_REG_SP, d.STACK + 0xfe00)
        self.mu.emu_start(start | 1, d.END, count=20000)
        if self.mu.reg_read(UC_ARM_REG_PC) != d.END:
            raise RuntimeError('block instruction budget exhausted')

    def sample(self, raw, compensation=0, error=0):
        if type(raw) is not int or not 0 <= raw <= 4095:
            raise ValueError('raw sample must be an integer in 0..4095')
        if type(compensation) is not int or not -1000 <= compensation <= 1000:
            raise ValueError('compensation fixture must be integer in -1000..1000')
        if type(error) is not int or not 0 <= error <= 0xffffffff:
            raise ValueError('uint32 conversion error required')
        if hasattr(self, '_used'):
            raise RuntimeError('fresh experiment required')
        self._used = True
        self.raw, self.raw_error = raw, error
        output = self.BUFFER + 256
        self.w32(output, 0x80000000)
        self.invoke(0x804c55c8, (self.OWNER, d.SOURCE, output, 0))
        encoded = struct.unpack('<i', self.mu.mem_read(output, 4))[0]
        common = {'raw_fixture': raw, 'encoded': encoded,
                  'compensation_fixture': compensation, 'raw_error_fixture': error,
                  'calls': list(self.calls), 'transactions': self.transactions,
                  'hardware_access': False}
        if encoded == -0x80000000:
            return common
        # One successful sample; fp/r8 form the signed64 accumulator.
        self.block(0x804c4dfe, ((UC_ARM_REG_R0, encoded), (UC_ARM_REG_R11, 0),
                                (UC_ARM_REG_R8, 0), (UC_ARM_REG_R10, 0)))
        low, high = self.mu.reg_read(UC_ARM_REG_R11), self.mu.reg_read(UC_ARM_REG_R8)
        self.block(0x804c5060, ((UC_ARM_REG_R11, low), (UC_ARM_REG_R8, high),
                                (UC_ARM_REG_R10, compensation), (UC_ARM_REG_R5, self.OWNER)))
        adjusted_low = self.mu.reg_read(UC_ARM_REG_R3)
        adjusted_high = self.mu.reg_read(UC_ARM_REG_R10)
        # Capacity interpolation and the intermediate stack-store block are
        # outside scope. Forward their known arithmetic context explicitly.
        self.w32(d.STACK + 0xfe00 + 0x24, adjusted_low)
        self.block(0x804c52f6, ((UC_ARM_REG_R10, adjusted_high), (UC_ARM_REG_R4, self.OWNER)))
        value = struct.unpack('<I', self.mu.mem_read(self.OWNER + 0x22c, 4))[0]
        return common | {'scaled64': (high << 32) | low,
                         'adjusted64': (adjusted_high << 32) | adjusted_low,
                         'stored_integer': value}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kernelcache', type=Path)
    args = parser.parse_args()
    try:
        data = a.events.load_input(args.kernelcache)
        examples = [Units(data).sample(raw, offset) for raw, offset in
                    ((0, 0), (2048, 0), (3072, 0), (4095, 0), (3072, 20), (3072, -15))]
        examples.append(Units(data).sample(3072, error=0xe00002c0))
        print(json.dumps({'format': 'p105-voltage-unit-chain-v1',
                          'kernelcache_sha256': d.EXPECTED_SHA256,
                          'hardware_access': False,
                          'limitations': ['command gate and raw ADC mocked',
                                          'single sample and bounded signed compensation fixtures',
                                          'averaging, compensation selection and full capacity routine not executed',
                                          'software mV convention does not certify physical calibration or charging'],
                          'experiments': examples}, indent=2))
    except (OSError, ValueError, RuntimeError) as error:
        parser.error(str(error))


if __name__ == '__main__':
    main()
