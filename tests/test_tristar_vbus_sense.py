"""Actual firmware measurement branches, fake RAM/providers, no hardware."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
CACHE = ROOT / 'build/firmware/kernelcache.12H321.macho'
try:
    import unicorn
except ModuleNotFoundError:
    unicorn = None
if unicorn:
    spec = importlib.util.spec_from_file_location('vbus_sense', ROOT / 'scripts/emulate-tristar-vbus-sense.py')
    sense = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(sense)


@unittest.skipUnless(unicorn is not None and CACHE.exists(), 'requires local verified firmware and Unicorn')
class TristarVBUSSense(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.data = CACHE.read_bytes()

    def test_independent_status_fixtures_and_actual_fallback_dispatch(self):
        for status, expected in ((0x00, 0), (0x01, 0), (0x02, 0), (0x03, 1),
                                 (0x80, 0), (0x81, 0), (0x82, 0), (0x83, 1),
                                 (0x04, 0), (0x07, 1), (0xFC, 0), (0xFF, 1)):
            with self.subTest(status=hex(status)):
                result = sense.experiment(self.data, status)
                self.assertEqual(result['presence_result'], expected)
                self.assertEqual(result['synthetic_i2c_reads'], [
                    {'register': 12, 'length': 1, 'synthetic_status': status, 'synthetic_result': 0}])
                self.assertEqual([x['address'] for x in result['actual_fallback_entries']],
                                 ['0x80884ea8', '0x80886ce4'])
                self.assertEqual(result['actual_fallback_entries'][1]['argument'], 1)
                self.assertEqual(result['synthetic_voltage_calls'], [])
                self.assertFalse(result['hardware_access'])
                self.assertFalse(result['power_source_verified'])

    def test_all_status_bytes_other_bits_cannot_substitute_for_low_pair(self):
        # Independent truth-set fixture; original instructions decide every
        # byte. No Python predicate stands in for the firmware consumer.
        true_bytes = set(range(3, 256, 4))
        for status in range(256):
            with self.subTest(status=hex(status)):
                self.assertEqual(sense.experiment(self.data, status)['presence_result'],
                                 1 if status in true_bytes else 0)

    def test_read_errors_do_not_publish_presence(self):
        for error in (1, 0xE00002C2, 0xFFFFFFFF):
            with self.subTest(error=hex(error)):
                result = sense.experiment(self.data, 0xFF, read_error=error)
                self.assertEqual(result['presence_result'], 0)
                self.assertEqual(len(result['synthetic_i2c_reads']), 1)
                self.assertEqual(result['synthetic_i2c_reads'][0]['synthetic_result'], error)

    def test_successful_optional_function_uses_signed_raw_threshold_without_i2c(self):
        for voltage, expected in ((0, 0), (0x3FFFF, 0), (0x40000, 1),
                                  (0x40001, 1), (0x7FFFFFFF, 1),
                                  (0x80000000, 0), (0xFFFFFFFF, 0)):
            with self.subTest(raw=hex(voltage)):
                result = sense.experiment(self.data, 0xFF, voltage=voltage)
                self.assertEqual(result['presence_result'], expected)
                self.assertEqual(result['synthetic_i2c_reads'], [])
                self.assertEqual(result['actual_fallback_entries'], [])
                self.assertEqual(result['synthetic_voltage_calls'], [
                    {'synthetic_raw_reply': voltage, 'synthetic_result': 0}])

    def test_optional_function_error_uses_actual_status_fallback(self):
        for status, expected in ((2, 0), (3, 1)):
            for error in (1, 0xE00002C2, 0xFFFFFFFF):
                with self.subTest(status=status, error=hex(error)):
                    result = sense.experiment(self.data, status, voltage=0x40000, voltage_error=error)
                    self.assertEqual(result['presence_result'], expected)
                    self.assertEqual(len(result['synthetic_voltage_calls']), 1)
                    self.assertEqual(len(result['synthetic_i2c_reads']), 1)
                    self.assertEqual(result['actual_fallback_entries'][1]['argument'], 1)

    def test_hash_and_synthetic_input_bounds_refused(self):
        with self.assertRaisesRegex(ValueError, 'SHA256 mismatch'):
            sense.experiment(b'unverified', 3)
        for status in (-1, 256, True, 3.0):
            with self.assertRaisesRegex(ValueError, 'THS status'):
                sense.experiment(self.data, status)
        for name in ('read_error', 'voltage', 'voltage_error'):
            for value in (-1, 0x100000000, True, 1.0):
                with self.subTest(argument=name, value=value):
                    with self.assertRaisesRegex(ValueError, 'integer'):
                        sense.experiment(self.data, 3, **{name: value})


if __name__ == '__main__':
    unittest.main()
