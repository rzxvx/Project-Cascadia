#!/usr/bin/env python3
"""Actual ADC metadata/scalers with a blocked raw conversion provider."""
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
KC = Path(os.environ.get('CASCADIA_TEST_KERNELCACHE', ROOT / 'build/firmware/kernelcache.12H321.macho'))
AVAILABLE = importlib.util.find_spec('unicorn') is not None and KC.is_file()


@unittest.skipUnless(AVAILABLE, 'needs Unicorn and local p105 12H321 kernelcache')
class D1946ADC(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        spec = importlib.util.spec_from_file_location('adc', ROOT / 'tools/power/emulate-d1946-adc.py')
        cls.m = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.m)
        cls.data = cls.m.events.load_input(KC)

    def test_actual_metadata_separates_two_vbus_channels_and_charger_temperature(self):
        for channel, name, metadata in ((0, 'vcc_main', 'Vc0'), (4, 'vbat', 'VB0'),
                (15, 'vbus', 'Vu0'), (27, 'vbus_b', 'Vu1'), (21, 'chg_temp', 'Tg0')):
            out = self.m.ADC(self.data).name(channel)
            self.assertEqual((out['name'], out['metadata'], out['return_code']), (name, metadata, 0))
            self.assertFalse(out['hardware_access'])

    def test_temperature_channels_keep_distinct_names(self):
        expected = {18: 'buck5_temp', 19: 'ldo9_temp', 20: 'cbuck_b_temp',
                    22: 'buck0_temp', 23: 'buck2_temp', 24: 'buck3_temp',
                    25: 'buck4_temp', 26: 'cbuck_a_temp'}
        for channel, name in expected.items():
            self.assertEqual(self.m.ADC(self.data).name(channel)['name'], name)

    def test_vbus_scalers_execute_same_linear_transform_at_12_bit_boundaries(self):
        for raw in (0, 1, 2047, 2048, 4094, 4095):
            for channel in (15, 27):
                out = self.m.ADC(self.data).scale(channel, raw)
                self.assertEqual(out['scaled_integer'], 88 * raw)
                self.assertEqual(out['return_code'], 0)
                self.assertEqual(out['transactions'], [])
                self.assertEqual(out['calls'][0]['channel'], channel)

    def test_battery_main_voltage_scaler_preserves_offset(self):
        for raw in (0, 1, 2048, 4095):
            for channel in (0, 4):
                out = self.m.ADC(self.data).scale(channel, raw)
                self.assertEqual(out['scaled_integer'], 163840 + 32 * raw)

    def test_charger_temperature_is_signed_and_not_a_battery_temperature_measurement(self):
        for raw in (0, 2048, 2366, 2367, 4095):
            numerator = (23662 - 10 * raw) * 17290 * 65536
            truncated = (abs(numerator) // 1000000) * (-1 if numerator < 0 else 1)
            out = self.m.ADC(self.data).scale(21, raw)
            self.assertEqual(out['scaled_integer'], 26 * 65536 + truncated)
            self.assertEqual(out['transactions'], [])

    def test_raw_errors_propagate_and_do_not_publish_output(self):
        for channel in (0, 4, 15, 21, 27):
            out = self.m.ADC(self.data).scale(channel, 2048, raw_error=0xe00002c0)
            self.assertEqual(out['return_code'], 0xe00002c0)
            self.assertTrue(out['output_untouched'])
            self.assertEqual(out['transactions'], [])

    def test_input_guards_refuse_unsupported_channels_raw_ranges_and_images(self):
        a = self.m.ADC(self.data)
        for channel, raw in ((1, 0), (15, -1), (15, 4096)):
            with self.assertRaises(ValueError):
                a.scale(channel, raw)
        with self.assertRaises(ValueError):
            a.name(7)  # needs additional property mocks beyond this bounded fixture
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'bad-kernelcache'
            path.write_bytes(b'not the pinned Apple image')
            with self.assertRaisesRegex(ValueError, 'hash mismatch'):
                self.m.events.load_input(path)


if __name__ == '__main__':
    unittest.main()
