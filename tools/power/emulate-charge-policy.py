#!/usr/bin/env python3
"""Offline p105 temperature/voltage charge-table experiment.

Runs the actual 12H321 AppleARMPlatform function 0x804c3ef4 against mocked
OSData methods and a synthetic charger object. Hashes both local firmware
inputs. It never opens devices, contacts the iPad, or programs charger hardware.
Selected input-budget and USBd state routines are also exercised offline.
Whole-source classification, debounce, PMU safety and hardware protection are
not emulated; a table/current result is not permission to apply that current.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct

from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_LR, UC_ARM_REG_PC

ROOT = Path(__file__).resolve().parents[2]


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


d1946 = load_module('d1946', Path(__file__).with_name('emulate-d1946.py'))


def load_inputs(kernelcache, adt):
    data, tree = Path(kernelcache).read_bytes(), Path(adt).read_bytes()
    if hashlib.sha256(data).hexdigest() != d1946.EXPECTED_SHA256:
        raise ValueError('kernelcache hash mismatch; refusing fixed addresses')
    if hashlib.sha256(tree).hexdigest() != d1946.ADT_SHA256:
        raise ValueError('ADT hash mismatch; refusing a different board policy')
    parser = load_module('adt_map', ROOT / 'scripts/adt-pmgr-map.py')
    props = parser.adt_nodes(tree)['device-tree/charger']
    return data, props['charge-currents'], struct.unpack('<4I', props['charge-limits'])


class Policy(d1946.Experiment):
    """Mock only OSData; execute Apple's selection, hysteresis and cap logic."""
    TABLE, LIMITS, VTABLE, BYTES = (d1946.SOURCE + n for n in (0x4000, 0x4100, 0x4200, 0x5000))
    GET_LENGTH, GET_BYTES = d1946.TRAPS + 0x20, d1946.TRAPS + 0x24

    def __init__(self, data, table, limit=2520):
        super().__init__(data)
        self.table = table
        # Map AppleARMPlatform including its constants. No Apple code is copied
        # into this repository; the user supplies their verified local image.
        lo, hi = 0x804a9000, 0x804f1000
        self.mu.mem_map(lo, hi - lo)
        for va, vsize, fileoff, filesize in d1946.segments(data):
            start, end = max(va, lo), min(va + filesize, hi)
            if start < end:
                self.mu.mem_write(start, data[fileoff + start - va:fileoff + end - va])
        self.mu.mem_write(self.BYTES, table)
        self.w32(self.TABLE, self.VTABLE)
        self.w32(self.VTABLE + 0x4c, self.GET_LENGTH | 1)
        self.w32(self.VTABLE + 0x6c, self.GET_BYTES | 1)
        self.w32(d1946.SOURCE + 0x238, self.TABLE)
        self.w32(d1946.SOURCE + 0x240, self.LIMITS)
        self.w32(d1946.SOURCE + 0x244, limit)
        self.mu.mem_write(self.GET_LENGTH, b'\x70\x47' * 4)
        self.mu.hook_add(UC_HOOK_CODE, self.osdata, begin=self.GET_LENGTH, end=self.GET_BYTES)

    def osdata(self, mu, address, size, user):
        if mu.reg_read(UC_ARM_REG_R0) != self.TABLE:
            raise RuntimeError('unexpected OSData object')
        if address == self.GET_LENGTH:
            result = len(self.table)
        elif address == self.GET_BYTES:
            offset, count = (mu.reg_read(r) for r in (UC_ARM_REG_R1, UC_ARM_REG_R2))
            result = self.BYTES + offset if offset <= len(self.table) and count <= len(self.table) - offset else 0
        else:
            raise RuntimeError('unexpected OSData call')
        mu.reg_write(UC_ARM_REG_R0, result)
        mu.reg_write(UC_ARM_REG_PC, mu.reg_read(UC_ARM_REG_LR))

    def sample(self, temperature_centi_c, voltage_mv):
        self.w32(d1946.SOURCE + 0x230, temperature_centi_c & 0xffffffff)
        self.w32(d1946.SOURCE + 0x22c, voltage_mv)
        result = self.call(0x804c3ef4)
        pointer = struct.unpack('<I', self.mu.mem_read(d1946.SOURCE + 0x23c, 4))[0]
        reason_pointer = struct.unpack('<I', self.mu.mem_read(d1946.SOURCE + 0x1c4, 4))[0]
        reasons = {0: None, 0x804cf09e: 'LowTemperature', 0x804cf0ad: 'HighTemperature'}
        if reason_pointer not in reasons:
            raise RuntimeError('unexpected table-policy reason pointer')
        return {'temperature_centi_c': temperature_centi_c, 'voltage_mV': voltage_mv,
                'policy_ceiling_mA': result, 'cached_row_offset': pointer - self.BYTES if pointer else None,
                'reason': reasons[reason_pointer]}


class InputBudget(Policy):
    """Actual 0x804c80e4/8014 arithmetic and the p105 class policy callback.

    The upstream producer of source classification is not emulated. Enum names
    cannot establish that a particular physical cable/adapter meets that class.
    """
    SOURCE_VTABLE, POLICY_CALLBACK = d1946.SOURCE + 0x6000, d1946.TRAPS + 0x28

    def __init__(self, data, policy_limit=None):
        super().__init__(data, b'')
        self.policy_limit = policy_limit
        self.policy_calls = []
        if policy_limit is None:
            # Actual AppleARMPMUCharger constructor 0x804c20c8 stores this
            # address point. Its inherited +0x350 slot resolves to 0x804c800c.
            self.w32(d1946.SOURCE, 0x804d8ce8)
            self.mu.hook_add(UC_HOOK_CODE, self.policy_callback,
                             begin=0x804c800c, end=0x804c800c)
        else:
            self.w32(d1946.SOURCE, self.SOURCE_VTABLE)
            self.w32(self.SOURCE_VTABLE + 0x350, self.POLICY_CALLBACK | 1)
            self.mu.mem_write(self.POLICY_CALLBACK, b'\x70\x47')
            self.mu.hook_add(UC_HOOK_CODE, self.policy_callback,
                             begin=self.POLICY_CALLBACK, end=self.POLICY_CALLBACK)

    def policy_callback(self, mu, address, size, user):
        if address not in (0x804c800c, self.POLICY_CALLBACK) or mu.reg_read(UC_ARM_REG_R0) != d1946.SOURCE:
            raise RuntimeError('unexpected policy callback')
        self.policy_calls.append({'arg1': mu.reg_read(UC_ARM_REG_R1),
                                  'method': 'actual return1000' if self.policy_limit is None else 'mocked',
                                  'synthetic_limit_mA': self.policy_limit})
        if self.policy_limit is not None:
            mu.reg_write(UC_ARM_REG_R0, self.policy_limit)
            mu.reg_write(UC_ARM_REG_PC, mu.reg_read(UC_ARM_REG_LR))

    def sample_budget(self, profile, usb_type=0, usb_report_ma=100, brick_index=0,
                      offset_ma=0, cap_ma=None, override_ma=None):
        self.mu.mem_write(d1946.SOURCE + 0x106, bytes([profile]))
        self.w32(d1946.SOURCE + 0x168, usb_type)
        self.w32(d1946.SOURCE + 0x160, usb_report_ma)
        self.w32(d1946.SOURCE + 0x178, brick_index)
        self.w32(d1946.SOURCE + 0x128, offset_ma & 0xffffffff)
        self.mu.mem_write(d1946.SOURCE + 0x11c, bytes([cap_ma is not None]))
        self.w32(d1946.SOURCE + 0x120, cap_ma or 0)
        self.mu.mem_write(d1946.SOURCE + 0x110, bytes([override_ma is not None]))
        self.w32(d1946.SOURCE + 0x114, override_ma or 0)
        out = d1946.SOURCE + 0x3000
        self.w32(out, 0xdeadbeef)
        self.policy_calls = []
        result = self.call(0x804c80e4, 0, out, 1)
        budget = struct.unpack('<I', self.mu.mem_read(out, 4))[0]
        return {'profile_enum106': profile, 'usb_type': usb_type,
                'usb_report_mA': usb_report_ma, 'brick_index': brick_index,
                'return_code': result, 'computed_budget_mA': budget if result == 0 else None,
                'policy_calls': list(self.policy_calls)}


class USBControl(InputBudget):
    """Execute USBd state mutation and real derived/base reset routines.

    Only cache bookkeeping, property publication, debounce and the downstream
    whole-source replan are intercepted. This is not a complete detach or
    charger-shutdown simulation: no PMU register operation is performed here.
    """
    MOCKED = {0x804c2a8c: 'cache bookkeeping', 0x804c7edc: 'property publication',
              0x804c7414: 'debounce', 0x804c75b8: 'whole-source replan'}

    def __init__(self, data):
        super().__init__(data)
        self.control_calls = []
        for address in self.MOCKED:
            self.mu.hook_add(UC_HOOK_CODE, self.side_effect,
                             begin=address, end=address)
        self.mu.hook_add(UC_HOOK_CODE, self.reset_entry,
                         begin=0x804c3e24, end=0x804c3e24)

    def side_effect(self, mu, address, size, user):
        self.control_calls.append({'mocked': self.MOCKED[address],
                                   'r1': mu.reg_read(UC_ARM_REG_R1),
                                   'r2': mu.reg_read(UC_ARM_REG_R2)})
        mu.reg_write(UC_ARM_REG_R0, 0)
        mu.reg_write(UC_ARM_REG_PC, mu.reg_read(UC_ARM_REG_LR))

    def reset_entry(self, mu, address, size, user):
        self.control_calls.append({'actual': 'derived/base reset',
                                   'port': mu.reg_read(UC_ARM_REG_R1),
                                   'reason': mu.reg_read(UC_ARM_REG_R2)})

    def control(self, flags, port=0):
        if port not in (0, 1) or not 0 <= flags <= 0xffffffff:
            raise ValueError('experiment supports ports 0/1 and a u32 flag mask')
        self.control_calls = []
        result = self.call(0x804c70e4, port, flags, 0)
        read32 = lambda offset: struct.unpack('<I', self.mu.mem_read(d1946.SOURCE + offset, 4))[0]
        read8 = lambda offset: self.mu.mem_read(d1946.SOURCE + offset, 1)[0]
        return {'flags': flags, 'port': port, 'return_code': result,
                'profile_enum106': read8(0x106 + port),
                'usb_report_mA': read32(0x160 + 4 * port),
                'usb_type': read32(0x168 + 4 * port),
                'brick_index': read32(0x178 + 4 * port),
                'source_inhibit_byte130': read8(0x130 + port),
                'handshake_failure_byte132': read8(0x132 + port),
                'calls': list(self.control_calls)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kernelcache', type=Path)
    parser.add_argument('adt', type=Path)
    args = parser.parse_args()
    try:
        data, table, limits = load_inputs(args.kernelcache, args.adt)
        cold = []
        for temperature in (-100, 999, 1000, 1500, 1600, 1601, 2000, 2100, 2101, 3400, 3500, 3501, 4500, 4501):
            for voltage in (3999, 4000, 4001, 4100, 4101):
                cold.append(Policy(data, table, limits[0]).sample(temperature, voltage))
        policy = Policy(data, table, limits[0])
        warm = [policy.sample(t, 4050) for t in (2200, 2000, 1500, 1600, 1601, 2000, 2101)]
        budgets = [InputBudget(data).sample_budget(profile, usb_type=usb_type)
                   for profile in range(4, 8) for usb_type in range(6)]
        bricks = [InputBudget(data).sample_budget(7, brick_index=index) for index in range(7)]
        control = USBControl(data)
        controls = [control.control(flags) for flags in (0x40000, 0x80000, 0x100000)]
        print(json.dumps({'format': 'p105-charge-policy-emulation-v2', 'hardware_access': False,
                          'kernelcache_sha256': d1946.EXPECTED_SHA256, 'adt_sha256': d1946.ADT_SHA256,
                          'limitations': ['selected table/budget/USBd routines only',
                                          'USBd cache bookkeeping, property publication, debounce and replan mocked',
                                          'physical source classification and PMU protection not emulated'],
                          'cold_start': cold, 'cached_sequence': warm,
                          'input_budget': budgets, 'brick_budget': bricks,
                          'usb_handshake_controls': controls}, indent=2))
    except (ValueError, RuntimeError) as error:
        parser.error(str(error))


if __name__ == '__main__':
    main()
