#!/usr/bin/env python3
"""Offline actual 12H321 budget construction and PMUCharger dispatch.

Runs 0x804c81a0, real source-budget helpers and actual vptr+0x358 dispatch.
Intercepts the downstream current planner before any PMU transaction. Port
classification and clamp fields are fixtures, not physical attach evidence.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_PC, UC_ARM_REG_LR, UC_ARM_REG_SP

spec = importlib.util.spec_from_file_location('policy', Path(__file__).with_name('emulate-charge-policy.py'))
p = importlib.util.module_from_spec(spec)
spec.loader.exec_module(p)
d = p.d1946


def load_input(path):
    data = Path(path).read_bytes()
    if hashlib.sha256(data).hexdigest() != d.EXPECTED_SHA256:
        raise ValueError('kernelcache hash mismatch; refusing fixed addresses')
    return data


def integer(value, low, high, name):
    if type(value) is not int or not low <= value <= high:
        raise ValueError(f'{name} must be an integer in {low}..{high}')
    return value


class InputDispatch(p.InputBudget):
    def __init__(self, data):
        super().__init__(data)
        self.dispatches = []
        self.mu.mem_map(0x80405000, 0x1000)
        self.w32(0x80405388, 0x13579bdf)  # synthetic stack canary
        self.mu.hook_add(UC_HOOK_CODE, self.block_dispatch, begin=0x804c437c, end=0x804c437c)

    def block_dispatch(self, mu, address, size, user):
        args = [mu.reg_read(r) for r in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)]
        if args[0] != d.SOURCE:
            raise RuntimeError('unexpected dispatch owner')
        count = struct.unpack('<I', mu.mem_read(d.SOURCE + 0xac, 4))[0]
        if not 1 <= count <= 2 or args[2] >= count or args[3] not in (0, 1):
            raise RuntimeError('unexpected dispatch bounds')
        output = struct.unpack('<I', mu.mem_read(mu.reg_read(UC_ARM_REG_SP), 4))[0]
        if output != d.SOURCE + 0xf000:
            raise RuntimeError('unexpected dispatch output pointer')
        self.dispatches.append({'actual_entry': hex(address),
                                'mocked': 'downstream current planner blocked',
                                'budgets_ma': list(struct.unpack('<' + 'I' * count, mu.mem_read(args[1], 4 * count))),
                                'start_index': args[2], 'flag': args[3]})
        mu.reg_write(UC_ARM_REG_R0, 0)
        mu.reg_write(UC_ARM_REG_PC, mu.reg_read(UC_ARM_REG_LR))

    def plan(self, ports, minimum_ma=0, maximum_ma=3262, inhibit=False):
        if not isinstance(ports, (tuple, list)) or not 1 <= len(ports) <= 2:
            raise ValueError('one or two synthetic ports required')
        integer(minimum_ma, 0, 3262, 'minimum fixture')
        integer(maximum_ma, 0, 3262, 'maximum fixture')
        if type(inhibit) is not bool:
            raise ValueError('inhibit must be boolean')
        defaults = {'description': 0, 'profile': 4, 'usb_type': 0,
                    'usb_report_ma': 0, 'brick_index': 0, 'port_inhibit': False}
        normalized = []
        for port in ports:
            if not isinstance(port, dict) or set(port) - set(defaults):
                raise ValueError('invalid port fixture keys')
            values = defaults | port
            for key, high in (('description', 9), ('profile', 7), ('usb_type', 4),
                              ('usb_report_ma', 3262), ('brick_index', 5)):
                integer(values[key], 0, high, key)
            if type(values['port_inhibit']) is not bool:
                raise ValueError('port_inhibit must be boolean')
            normalized.append(values)
        if hasattr(self, '_used'):
            raise RuntimeError('use a fresh experiment for each fixture')
        self._used = True
        self.w32(d.SOURCE + 0xac, len(ports))
        self.w32(d.SOURCE + 0x138, minimum_ma)
        self.w32(d.SOURCE + 0x14c, maximum_ma)
        self.mu.mem_write(d.SOURCE + 0x109, bytes((inhibit,)))
        for index, values in enumerate(normalized):
            for offset, key in ((0x104, 'description'), (0x106, 'profile'), (0x130, 'port_inhibit')):
                self.mu.mem_write(d.SOURCE + offset + index, bytes((values[key],)))
            for offset, key in ((0x160, 'usb_report_ma'), (0x168, 'usb_type'), (0x178, 'brick_index')):
                self.w32(d.SOURCE + offset + 4 * index, values[key])
        self.call(0x804c81a0, d.SOURCE + 0xf000)
        if len(self.dispatches) != 1 or self.transactions:
            raise RuntimeError('unexpected dispatch count or PMU access')
        return {'port_fixtures': normalized, 'minimum_ma_fixture': minimum_ma,
                'maximum_ma_fixture': maximum_ma, 'inhibit_fixture': inhibit,
                'dispatch': self.dispatches[0], 'policy_calls': self.policy_calls,
                'transactions': self.transactions, 'hardware_access': False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kernelcache', type=Path)
    args = parser.parse_args()
    try:
        data = load_input(args.kernelcache)
        examples = [InputDispatch(data).plan([{'profile': 4}], floor)
                    for floor in (0, 100)]
        examples.extend(InputDispatch(data).plan([{'profile': profile, 'description': desc}], floor)
                        for profile, desc, floor in ((0, 0, 0), (4, 8, 100), (7, 9, 100)))
        print(json.dumps({'format': 'p105-input-dispatch-v1', 'hardware_access': False,
                          'kernelcache_sha256': d.EXPECTED_SHA256,
                          'limitations': ['port classification and clamp fields synthetic',
                                          'downstream planner blocked before PMU I/O',
                                          'no physical detach or fault safety proof'],
                          'experiments': examples}, indent=2))
    except (ValueError, RuntimeError) as error:
        parser.error(str(error))


if __name__ == '__main__': main()
