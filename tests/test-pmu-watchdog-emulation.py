#!/usr/bin/env python3
"""Actual system-watchdog paths with fake PMU/GPIO/provider fixtures."""
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
KC = Path(os.environ.get('CASCADIA_TEST_KERNELCACHE', ROOT / 'build/firmware/kernelcache.12H321.macho'))
ADT = Path(os.environ.get('CASCADIA_TEST_ADT', ROOT / 'build/usb-host-research/firmware/DeviceTree.p105ap.raw'))
AVAILABLE = importlib.util.find_spec('unicorn') is not None and KC.is_file() and ADT.is_file()


@unittest.skipUnless(AVAILABLE, 'needs Unicorn and local pinned kernelcache/ADT')
class Watchdog(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        spec = importlib.util.spec_from_file_location('watchdog', ROOT / 'tools/power/emulate-pmu-watchdog.py')
        cls.m = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.m)
        cls.data = cls.m.load_input(KC)

    def fixture(self, **kwargs):
        return self.m.Watchdog(self.data, **kwargs)

    def test_raw_board_property_gate_is_separate_from_keepact_presence(self):
        out = self.m.board_evidence(ADT)
        self.assertEqual(out['pmu_property_count'], 34)
        self.assertFalse(out['pmu_watchdog_present_in_raw_adt'])
        self.assertEqual(out['function_keepact_raw_hex'], '0000000f4f4950470402000001010000')
        self.assertFalse(out['hardware_access'])

    def test_actual_start_refuses_missing_pmu_or_property_without_io(self):
        for provider, prop, expected_calls in ((False, True, ['PMU metacast']),
                (True, False, ['PMU metacast', 'PMU getProvider', 'provider property lookup'])):
            out = self.fixture().start(provider_present=provider, property_present=prop)
            self.assertEqual(out['r0_after_return'], 0)
            self.assertEqual(out['transactions'], [])
            self.assertEqual([c['mocked'] for c in out['calls']], expected_calls)

    def test_start_with_property_reaches_only_mocked_superclass(self):
        for base_result in (False, True):
            out = self.fixture().start(property_present=True, base_result=base_result)
            self.assertEqual(out['r0_after_return'], int(base_result))
            self.assertEqual(out['transactions'], [])
            self.assertEqual([c['mocked'] for c in out['calls']], ['PMU metacast', 'PMU getProvider',
                'provider property lookup', 'PMU getProvider', 'keepact function lookup',
                'generic watchdog superclass start'])

    def test_missing_keepact_logs_and_still_delegates_to_mocked_superclass(self):
        out = self.fixture().start(property_present=True, keepact_found=False)
        self.assertEqual(out['r0_after_return'], 1)
        self.assertEqual(out['transactions'], [])
        self.assertIn('logging', [c['mocked'] for c in out['calls']])
        self.assertEqual(out['calls'][-1]['mocked'], 'generic watchdog superclass start')

    def test_enable_disable_and_repeated_requests_execute_actual_control(self):
        fixture = self.fixture()
        out = fixture.control(True)
        self.assertEqual((out['enabled_cache'], out['fake_keepact_level'], out['fake_register_80']), (1, 1, 0x2f))
        self.assertEqual(out['transactions'], [{'direction': 'write', 'register': '0x80', 'bytes': '2f'}])
        self.assertEqual(out['calls'], [{'mocked': 'keepact ARMFunction', 'fake_level': 1}])
        repeated = fixture.control(True)
        for key in ('enabled_cache', 'fake_keepact_level', 'fake_register_80', 'calls', 'transactions'):
            self.assertEqual(repeated[key], out[key])
        disabled = fixture.control(False)
        self.assertEqual((disabled['enabled_cache'], disabled['fake_register_80']), (0, 0x0e))
        self.assertEqual(disabled['transactions'][-1], {'direction': 'write', 'register': '0x80', 'bytes': '0e'})
        repeated = fixture.control(False)
        for key in ('enabled_cache', 'fake_keepact_level', 'fake_register_80', 'calls', 'transactions'):
            self.assertEqual(repeated[key], disabled[key])

    def test_no_keepact_means_no_control_or_kick_transactions(self):
        for enabled in (False, True):
            fixture = self.fixture(keepact=False, enabled=enabled)
            out = fixture.control(not enabled)
            self.assertEqual(out['enabled_cache'], int(enabled))
            self.assertEqual(out['calls'], [])
            self.assertEqual(out['transactions'], [])
            self.assertEqual(fixture.kick()['fake_keepact_level'], 0)

    def test_kick_toggles_synthetic_level_without_pmu_io_or_enable_gate(self):
        fixture = self.fixture(enabled=False)
        self.assertEqual(fixture.kick()['fake_keepact_level'], 1)
        out = fixture.kick()
        self.assertEqual(out['fake_keepact_level'], 0)
        self.assertEqual([c['fake_level'] for c in out['calls']], [1, 0])
        self.assertEqual(out['enabled_cache'], 0)
        self.assertEqual(out['transactions'], [])

    def test_failed_control_write_or_gpio_does_not_roll_back_software_cache(self):
        out = self.fixture(fail_transfer=0).control(True)
        self.assertEqual((out['enabled_cache'], out['fake_keepact_level'], out['fake_register_80']), (1, 1, 0))
        self.assertIn('error', out['transactions'][0])
        out = self.fixture(enabled=True, fail_transfer=0).control(False)
        self.assertEqual((out['enabled_cache'], out['fake_register_80']), (0, 0))
        self.assertIn('error', out['transactions'][0])
        out = self.fixture(gpio_error=0xe00002c0).control(True)
        self.assertEqual((out['enabled_cache'], out['fake_keepact_level']), (1, 1))
        self.assertEqual(out['calls'][-1]['mocked'], 'keepact ARMFunction')

    def test_interval_arguments_use_500_and_one_million_without_device_clock(self):
        out = self.fixture().interval()
        self.assertEqual(out['calls'], [{'mocked': 'interval conversion', 'interval': 500, 'scale': 1000000}])
        self.assertEqual(out['transactions'], [])

    def test_hash_and_fixture_type_guards(self):
        for kwargs in ({'keepact': 1}, {'enabled': 0}, {'level': True}, {'level': 2}, {'gpio_error': -1}):
            with self.assertRaises(ValueError):
                self.fixture(**kwargs)
        with self.assertRaises(ValueError):
            self.fixture().control(1)
        with self.assertRaises(ValueError):
            self.fixture().start(property_present=1)
        with self.assertRaisesRegex(ValueError, 'hash mismatch'):
            self.m.Watchdog(b'not the pinned image')
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'bad-input'
            path.write_bytes(b'not the pinned image')
            for loader in (self.m.load_input, self.m.board_evidence):
                with self.assertRaisesRegex(ValueError, 'hash mismatch'):
                    loader(path)


if __name__ == '__main__':
    unittest.main()
