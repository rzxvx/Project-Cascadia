#!/usr/bin/env python3
"""Offline D1946 ADC names and scaling, with physical conversion blocked.

Actual metadata/voltage/temperature methods execute. String copying and raw
ADC samples are mocks. The raw ADC start method never executes: the real
firmware measurement sequence would write PMU control registers.
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


class ADC(d.Experiment):
    RAW = d.TRAPS + 0x90
    BUFFER = d.SOURCE + 0xd000
    NAME_CHANNELS = (0, 4, 15, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27)

    def __init__(self, data):
        super().__init__(data)
        self.raw, self.raw_error = 0, 0
        self.calls = []
        self.w32(d.PMU_VTABLE + 0x3ac, self.RAW | 1)
        self.mu.mem_write(self.RAW, b'\x70\x47')
        self.mu.hook_add(UC_HOOK_CODE, self.helper, begin=self.RAW, end=self.RAW)
        self.mu.hook_add(UC_HOOK_CODE, self.helper, begin=0x80ed216c, end=0x80ed216c)

    def helper(self, mu, address, size, user):
        args = [mu.reg_read(r) for r in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)]
        if address == self.RAW:
            if args[0] != d.PMU or args[2] != 0 or not d.STACK <= args[3] < d.STACK + 0x10000:
                raise RuntimeError('unexpected synthetic raw ADC arguments')
            self.calls.append({'mocked': 'raw ADC conversion', 'channel': args[1], 'raw': self.raw,
                               'return_code': self.raw_error})
            if not self.raw_error:
                self.w32(args[3], self.raw)
            result = self.raw_error
        else:
            dst, src, count = args[:3]
            if not self.BUFFER <= dst < self.BUFFER + 128 or not 1 <= count <= 64 or not d.KLO <= src < d.KHI:
                raise RuntimeError('unexpected string-copy fixture')
            value = bytes(mu.mem_read(src, 64)).split(b'\0')[0]
            mu.mem_write(dst, value[:count - 1] + b'\0')
            self.calls.append({'mocked': 'strlcpy', 'size': count})
            result = len(value)
        mu.reg_write(UC_ARM_REG_R0, result)
        mu.reg_write(UC_ARM_REG_PC, mu.reg_read(UC_ARM_REG_LR))

    def invoke(self, address, args, stack_args=()):
        stack = d.STACK + 0xff00
        self.calls, self.transactions = [], []
        for idx, value in enumerate(stack_args):
            self.w32(stack + idx * 4, value)
        for reg, value in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3), args):
            self.mu.reg_write(reg, value)
        self.mu.reg_write(UC_ARM_REG_SP, stack)
        self.mu.reg_write(UC_ARM_REG_LR, d.END | 1)
        self.mu.emu_start(address | 1, d.END, count=20000)
        if self.mu.reg_read(UC_ARM_REG_PC) != d.END:
            raise RuntimeError('ADC fixture instruction budget exhausted')
        if self.transactions:
            raise RuntimeError('metadata/scaling unexpectedly attempted PMU I/O')
        return self.mu.reg_read(UC_ARM_REG_R0)

    def name(self, channel):
        if channel not in self.NAME_CHANNELS:
            raise ValueError('channel not in bounded metadata fixture set')
        self.mu.mem_write(self.BUFFER, bytes(256))
        meta = self.BUFFER + 128
        result = self.invoke(0x80ecdd8c, (d.PMU, channel, self.BUFFER, 64),
                             (meta, meta + 1, meta + 2))
        return {'channel': channel, 'name': bytes(self.mu.mem_read(self.BUFFER, 64)).split(b'\0')[0].decode(),
                'metadata': bytes(self.mu.mem_read(meta, 3)).decode(), 'return_code': result,
                'calls': list(self.calls), 'hardware_access': False}

    def scale(self, channel, raw, raw_error=0):
        if channel not in (0, 4, 15, 21, 27) or not isinstance(raw, int) or not 0 <= raw <= 4095:
            raise ValueError('bounded scale fixture permits channels 0/4/15/21/27 and raw12-bit samples')
        if not isinstance(raw_error, int) or not 0 <= raw_error <= 0xffffffff:
            raise ValueError('raw conversion error must be unsigned 32-bit')
        self.raw, self.raw_error = raw, raw_error
        output, sentinel = self.BUFFER + 256, 0xc0ffee
        self.w32(output, sentinel)
        method = 0x80ece328 if channel == 21 else 0x80ece490
        result = self.invoke(method, (d.PMU, channel, output, 0))
        return {'channel': channel, 'raw_fixture': raw, 'return_code': result,
                'scaled_integer': struct.unpack('<i', self.mu.mem_read(output, 4))[0],
                'output_untouched': bytes(self.mu.mem_read(output, 4)) == struct.pack('<I', sentinel),
                'calls': list(self.calls), 'transactions': list(self.transactions), 'hardware_access': False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kernelcache', type=Path)
    args = parser.parse_args()
    try:
        data = events.load_input(args.kernelcache)
        print(json.dumps({'format': 'p105-d1946-adc-v1', 'kernelcache_sha256': d.EXPECTED_SHA256,
                          'hardware_access': False,
                          'limitations': ['raw ADC conversion blocked', 'no physical calibration or charging permission'],
                          'names': [ADC(data).name(c) for c in ADC.NAME_CHANNELS],
                          'scale_examples': [ADC(data).scale(c, raw) for c in (0, 4, 15, 21, 27)
                                             for raw in (0, 2048, 4095)]}, indent=2))
    except (ValueError, RuntimeError) as error:
        parser.error(str(error))


if __name__ == '__main__':
    main()
