#!/usr/bin/env python3
"""Offline actual D1946 ADC start/IRQ/completion/error paths in fake RAM.

Actual PMU writes operate exclusively on a synthetic byte array. Command-gate
waits, clock deadlines, locks and IRQ arrival are fixtures. No device access,
physical charger/ADC operation or Linux callback is enabled by this tool.
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


class Acquisition(d.Experiment):
    GATE, GATE_VTABLE, EVENT_TABLE = (d.SOURCE + n for n in (0x9000, 0xa000, 0xb000))
    SLEEP, WAKE, DEADLINE_SLEEP, IRQ_RETURN = (d.TRAPS + n for n in (0x100, 0x104, 0x108, 0x10c))
    ROOT_DOMAIN, ROOT_VTABLE, ROOT_PROPERTY = d.SOURCE + 0xe000, d.SOURCE + 0xe100, d.TRAPS + 0x110
    MOCKED = {0x80ed20cc: 'clock_interval_to_deadline',
              0x80ed211c: 'interrupt state helper', 0x80ed1d4c: 'event-mask lock',
              0x80ed1d5c: 'event-mask unlock', 0x80ed1d1c: 'logging',
              0x80ed203c: 'getPMRootDomain', 0x80ed1ecc: 'OSNumber metacast',
              ROOT_PROPERTY: 'sleep-type property lookup'}

    def __init__(self, data, fail_transfer=None):
        super().__init__(data, fail_transfer=fail_transfer)
        self.calls, self.wait_plan = [], []
        self.wait_return = None
        self.last_interval = 0
        self.w32(d.PMU + 0x60, self.GATE)
        self.w32(d.PMU + 0x50, self.EVENT_TABLE)
        self.w32(self.GATE, self.GATE_VTABLE)
        self.w32(self.ROOT_DOMAIN, self.ROOT_VTABLE)
        self.w32(self.ROOT_VTABLE + 0x80, self.ROOT_PROPERTY | 1)
        for offset, address in ((0x7c, self.SLEEP), (0x80, self.WAKE), (0x84, self.DEADLINE_SLEEP)):
            self.w32(self.GATE_VTABLE + offset, address | 1)
        for offset, address in ((0x388, 0x80ecc460), (0x36c, 0x80ecc608)):
            self.w32(d.PMU_VTABLE + offset, address | 1)
        self.mu.mem_write(self.SLEEP, b'\x70\x47' * 10)
        self.mu.mem_map(0x80405000, 0x1000)
        self.w32(0x80405388, 0x13579bdf)
        # Imported OSNumber metaclass cell: its identity is explicitly mocked.
        self.mu.mem_map(0x80398000, 0x1000)
        self.w32(0x803988d0, 0)
        entry = self.EVENT_TABLE + 13 * 32
        self.mu.mem_write(entry + 3, b'\x01')
        self.w32(entry + 0x10, d.PMU)
        self.w32(entry + 0x14, 0x80ecbe2d)
        self.w32(entry + 0x18, 0x120)
        for address in (*self.MOCKED, self.SLEEP, self.WAKE, self.DEADLINE_SLEEP, self.IRQ_RETURN):
            self.mu.hook_add(UC_HOOK_CODE, self.helper, begin=address, end=address)

    def helper(self, mu, address, size, user):
        args = [mu.reg_read(r) for r in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)]
        result = 0
        if address == self.IRQ_RETURN:
            if self.wait_return is None:
                raise RuntimeError('unexpected nested IRQ return')
            ret, result = self.wait_return
            self.wait_return = None
            mu.reg_write(UC_ARM_REG_R0, result)
            mu.reg_write(UC_ARM_REG_PC, ret)
            return
        if address == self.WAKE:
            if args[:3] != [self.GATE, d.PMU + 0x94, 0]:
                raise RuntimeError('unexpected ADC wakeup')
            self.calls.append({'mocked': 'commandWakeup', 'state': self.state()})
        elif address == self.SLEEP:
            raise RuntimeError('indefinite ADC state wait encountered; no automatic wake fixture')
        elif address == self.DEADLINE_SLEEP:
            if args[:2] != [self.GATE, d.PMU + 0x94]:
                raise RuntimeError('unexpected deadline wait owner/event')
            interruptible = struct.unpack('<I', mu.mem_read(mu.reg_read(UC_ARM_REG_SP), 4))[0]
            if interruptible != 0:
                raise RuntimeError('unexpected interruptible wait fixture')
            if self.last_interval == 80:
                result, event = 1, None  # synthetic settle deadline expires
            else:
                if not self.wait_plan:
                    raise RuntimeError('bounded ADC wait plan exhausted')
                result, event = self.wait_plan.pop(0)
            self.calls.append({'mocked': 'commandSleep deadline', 'result': result,
                               'event': event, 'state': self.state()})
            if event is not None:
                if self.wait_return is not None:
                    raise RuntimeError('nested wait injection')
                self.registers[1:10] = bytes(9)
                self.registers[1 + (event >> 8)] = event & 255
                self.wait_return = (mu.reg_read(UC_ARM_REG_LR), result)
                mu.reg_write(UC_ARM_REG_R0, d.PMU)
                mu.reg_write(UC_ARM_REG_LR, self.IRQ_RETURN | 1)
                mu.reg_write(UC_ARM_REG_PC, 0x80ecbca5)
                return
        elif address == 0x80ed20cc:
            if args[0] not in (80, 1000) or args[1] != 1000000:
                raise RuntimeError('unexpected ADC deadline interval')
            self.last_interval = args[0]
            mu.mem_write(args[2], struct.pack('<Q', args[0]))
            self.calls.append({'mocked': self.MOCKED[address], 'interval': args[0], 'scale': args[1]})
        else:
            self.calls.append({'mocked': self.MOCKED[address]})
            if address == 0x80ed203c:
                result = self.ROOT_DOMAIN
        mu.reg_write(UC_ARM_REG_R0, result)
        mu.reg_write(UC_ARM_REG_PC, mu.reg_read(UC_ARM_REG_LR))

    def state(self):
        return struct.unpack('<I', self.mu.mem_read(d.PMU + 0x94, 4))[0]

    def invoke(self, address, args):
        for reg, value in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3), args):
            self.mu.reg_write(reg, value)
        self.mu.reg_write(UC_ARM_REG_SP, d.STACK + 0xff00)
        self.mu.reg_write(UC_ARM_REG_LR, d.END | 1)
        self.mu.emu_start(address | 1, d.END, count=30000)
        if self.mu.reg_read(UC_ARM_REG_PC) != d.END:
            raise RuntimeError('ADC lifecycle instruction budget exhausted')
        return self.mu.reg_read(UC_ARM_REG_R0)

    def callback(self, state):
        if state not in (0, 1, 2, 3):
            raise ValueError('callback fixture state must be 0..3')
        self.w32(d.PMU + 0x94, state)
        self.calls, self.transactions = [], []
        result = self.invoke(0x80ecbe2c, (d.PMU, 0, 0, 0))
        return {'return_code': result, 'state': self.state(), 'calls': list(self.calls),
                'transactions': list(self.transactions), 'hardware_access': False}

    def power_gate(self, active, state=0):
        """Execute the real PMU active/sleep setter; absent property is a fixture.

        A busy sleep is explicitly rejected by the mock instead of inventing
        a cancellation or spinning indefinitely in a synthetic scheduler.
        """
        if state not in (0, 1, 2, 3):
            raise ValueError('power gate state must be0..3')
        self.calls, self.transactions = [], []
        self.w32(d.PMU + 0x94, state)
        self.w32(d.PMU + 0xe4, 7)
        self.mu.mem_write(d.PMU + 0x99, bytes((int(bool(active)),)))
        result = self.invoke(0x80ecd7b0, (d.PMU, int(bool(active)), 0, 0))
        return {'return_code': result, 'state': self.state(),
                'sleep_type': struct.unpack('<I', self.mu.mem_read(d.PMU + 0xe4, 4))[0],
                'calls': list(self.calls), 'transactions': list(self.transactions), 'hardware_access': False}

    def acquire(self, channel, raw, mode='complete', qualifier=False, masked=False, bad_first_byte=False):
        if not isinstance(channel, int) or not 0 <= channel <= 27 or not isinstance(raw, int) or not 0 <= raw <= 4095:
            raise ValueError('bounded acquisition requires channel0..27/raw12-bit')
        plans = {'complete': [(0, 0x120), (1, None)], 'timeout': [(1, None)],
                 'unrelated': [(0, 0x110), (1, None)], 'interrupted': [(2, None), (1, None)],
                 'restart': [(3, None), (1, None)], 'timeout-with-completion': [(1, 0x120)]}
        if mode not in plans:
            raise ValueError('unknown IRQ arrival fixture')
        self.wait_plan = plans[mode].copy()
        self.calls, self.transactions = [], []
        self.last_interval, self.wait_return = 0, None
        self.mu.mem_write(d.STACK, bytes(0x10000))
        self.w32(d.PMU + 0x94, 0)
        self.mu.mem_write(d.PMU + 0x98, b'\0')  # explicitly synthetic initial control cache
        self.mu.mem_write(self.EVENT_TABLE + 13 * 32 + 1, bytes((int(masked),)))
        self.registers[0x44:0x46] = bytes(((raw & 15) | (0x80 if bad_first_byte else 0), raw >> 4))
        output, sentinel = d.SOURCE + 0xd000, 0xc0ffee
        self.w32(output, sentinel)
        result = self.invoke(0x80ecd9c8, (d.PMU, channel, int(bool(qualifier)), output))
        value = struct.unpack('<I', self.mu.mem_read(output, 4))[0]
        return {'channel': channel, 'raw_fixture': raw, 'mode': mode, 'return_code': result,
                'output': value, 'output_untouched': value == sentinel,
                'state': self.state(), 'event_pending': self.mu.mem_read(self.EVENT_TABLE + 13 * 32 + 2, 1)[0],
                'control_43': self.registers[0x43], 'calls': list(self.calls),
                'transactions': list(self.transactions), 'hardware_access': False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kernelcache', type=Path)
    args = parser.parse_args()
    try:
        data = events.load_input(args.kernelcache)
        print(json.dumps({'format': 'p105-adc-lifecycle-v1', 'hardware_access': False,
                          'kernelcache_sha256': d.EXPECTED_SHA256,
                          'limitations': ['ADC/control/event bytes synthetic', 'no asynchronous hardware scheduling',
                                          'direct gated-method entry; locks/deadlines/waits mocked'],
                          'experiments': [Acquisition(data).acquire(c, 2048, mode)
                                          for c in (3, 15, 21, 27) for mode in ('complete', 'timeout')]}, indent=2))
    except (ValueError, RuntimeError) as error:
        parser.error(str(error))


if __name__ == '__main__':
    main()
