#!/usr/bin/env python3
"""Offline D1946 source +0x84 getter through its ADC wrapper and scaler.

Only original instructions and synthetic RAM execute. Command-gate scheduling
and raw conversion are mocked; any PMU I/O or real conversion entry is refused.
This checks the getter's error contract, not its caller's charging policy.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct

from unicorn import UC_HOOK_CODE
from unicorn.arm_const import (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,
                               UC_ARM_REG_R3, UC_ARM_REG_LR, UC_ARM_REG_PC)

spec = importlib.util.spec_from_file_location('adc', Path(__file__).with_name('emulate-d1946-adc.py'))
adc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(adc)
d = adc.d


class SourceVBUS(adc.ADC):
    GATE, GATE_VT = d.SOURCE + 0xc000, d.SOURCE + 0xc100
    GATE_CALL = d.TRAPS + 0x98

    def __init__(self, data):
        if hashlib.sha256(data).hexdigest() != d.EXPECTED_SHA256:
            raise ValueError('kernelcache hash mismatch; refusing fixed addresses')
        super().__init__(data)
        self.mu.mem_map(0x80ec8000, 0x3000)
        for va, _, offset, size in d.segments(data):
            start, end = max(va, 0x80ec8000), min(va + size, 0x80ecb000)
            if start < end:
                self.mu.mem_write(start, data[offset + start - va:offset + end - va])
        for address, expected in ((0x80ed3cec, 0x80ed0b45),
                                  (0x80ed3064, 0x80ec8b11),
                                  (0x80ed359c, 0x80ece491)):
            if self.u32(address) != expected:
                raise ValueError('original source/import/PMU vtable mismatch')
        self.w32(d.PMU + 0x60, self.GATE)
        self.w32(self.GATE, self.GATE_VT)
        self.w32(self.GATE_VT + 0x70, self.GATE_CALL | 1)
        self.w32(d.PMU_VTABLE + 0x394, self.u32(0x80ed359c))
        self.mu.mem_write(self.GATE_CALL, b'\x70\x47')  # synthetic return stub
        self.mu.hook_add(UC_HOOK_CODE, self.gate, begin=self.GATE_CALL, end=self.GATE_CALL)
        self.mu.hook_add(UC_HOOK_CODE, self.refuse_conversion,
                         begin=0x80ecd9c8, end=0x80ecd9c8)

    def u32(self, address):
        return struct.unpack('<I', self.mu.mem_read(address, 4))[0]

    def io(self, mu, address, size, user):
        raise RuntimeError('PMU I/O is outside the getter fixture')

    def refuse_conversion(self, mu, address, size, user):
        raise RuntimeError('real ADC conversion is outside the getter fixture')

    def gate(self, mu, address, size, user):
        obj, callback, channel, output = [mu.reg_read(r) for r in
                                         (UC_ARM_REG_R0, UC_ARM_REG_R1,
                                          UC_ARM_REG_R2, UC_ARM_REG_R3)]
        if (obj, callback, channel) != (self.GATE, 0x80ece491, 15):
            raise RuntimeError('unexpected source ADC dispatch')
        if not d.STACK <= output <= d.STACK + 0x10000 - 4:
            raise RuntimeError('unexpected command-gate output pointer')
        self.calls.append({'mocked': 'command gate', 'channel': channel,
                           'actual_callback': hex(callback), 'error_fixture': self.gate_error})
        if self.gate_error:
            mu.reg_write(UC_ARM_REG_R0, self.gate_error)
            mu.reg_write(UC_ARM_REG_PC, mu.reg_read(UC_ARM_REG_LR))
        else:
            for reg, value in zip((UC_ARM_REG_R0, UC_ARM_REG_R1,
                                   UC_ARM_REG_R2, UC_ARM_REG_R3),
                                  (d.PMU, channel, output, 0)):
                mu.reg_write(reg, value)
            mu.reg_write(UC_ARM_REG_PC, callback)

    def sample(self, raw, raw_error=0, gate_error=0, sentinel=0x80000000):
        if type(raw) is not int or not 0 <= raw <= 4095:
            raise ValueError('raw fixture must be integer in 0..4095')
        for value in (raw_error, gate_error, sentinel):
            if type(value) is not int or not 0 <= value <= 0xffffffff:
                raise ValueError('errors and sentinel must be uint32')
        if hasattr(self, '_used'):
            raise RuntimeError('fresh experiment required')
        self._used = True
        self.raw, self.raw_error, self.gate_error = raw, raw_error, gate_error
        output = self.BUFFER + 256
        self.w32(output, sentinel)
        # Resolve the actual original vtable entry rather than a fake getter.
        entry = self.u32(self.u32(d.SOURCE) + 0x84)
        result = self.invoke(entry, (d.SOURCE, output, 0, 0))
        return {'raw_fixture': raw, 'raw_error_fixture': raw_error,
                'gate_error_fixture': gate_error, 'return_code': result,
                'output_u32': self.u32(output), 'output_untouched': self.u32(output) == sentinel,
                'calls': list(self.calls), 'transactions': list(self.transactions),
                'hardware_access': False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kernelcache', type=Path)
    args = parser.parse_args()
    try:
        data = adc.events.load_input(args.kernelcache)
        cases = [SourceVBUS(data).sample(raw) for raw in (0, 1, 2048, 4095)]
        cases += [SourceVBUS(data).sample(3072, **kw) for kw in
                  ({'raw_error': 0xe00002c0}, {'gate_error': 0xe00002d6})]
        print(json.dumps({'format': 'p105-source-vbus-getter-v1',
                          'kernelcache_sha256': d.EXPECTED_SHA256,
                          'hardware_access': False,
                          'limitations': ['command gate and raw ADC are mocks',
                                          'no consumer policy, physical calibration or cutoff proof'],
                          'cases': cases}, indent=2))
    except (OSError, ValueError, RuntimeError) as error:
        parser.error(str(error))


if __name__ == '__main__':
    main()
