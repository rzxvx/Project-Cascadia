#!/usr/bin/env python3
"""Actual voltage callback and bounded arithmetic; optional original image."""
import importlib.util
import os
from pathlib import Path
import struct
import unittest

STAGE = Path(__file__).resolve().parents[1]
REPO = Path(os.environ.get('CASCADIA_RESEARCH_REPO', STAGE))
KC = Path(os.environ.get('CASCADIA_TEST_KERNELCACHE', REPO / 'build/firmware/kernelcache.12H321.macho'))
AVAILABLE = importlib.util.find_spec('unicorn') is not None and KC.is_file()


@unittest.skipUnless(AVAILABLE, 'requires Unicorn and hash-pinned local 12H321 kernelcache')
class UnitsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        spec = importlib.util.spec_from_file_location('units', STAGE / 'tools/power/check-voltage-units.py')
        cls.v = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.v)
        cls.data = cls.v.a.events.load_input(KC)

    def run_case(self, raw=3072, compensation=0, **kwargs):
        return self.v.Units(self.data).sample(raw, compensation, **kwargs)

    def test_exact_encoded_to_integer_boundaries(self):
        for raw in (0, 1, 2047, 2048, 3072, 4094, 4095):
            with self.subTest(raw=raw):
                out = self.run_case(raw)
                encoded = 163840 + 32 * raw
                self.assertEqual(out['encoded'], encoded)
                self.assertEqual(out['scaled64'], 1000 * encoded)
                self.assertEqual(out['stored_integer'], 1000 * encoded // 65536)

    def test_signed_compensation_and_truncation(self):
        for compensation in (-1000, -100, -15, -1, 0, 1, 20, 100, 1000):
            with self.subTest(compensation=compensation):
                out = self.run_case(4095, compensation)
                self.assertEqual(out['adjusted64'], 1000 * (163840 + 32 * 4095) + compensation * 65536)
                self.assertEqual(out['stored_integer'], 4499 + compensation)

    def test_raw_sample_is_not_stored_voltage(self):
        out = self.run_case(3072, 20)
        self.assertEqual(out['stored_integer'], 4020)
        self.assertNotEqual(out['stored_integer'], 3072)
        self.assertNotEqual(out['encoded'], out['stored_integer'])

    def test_exact_actual_dispatch_calls_and_no_io(self):
        out = self.run_case()
        self.assertEqual(out['calls'][0]['actual_callback'], '0x80ece491')
        self.assertEqual(out['calls'][1]['channel'], 4)
        self.assertEqual(out['transactions'], [])
        self.assertFalse(out['hardware_access'])

    def test_raw_error_restores_missing_sample_sentinel(self):
        out = self.run_case(error=0xe00002c0)
        self.assertEqual(out['encoded'], -2147483648)
        self.assertNotIn('stored_integer', out)
        self.assertEqual(out['transactions'], [])

    def test_voltage_limit_field_is_not_written(self):
        probe = self.v.Units(self.data)
        probe.w32(probe.OWNER + 0x24c, 4200)
        probe.sample(3072)
        self.assertEqual(struct.unpack('<I', probe.mu.mem_read(probe.OWNER + 0x24c, 4))[0], 4200)

    def test_guards_and_original_hash(self):
        for raw, compensation in ((True, 0), (4096, 0), (-1, 0), (0, True), (0, 1001)):
            with self.assertRaises(ValueError):
                self.run_case(raw, compensation)
        with self.assertRaises(ValueError):
            self.run_case(error=True)
        with self.assertRaises(ValueError):
            self.v.Units(b'wrong firmware image')

    def test_single_use(self):
        probe = self.v.Units(self.data)
        probe.sample(3072)
        with self.assertRaises(RuntimeError):
            probe.sample(3072)


if __name__ == '__main__':
    unittest.main()
