#!/usr/bin/env python3
"""Offline 12H321 USB vendor-current request receiver and budget experiment.

Actual firmware instructions decode the setup packet, calculate current and
execute the deferred worker. Scheduler, ACK, provider delivery and cleanup
are explicit mocks. This never sends USB requests or changes a charger.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct

from unicorn import UC_HOOK_CODE
from unicorn.arm_const import (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,
                               UC_ARM_REG_R3, UC_ARM_REG_SP, UC_ARM_REG_LR,
                               UC_ARM_REG_PC)

spec = importlib.util.spec_from_file_location('d1946', Path(__file__).with_name('emulate-d1946.py'))
d = importlib.util.module_from_spec(spec)
spec.loader.exec_module(d)


def load_input(path):
    data = Path(path).read_bytes()
    if hashlib.sha256(data).hexdigest() != d.EXPECTED_SHA256:
        raise ValueError('kernelcache hash mismatch; refusing fixed addresses')
    return data


class USBCurrent(d.Experiment):
    VTABLE = 0x80bc0168
    REQUEST, TASK, PROVIDER, PROVIDER_VTABLE = (d.SOURCE + n for n in (0x1000, 0x2000, 0x3000, 0x4000))
    DELIVER, RELEASE = d.TRAPS + 0x40, d.TRAPS + 0x44

    def __init__(self, data, base_ma=500, cap_ma=0):
        super().__init__(data)
        for lo, hi in ((0x806e5000, 0x806f3000), (0x80bb4000, 0x80bc3000)):
            self.mu.mem_map(lo, hi - lo)
            for va, _, fileoff, filesize in d.segments(data):
                start, end = max(va, lo), min(va + filesize, hi)
                if start < end:
                    self.mu.mem_write(start, data[fileoff + start - va:fileoff + end - va])
        # Independently confirm the actual constructor address point, including
        # the ABI header +8. Never infer virtual slot offsets from a raw prefix.
        literal = self.u32(0x80bb51fc)
        if (literal + 0x80bb51f2 + 4 + 8) & 0xffffffff != self.VTABLE:
            raise RuntimeError('unexpected Synopsys constructor vptr')
        if (self.u32(self.VTABLE + 0x378), self.u32(self.VTABLE + 0x37c)) != (0x80bb8371, 0x806e9d99):
            raise RuntimeError('unexpected current-method dispatch')
        self.w32(d.SOURCE, self.VTABLE)
        self.w32(d.SOURCE + 0xf0, self.REQUEST)
        self.w32(d.SOURCE + 0x94, base_ma & 0xffffffff)
        self.w32(d.SOURCE + 0x98, cap_ma & 0xffffffff)
        self.w32(self.PROVIDER, self.PROVIDER_VTABLE)
        self.w32(self.PROVIDER_VTABLE + 0x3c, self.DELIVER | 1)
        self.w32(self.PROVIDER_VTABLE + 0x14, self.RELEASE | 1)
        self.mu.mem_write(self.DELIVER, b'\x70\x47' * 4)
        self.events = []
        self.mocked = {0x80bb8370: 'schedule current worker',
                       0x806e7240: 'USB request acknowledgement',
                       0x806eb814: 'generic request delegation',
                       0x80bbd2f4: 'task deallocation',
                       0x80bbd3d4: 'thread cleanup',
                       self.DELIVER: 'ARMFunction provider delivery',
                       self.RELEASE: 'provider release',
                       self.u32(self.VTABLE + 0x14) & ~1: 'controller release'}
        mapped_pages = set()
        for address in self.mocked:
            if not any(lo <= address < hi for lo, hi in ((d.KLO, d.KHI),
                       (0x806e5000, 0x806f3000), (0x80bb4000, 0x80bc3000),
                       (d.TRAPS, d.TRAPS + 0x1000))):
                page = address & ~0xfff
                if page not in mapped_pages:
                    self.mu.mem_map(page, 0x1000)
                    mapped_pages.add(page)
                self.mu.mem_write(address, b'\x70\x47')
            self.mu.hook_add(UC_HOOK_CODE, self.helper, begin=address, end=address)

    def u32(self, address):
        return struct.unpack('<I', self.mu.mem_read(address, 4))[0]

    def s32(self, address):
        return struct.unpack('<i', self.mu.mem_read(address, 4))[0]

    def helper(self, mu, address, size, user):
        args = [mu.reg_read(r) for r in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)]
        event = {'mocked': self.mocked[address]}
        if address == 0x80bb8370:
            if args[0] != d.SOURCE:
                raise RuntimeError('unexpected scheduled controller')
            event['request_type'] = args[1]
        elif address == 0x806e7240:
            if args[0] != self.REQUEST or args[1] != 0:
                raise RuntimeError('unexpected request acknowledgement')
        elif address == self.DELIVER:
            if args[0] != self.PROVIDER or args[3] != 0:
                raise RuntimeError('unexpected ARMFunction invocation')
            event.update(current_ma=self.s32(args[1]), request_type=self.u32(args[2]))
        self.events.append(event)
        mu.reg_write(UC_ARM_REG_R0, 0)
        # The generic dispatcher is outside this focused receiver experiment.
        # Stop before invoking arbitrary USB drivers, and report delegation.
        mu.reg_write(UC_ARM_REG_PC, d.END if address == 0x806eb814 else mu.reg_read(UC_ARM_REG_LR))

    def setup(self, extra_ma, suspend_ma, request_type=0x40, request=0x40, length=0):
        if not all(isinstance(v, int) and -32768 <= v <= 32767 for v in (extra_ma, suspend_ma)):
            raise ValueError('current offsets must be signed 16-bit values')
        if not 0 <= request_type <= 255 or not 0 <= request <= 255 or not 0 <= length <= 65535:
            raise ValueError('invalid USB setup fields')
        self.events = []
        packet = struct.pack('<BBhhH', request_type, request, extra_ma, suspend_ma, length)
        self.mu.mem_write(self.REQUEST + 0xc, packet)
        result = self.call(0x806eb678)
        return {'return_code': result, 'extra_offset_ma': self.s32(d.SOURCE + 0x8c),
                'suspend_offset_ma': self.s32(d.SOURCE + 0x90),
                'events': list(self.events), 'hardware_access': False}

    def budget(self, request_type):
        if not 0 <= request_type <= 0xffffffff:
            raise ValueError('request type must be unsigned 32-bit')
        raw = self.call(0x806e9d98, request_type)
        return raw - (1 << 32) if raw & 0x80000000 else raw

    def worker(self, request_type=0):
        if not 0 <= request_type <= 0xffffffff:
            raise ValueError('request type must be unsigned 32-bit')
        self.events = []
        self.w32(self.TASK, self.PROVIDER)
        self.w32(self.TASK + 4, d.SOURCE)
        self.w32(self.TASK + 8, request_type)
        self.mu.reg_write(UC_ARM_REG_SP, d.STACK + 0xff00)
        self.mu.reg_write(UC_ARM_REG_LR, d.END | 1)
        self.mu.reg_write(UC_ARM_REG_R0, self.TASK)
        self.mu.reg_write(UC_ARM_REG_R1, 0)
        self.mu.emu_start(0x80bb8465, d.END, count=20000)
        if self.mu.reg_read(UC_ARM_REG_PC) != d.END:
            raise RuntimeError('worker instruction budget exhausted')
        return list(self.events)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('kernelcache', type=Path)
    args = p.parse_args()
    try:
        data = load_input(args.kernelcache)
        experiments = []
        for extra, suspend in ((0, 0), (2500, 2500), (-600, -600), (32767, 32767)):
            e = USBCurrent(data)
            experiments.append({'setup': e.setup(extra, suspend),
                                'budget_by_type': [e.budget(t) for t in range(7)],
                                'worker': e.worker()})
        print(json.dumps({'format': 'p105-usb-current-request-v1', 'hardware_access': False,
                          'kernelcache_sha256': d.EXPECTED_SHA256,
                          'limitations': ['scheduler, ACK, provider and cleanup mocked',
                                          'base current and positive cap are synthetic fixtures',
                                          'no physical host capacity or PMU safety assertion'],
                          'experiments': experiments}, indent=2))
    except (ValueError, RuntimeError) as error:
        p.error(str(error))


if __name__ == '__main__':
    main()
