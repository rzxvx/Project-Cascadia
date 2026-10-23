#!/usr/bin/env python3
"""Actual D1946 system-watchdog control in fake RAM, never charger protection.

PMU I/O, keepact ARMFunction, provider/property lookup and superclass start are
explicit mocks. No hardware watchdog, charger callback or service is enabled.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import sys
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_LR, UC_ARM_REG_PC

spec = importlib.util.spec_from_file_location('d1946', Path(__file__).with_name('emulate-d1946.py'))
d = importlib.util.module_from_spec(spec)
spec.loader.exec_module(d)
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'src'))
import dtparse


def load_input(path):
    data = Path(path).read_bytes()
    if hashlib.sha256(data).hexdigest() != d.EXPECTED_SHA256:
        raise ValueError('kernelcache hash mismatch; only pinned p105 12H321 accepted')
    return data


def board_evidence(path):
    data = Path(path).read_bytes()
    if hashlib.sha256(data).hexdigest() != d.ADT_SHA256:
        raise ValueError('ADT hash mismatch; only pinned p105 accepted')
    root, end = dtparse.parse_node(data, 0)
    if end != len(data):
        raise ValueError('ADT parser did not consume full payload')
    matches = []

    def walk(node, parent=''):
        name = node['props'].get('name', b'?').rstrip(b'\0').decode('ascii')
        path = parent + '/' + name
        if path == '/device-tree/arm-io/i2c0/pmu':
            matches.append(node['props'])
        for child in node['children']:
            walk(child, path)

    walk(root)
    if len(matches) != 1:
        raise ValueError('expected one pinned p105 PMU node')
    properties = matches[0]
    keepact = properties.get('function-keepact', b'')
    return {'adt_sha256': d.ADT_SHA256, 'pmu_property_count': len(properties),
            'pmu_watchdog_present_in_raw_adt': 'pmu-watchdog' in properties,
            'function_keepact_raw_hex': keepact.hex(), 'hardware_access': False,
            'limitation': 'raw ADT is not a runtime IORegistry property snapshot'}


class Watchdog(d.Experiment):
    FUNCTION, FUNCTION_VTABLE, PROVIDER, PROVIDER_VTABLE = (d.SOURCE + n for n in (0x6000, 0x7000, 0x9000, 0xa000))
    GPIO, GET_PROVIDER, GET_PROPERTY, GET_NAME = (d.TRAPS + n for n in (0x20, 0x24, 0x28, 0x2c))
    BASE_START = 0x804b8acc
    MOCKED = {0x80ed1ecc: 'PMU metacast', 0x80ed1edc: 'keepact function lookup',
              0x80ed1d1c: 'logging', 0x80ed20bc: 'interval conversion',
              GPIO: 'keepact ARMFunction', GET_PROVIDER: 'PMU getProvider',
              GET_PROPERTY: 'provider property lookup', GET_NAME: 'owner name',
              BASE_START: 'generic watchdog superclass start'}

    def __init__(self, data, keepact=True, enabled=False, level=0, fail_transfer=None, gpio_error=0):
        if hashlib.sha256(data).hexdigest() != d.EXPECTED_SHA256:
            raise ValueError('kernelcache hash mismatch; only pinned p105 12H321 accepted')
        if type(keepact) is not bool or type(enabled) is not bool or type(level) is not int or level not in (0, 1):
            raise ValueError('keepact/enabled must be booleans and fake level 0..1')
        if type(gpio_error) is not int or not 0 <= gpio_error <= 0xffffffff:
            raise ValueError('synthetic GPIO result must fit 32 bits')
        super().__init__(data, fail_transfer=fail_transfer)
        self.calls, self.gpio_error = [], gpio_error
        self.provider_present, self.property_present, self.keepact_found, self.base_result = True, False, keepact, True
        self.w32(d.SOURCE, 0x80ed4138)  # proven actual constructor address point
        self.w32(d.SOURCE + 0x4a0, d.PMU)
        self.w32(d.SOURCE + 0x4a4, self.FUNCTION if keepact else 0)
        self.mu.mem_write(d.SOURCE + 0x4a8, bytes((int(enabled),)))
        self.w32(d.SOURCE + 0x4ac, level)
        self.w32(self.FUNCTION, self.FUNCTION_VTABLE)
        self.w32(self.FUNCTION_VTABLE + 0x3c, self.GPIO | 1)
        self.w32(d.PMU_VTABLE + 0x1cc, self.GET_PROVIDER | 1)
        self.w32(self.PROVIDER, self.PROVIDER_VTABLE)
        self.w32(self.PROVIDER_VTABLE + 0x88, self.GET_PROPERTY | 1)
        # The generic superclass table is also an explicit fake dependency.
        # It points to the blocked start helper, never the surrounding service.
        if struct.unpack('<I', self.mu.mem_read(0x80ed31c0, 4))[0] != 0x804d6730:
            raise RuntimeError('unexpected pinned superclass table import')
        self.w32(0x80ed31c0, d.SOURCE + 0xd000)
        self.w32(d.SOURCE + 0xd000 + 0x170, self.BASE_START | 1)
        self.w32(0x80ed4138 + 0xf0, self.GET_NAME | 1)  # fake name getter in emulator RAM only
        self.mu.mem_write(self.GPIO, b'\x70\x47' * 8)
        self.mu.mem_map(0x804b8000, 0x1000)
        self.mu.mem_write(self.BASE_START, b'\x70\x47')
        for address in self.MOCKED:
            self.mu.hook_add(UC_HOOK_CODE, self.helper, begin=address, end=address)

    def helper(self, mu, address, size, unused):
        args = [mu.reg_read(r) for r in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)]
        call, result = {'mocked': self.MOCKED[address]}, 0
        if address == self.GPIO:
            if args != [self.FUNCTION, d.SOURCE + 0x4ac, 0, 0]:
                raise RuntimeError('unexpected keepact dispatch arguments')
            call['fake_level'] = struct.unpack('<I', mu.mem_read(args[1], 4))[0]
            result = self.gpio_error
        elif address == self.GET_PROVIDER:
            if args[0] != d.PMU:
                raise RuntimeError('unexpected PMU provider owner')
            result = self.PROVIDER
        elif address == self.GET_PROPERTY:
            if args[:2] != [self.PROVIDER, 0x80ed2d45]:
                raise RuntimeError('unexpected watchdog property lookup')
            result = d.SOURCE + 0xb000 if self.property_present else 0
        elif address == 0x80ed1ecc:
            if args[0] != d.PMU:
                raise RuntimeError('unexpected PMU metacast input')
            result = d.PMU if self.provider_present else 0
        elif address == 0x80ed1edc:
            if args[:2] != [self.PROVIDER, 0x80ed2d52]:
                raise RuntimeError('unexpected keepact lookup')
            result = self.FUNCTION if self.keepact_found else 0
        elif address == self.BASE_START:
            if args[:2] != [d.SOURCE, d.PMU]:
                raise RuntimeError('unexpected generic watchdog start')
            result = int(self.base_result)
        elif address == 0x80ed20bc:
            if args[:3] != [500, 1000000, d.SOURCE + 0xc000]:
                raise RuntimeError('unexpected watchdog interval arguments')
            call.update(interval=500, scale=1000000)
            mu.mem_write(args[2], struct.pack('<Q', 500000))  # explicit fake result, not real ticks
        self.calls.append(call)
        mu.reg_write(UC_ARM_REG_R0, result)
        mu.reg_write(UC_ARM_REG_PC, mu.reg_read(UC_ARM_REG_LR))

    def snapshot(self, result):
        return {'r0_after_return': result, 'enabled_cache': self.mu.mem_read(d.SOURCE + 0x4a8, 1)[0],
                'fake_keepact_level': struct.unpack('<I', self.mu.mem_read(d.SOURCE + 0x4ac, 4))[0],
                'fake_register_80': self.registers[0x80], 'calls': list(self.calls),
                'transactions': list(self.transactions), 'hardware_access': False}

    def control(self, enabled):
        if type(enabled) is not bool:
            raise ValueError('enable fixture must be a boolean')
        return self.snapshot(self.call(0x80ed17f0, int(enabled)))

    def kick(self):
        return self.snapshot(self.call(0x80ed17c0))

    def interval(self):
        output = d.SOURCE + 0xc000
        self.call(0x80ed17b0, output)
        return {'calls': list(self.calls), 'transactions': list(self.transactions), 'hardware_access': False}

    def start(self, provider_present=True, property_present=False, keepact_found=True, base_result=True):
        if any(type(v) is not bool for v in (provider_present, property_present, keepact_found, base_result)):
            raise ValueError('start fixtures must be booleans')
        self.provider_present, self.property_present = provider_present, property_present
        self.keepact_found, self.base_result = keepact_found, base_result
        self.w32(d.SOURCE + 0x4a4, 0)
        return self.snapshot(self.call(0x80ed1724, d.PMU))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kernelcache', type=Path)
    parser.add_argument('--adt', type=Path, default=ROOT / 'build/usb-host-research/firmware/DeviceTree.p105ap.raw')
    args = parser.parse_args()
    try:
        data = load_input(args.kernelcache)
        print(json.dumps({'format': 'p105-pmu-watchdog-v1', 'hardware_access': False,
                          'kernelcache_sha256': d.EXPECTED_SHA256, 'board': board_evidence(args.adt),
                          'limitations': ['system-watchdog class is not proof of charger safety timer',
                                          'generic superclass start mocked', 'no physical GPIO or PMU operation'],
                          'experiments': {'start_no_property': Watchdog(data).start(),
                                          'enable': Watchdog(data).control(True),
                                          'disable': Watchdog(data, enabled=True).control(False),
                                          'interval': Watchdog(data).interval()}}, indent=2))
    except (OSError, ValueError, RuntimeError) as error:
        parser.error(str(error))


if __name__ == '__main__':
    main()
