#!/usr/bin/env python3
"""Run the actual Apple temperature/voltage selector; no hardware access."""
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
KC = Path(os.environ.get('CASCADIA_TEST_KERNELCACHE', ROOT / 'build/firmware/kernelcache.12H321.macho'))
ADT = Path(os.environ.get('CASCADIA_TEST_ADT', ROOT / 'build/usb-host-research/firmware/DeviceTree.p105ap.raw'))
AVAILABLE = importlib.util.find_spec('unicorn') is not None and KC.is_file() and ADT.is_file()


@unittest.skipUnless(AVAILABLE, 'needs Unicorn and locally extracted p105 12H321 kernelcache/ADT')
class ChargePolicy(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        spec = importlib.util.spec_from_file_location('charge_policy', ROOT / 'tools/power/emulate-charge-policy.py')
        cls.m = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.m)
        cls.data, cls.table, cls.limits = cls.m.load_inputs(KC, ADT)

    def policy(self, limit=None):
        return self.m.Policy(self.data, self.table, self.limits[0] if limit is None else limit)

    def test_outside_temperature_table_returns_zero_including_negative(self):
        for temperature in (-100, 0, 999, 4501, 10000):
            with self.subTest(temperature=temperature):
                sample = self.policy().sample(temperature, 3900)
                self.assertEqual(sample['policy_ceiling_mA'], 0)
                self.assertEqual(sample['reason'], 'LowTemperature' if temperature < 1000 else 'HighTemperature')

    def test_inclusive_voltage_thresholds_and_current_taper(self):
        for temperature, currents in ((1100, (2520, 2520, 2240, 2240, 1320)),
                                      (1800, (2520, 2520, 2520, 2520, 1320)),
                                      (3000, (2520, 2520, 2520, 2520, 2240))):
            for voltage, current in zip((3999, 4000, 4001, 4100, 4101), currents):
                with self.subTest(temperature=temperature, voltage=voltage):
                    self.assertEqual(self.policy().sample(temperature, voltage)['policy_ceiling_mA'], current)

    def test_first_matching_row_at_cold_start_overlap(self):
        for temperature, offset in ((1000, 0), (1500, 0), (1600, 0), (1601, 20),
                                    (2000, 20), (2100, 20), (2101, 40), (3400, 40),
                                    (3500, 40), (3501, 60), (4500, 60)):
            with self.subTest(temperature=temperature):
                self.assertEqual(self.policy().sample(temperature, 3900)['cached_row_offset'], offset)

    def test_cached_row_hysteresis_and_exit(self):
        p = self.policy()
        for temperature, offset in ((2200, 40), (2000, 40), (2100, 40), (1999, 20),
                                    (1500, 20), (1499, 0), (1600, 0), (1601, 20)):
            with self.subTest(temperature=temperature):
                self.assertEqual(p.sample(temperature, 4050)['cached_row_offset'], offset)

    def test_selected_charge_limit_caps_table_current(self):
        for limit in self.limits:
            self.assertEqual(self.policy(limit).sample(3000, 3900)['policy_ceiling_mA'], limit)

    def test_inhibition_flags_short_circuit_policy(self):
        for offset in (0x1c0, 0x1c1):
            p = self.policy()
            p.mu.mem_write(self.m.d1946.SOURCE + offset, b'\x01')
            self.assertEqual(p.sample(3000, 3900)['policy_ceiling_mA'], 0)

    def test_both_local_input_hashes_required(self):
        with tempfile.TemporaryDirectory() as td:
            bad = Path(td) / 'bad'
            bad.write_bytes(b'wrong firmware')
            for kc, adt in ((bad, ADT), (KC, bad)):
                with self.assertRaises(ValueError):
                    self.m.load_inputs(kc, adt)

    def test_usb_report_budget_and_separate_stack_types(self):
        for stack_type, expected, calls in ((0, 100, 0), (1, 100, 0), (2, 1000, 1),
                                            (3, 1500, 0), (4, 1000, 1), (5, 100, 0)):
            with self.subTest(stack_type=stack_type):
                sample = self.m.InputBudget(self.data).sample_budget(5, usb_type=stack_type)
                self.assertEqual(sample['return_code'], 0)
                self.assertEqual(sample['computed_budget_mA'], expected)
                self.assertEqual(len(sample['policy_calls']), calls)
                if calls:
                    self.assertEqual(sample['policy_calls'][0]['method'], 'actual return1000')
        sample = self.m.InputBudget(self.data).sample_budget(5, usb_report_ma=500)
        self.assertEqual(sample['computed_budget_mA'], 500)

    def test_disabled_and_other_profile_budgets(self):
        for profile, expected in ((4, 0), (6, 500)):
            self.assertEqual(self.m.InputBudget(self.data).sample_budget(profile)['computed_budget_mA'], expected)
        for profile in (0, 1, 2, 3, 8, 9):
            sample = self.m.InputBudget(self.data).sample_budget(profile)
            self.assertNotEqual(sample['return_code'], 0)
            self.assertIsNone(sample['computed_budget_mA'])

    def test_brick_table_and_unknown_index_fallback(self):
        for index, expected in ((0, 1000), (1, 500), (2, 1000), (3, 1000),
                                (4, 2100), (5, 2400), (6, 1000)):
            with self.subTest(index=index):
                self.assertEqual(self.m.InputBudget(self.data).sample_budget(7, brick_index=index)
                                 ['computed_budget_mA'], expected)

    def test_input_adjustment_override_and_cap(self):
        for arguments, expected in (({'offset_ma': -150}, 0), ({'offset_ma': -25}, 75),
                                    ({'offset_ma': 50}, 150), ({'override_ma': 1000}, 1000),
                                    ({'override_ma': 1000, 'cap_ma': 500}, 500),
                                    ({'offset_ma': 50, 'cap_ma': 125}, 125)):
            with self.subTest(arguments=arguments):
                self.assertEqual(self.m.InputBudget(self.data).sample_budget(5, **arguments)
                                 ['computed_budget_mA'], expected)

    def test_handshake_allow_failure_and_unknown_flags(self):
        p = self.m.USBControl(self.data)
        for flag, inhibit, failure, action in ((0x40000, 1, 0, 'whole-source replan'),
                                               (0x80000, 0, 0, 'debounce'),
                                               (0x100000, 1, 1, 'whole-source replan')):
            with self.subTest(flag=flag):
                sample = p.control(flag)
                self.assertEqual(sample['return_code'], 0)
                self.assertEqual(sample['source_inhibit_byte130'], inhibit)
                self.assertEqual(sample['handshake_failure_byte132'], failure)
                self.assertEqual([c['mocked'] for c in sample['calls']], [action])
        before = p.control(0)
        after = p.control(0x80000000)
        self.assertEqual(after['source_inhibit_byte130'], before['source_inhibit_byte130'])
        self.assertEqual(after['handshake_failure_byte132'], before['handshake_failure_byte132'])
        self.assertEqual(after['calls'], [])

    def test_usb_classification_change_invalidates_old_budget(self):
        p = self.m.USBControl(self.data)
        s = self.m.d1946.SOURCE
        p.mu.mem_write(s + 0x106, b'\x07')
        p.w32(s + 0x160, 2100)
        p.w32(s + 0x168, 3)
        p.w32(s + 0x178, 4)
        sample = p.control(3)
        self.assertEqual(sample['profile_enum106'], 4)
        self.assertEqual(sample['usb_report_mA'], 100)
        self.assertEqual(sample['usb_type'], 0)
        self.assertEqual(sample['brick_index'], 0)
        self.assertEqual([c['reason'] for c in sample['calls'] if 'actual' in c], [1])
        self.assertEqual(p.sample_budget(4)['computed_budget_mA'], 0)

    def test_unchanged_usb_classification_does_not_reset(self):
        p = self.m.USBControl(self.data)
        p.mu.mem_write(self.m.d1946.SOURCE + 0x142, b'\x03')
        self.assertEqual(p.control(3)['calls'], [])


if __name__ == '__main__':
    unittest.main(verbosity=2)
