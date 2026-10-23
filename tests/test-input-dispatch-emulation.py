#!/usr/bin/env python3
"""Actual generic budget dispatch; explicit skip when optional image absent."""
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]
KC = Path(os.environ.get('CASCADIA_TEST_KERNELCACHE', ROOT / 'build/firmware/kernelcache.12H321.macho'))
AVAILABLE = importlib.util.find_spec('unicorn') is not None and KC.is_file()


@unittest.skipUnless(AVAILABLE, 'requires Unicorn and local hash-pinned 12H321 kernelcache')
class ActualInputDispatch(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        spec = importlib.util.spec_from_file_location('dispatch', ROOT / 'tools/power/emulate-input-dispatch.py')
        cls.p = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.p)
        cls.data = cls.p.load_input(KC)

    def run_fixture(self, ports=None, **kwargs):
        return self.p.InputDispatch(self.data).plan([{}] if ports is None else ports, **kwargs)

    def test_actual_owner_dispatch_profile4_zero(self):
        out = self.run_fixture()
        self.assertEqual(out['dispatch']['actual_entry'], '0x804c437c')
        self.assertEqual(out['dispatch']['budgets_ma'], [0])
        self.assertEqual(out['dispatch']['start_index'], 0)
        self.assertEqual(out['dispatch']['flag'], 1)
        self.assertEqual(out['transactions'], [])
        self.assertFalse(out['hardware_access'])

    def test_zero_profile_floor_can_raise_budget(self):
        self.assertEqual(self.run_fixture(minimum_ma=100)['dispatch']['budgets_ma'], [100])

    def test_unknown_profile_error_falls_back100(self):
        for profile in (0, 1, 2, 3):
            with self.subTest(profile=profile):
                out = self.run_fixture([{'profile': profile}])
                self.assertEqual(out['dispatch']['budgets_ma'], [100])
                self.assertEqual(out['dispatch']['flag'], int(profile > 1))

    def test_description8_and9_force_zero_after_floor(self):
        for description in (8, 9):
            out = self.run_fixture([{'description': description, 'profile': 7, 'brick_index': 4}], minimum_ma=100)
            self.assertEqual(out['dispatch']['budgets_ma'], [0])

    def test_global_and_port_inhibit_zero_before_floor(self):
        base = [{'profile': 7, 'brick_index': 4}]
        for ports, inhibit in ((base, True), ([base[0] | {'port_inhibit': True}], False)):
            self.assertEqual(self.run_fixture(ports, inhibit=inhibit)['dispatch']['budgets_ma'], [0])
            self.assertEqual(self.run_fixture(ports, inhibit=inhibit, minimum_ma=100)['dispatch']['budgets_ma'], [100])

    def test_usb_policy_uses_actual_class_slot(self):
        out = self.run_fixture([{'profile': 5, 'usb_type': 2, 'usb_report_ma': 500}])
        self.assertEqual(out['dispatch']['budgets_ma'], [1000])
        self.assertEqual(out['policy_calls'][0]['method'], 'actual return1000')

    def test_maximum_and_clamp_order(self):
        self.assertEqual(self.run_fixture([{'profile': 7, 'brick_index': 4}], maximum_ma=500)['dispatch']['budgets_ma'], [500])
        self.assertEqual(self.run_fixture(minimum_ma=500, maximum_ma=100)['dispatch']['budgets_ma'], [100])

    def test_two_ports_pick_larger_eligible_start_index(self):
        ports = [{'profile': 5, 'description': 3, 'usb_report_ma': 500},
                 {'profile': 7, 'description': 3, 'brick_index': 4}]
        out = self.run_fixture(ports)
        self.assertEqual(out['dispatch']['budgets_ma'], [500, 2100])
        self.assertEqual(out['dispatch']['start_index'], 1)

    def test_input_bounds_and_single_use(self):
        for ports in ([], [{}, {}, {}], [{'profile': True}], [{'description': 10}], [{'bad': 1}], [{'port_inhibit': 1}]):
            with self.assertRaises(ValueError): self.run_fixture(ports)
        for kwargs in ({'minimum_ma': True}, {'maximum_ma': -1}, {'inhibit': 1}):
            with self.assertRaises(ValueError): self.run_fixture(**kwargs)
        instance = self.p.InputDispatch(self.data)
        instance.plan([{}])
        with self.assertRaises(RuntimeError): instance.plan([{}])

    def test_hash_guard(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'wrong'
            path.write_bytes(b'wrong image')
            with self.assertRaises(ValueError): self.p.load_input(path)


if __name__ == '__main__': unittest.main()
