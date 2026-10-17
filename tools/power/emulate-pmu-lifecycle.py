#!/usr/bin/env python3
"""Offline 12H321 D1946 platform wake/sleep/quiesce dispatch experiment.

Execute actual dispatch/register mutation instructions against emulator RAM.
Mutex/provider/GPIO/timer helpers are explicit mocks. Events are synthetic;
this is not a complete resume implementation or physical charging simulation.
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


class Lifecycle(d.Experiment):
    KEYS = {'sleep': 0x10008000, 'wake': 0x10008004, 'quiesce': 0x10008008}
    MOCKED = {0x80ed20dc: 'current_thread', 0x80ed1cdc: 'mutex lock',
              0x80ed1cfc: 'mutex unlock', 0x80ed1e6c: 'provider state helper',
              0x80ecbe98: 'GPIO batch helper', 0x80ecd370: 'quiesce helper',
              0x80ed20ec: 'kprintf', 0x80ed210c: 'memory copy',
              d.TRAPS + 0x80: 'event source slot54',
              d.TRAPS + 0x84: 'event source slot58',
              d.TRAPS + 0x88: 'event source slot78',
              d.TRAPS + 0x8c: 'owner slot1cc'}

    def __init__(self, data, control=0xbf, variant_field=7, fail_transfer=None):
        super().__init__(data, initial_charge_control=control, fail_transfer=fail_transfer)
        self.helpers = []
        # Only virtual I/O is mocked. The dispatcher and event-read method are
        # actual code; these object fields do not describe a measured device.
        self.w32(d.SOURCE, d.PMU_VTABLE)
        self.w32(d.PMU_VTABLE + 0x388, 0x80ecc461)
        self.w32(d.PMU_VTABLE + 0x1cc, (d.TRAPS + 0x8c) | 1)
        self.w32(d.SOURCE + 0x80, d.SOURCE + 0x9000)
        self.w32(d.SOURCE + 0x9000, d.SOURCE + 0x9100)
        for slot, address in ((0x54, d.TRAPS + 0x80), (0x58, d.TRAPS + 0x84),
                              (0x78, d.TRAPS + 0x88)):
            self.w32(d.SOURCE + 0x9100 + slot, address | 1)
        self.mu.mem_write(d.TRAPS + 0x80, b'\x70\x47' * 8)
        self.w32(d.SOURCE + 0xe4, variant_field)
        self.registers[0x1c] = 0x12
        for lo in (0x80405000, 0x804da000):
            self.mu.mem_map(lo, 0x1000)
        self.w32(0x80405388, 0x13579bdf)  # synthetic stack canary
        for address, action in ((0x80405578, 'sleep'), (0x8040557c, 'wake'),
                                (0x80405580, 'quiesce')):
            self.w32(address, self.KEYS[action])
        self.w32(0x804daad0, 0x10008010)
        self.w32(0x804daad4, 0x10008014)
        for address in self.MOCKED:
            self.mu.hook_add(UC_HOOK_CODE, self.helper, begin=address, end=address)

    def io(self, mu, address, size, user):
        if mu.reg_read(UC_ARM_REG_R0) != d.SOURCE:
            raise RuntimeError('unexpected lifecycle PMU object')
        mu.reg_write(UC_ARM_REG_R0, d.PMU)
        super().io(mu, address, size, user)

    def helper(self, mu, address, size, user):
        args = [mu.reg_read(r) for r in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2)]
        self.helpers.append({'mocked': self.MOCKED[address], 'arguments': args})
        result = 0
        if address == 0x80ed210c:
            if args[2] > 256:
                raise RuntimeError('unexpected large mock copy')
            mu.mem_write(args[0], bytes(mu.mem_read(args[1], args[2])))
            result = args[0]
        mu.reg_write(UC_ARM_REG_R0, result)
        mu.reg_write(UC_ARM_REG_PC, mu.reg_read(UC_ARM_REG_LR))

    def dispatch(self, action):
        if action not in self.KEYS:
            raise ValueError('expected sleep, wake or quiesce')
        result = self.call(0x80ecc6e0, self.KEYS[action], stack_args=(0, 0, 0))
        return {'action': action, 'return_code': result,
                'register_1c': f'0x{self.registers[0x1c]:02x}',
                'register_3f': f'0x{self.registers[0x3f]:02x}',
                'transactions': self.transactions, 'helpers': self.helpers,
                'hardware_access': False}


def load_input(path):
    data = Path(path).read_bytes()
    if hashlib.sha256(data).hexdigest() != d.EXPECTED_SHA256:
        raise ValueError('kernelcache hash mismatch; refusing fixed addresses')
    return data


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('kernelcache', type=Path)
    args = p.parse_args()
    try:
        data = load_input(args.kernelcache)
        print(json.dumps({'format': 'p105-pmu-lifecycle-emulation-v1',
                          'kernelcache_sha256': d.EXPECTED_SHA256,
                          'limitations': ['synthetic zero pending events',
                                          'provider/mutex/GPIO/quiesce helpers mocked',
                                          'no physical charge-enable or protection claim'],
                          'experiments': [Lifecycle(data).dispatch(a) for a in Lifecycle.KEYS]}, indent=2))
    except (ValueError, RuntimeError) as error:
        p.error(str(error))


if __name__ == '__main__':
    main()
