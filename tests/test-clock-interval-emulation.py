#!/usr/bin/env python3
"""Actual ARM clock conversion comparison; frequency/time remain fake."""
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
KC = Path(os.environ.get('CASCADIA_TEST_KERNELCACHE', ROOT / 'build/firmware/kernelcache.12H321.macho'))
AVAILABLE = importlib.util.find_spec('unicorn') is not None and KC.is_file()


@unittest.skipUnless(AVAILABLE, 'needs Unicorn and local p105 12H321 kernelcache')
class ClockIntervals(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        spec = importlib.util.spec_from_file_location('clock', ROOT / 'tools/power/emulate-clock-interval.py')
        cls.m = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.m)
        cls.data = cls.m.load_input(KC)

    def clock(self, frequency=24000000, now=123456789):
        return self.m.Clock(self.data, frequency, now)

    def test_original_24_fixtures_agree_across_three_actual_functions(self):
        cases = ((80, 1000000), (1000, 1000000), (1, 1), (1, 1000000000),
                 (0xffffffff, 1), (123456789, 7))
        for frequency in (1000000, 12000000, 24000000, 1000000000):
            clock = self.clock(frequency)
            for interval, scale in cases:
                out = clock.compare(interval, scale)
                expected = interval * scale * frequency // 1000000000
                self.assertEqual((out['interval_ticks'], out['nanosecond_ticks']), (expected, expected))
                self.assertEqual(out['deadline'], 123456789 + expected)
                self.assertFalse(out['hardware_access'])

    def test_adc_arguments_are_millisecond_intervals_in_fake_ticks(self):
        clock = self.clock(24000000, 0)
        self.assertEqual(clock.compare(80, 1000000)['interval_ticks'], 1920000)
        self.assertEqual(clock.compare(1000, 1000000)['interval_ticks'], 24000000)

    def test_nanosecond_conversion_at_second_and_64_bit_boundaries(self):
        clock = self.clock(1, 0)
        for n in (0, 1, 999999999, 1000000000, 1000000001, 0xffffffff, 1 << 32, (1 << 64) - 1):
            self.assertEqual(clock.convert_nanoseconds(n), n // 1000000000)

    def test_frequency_boundaries_and_sub_tick_truncation(self):
        for frequency in (1, 0xffffffff):
            clock = self.clock(frequency, 0)
            for n in (0, 1, 2, 999999999, 1000000000, 1000000001):
                self.assertEqual(clock.convert_nanoseconds(n), n * frequency // 1000000000)
        self.assertEqual(self.clock(1000000).convert_nanoseconds(999), 0)
        self.assertEqual(self.clock(1000000).convert_nanoseconds(1000), 1)

    def test_current_time_high_word_and_carry_are_preserved(self):
        for now in (0, 0xfffffffe, 0x123456789abcdef0):
            self.assertEqual(self.clock(1000000000, now).compare(5, 1)['deadline'], now + 5)

    def test_zero_intervals_and_maximum_32_bit_product_are_bounded(self):
        for interval, scale in ((0, 0), (0, 0xffffffff), (0xffffffff, 0), (0xffffffff, 0xffffffff)):
            out = self.clock(1000000, 0).compare(interval, scale)
            self.assertEqual(out['scaled_nanoseconds'], interval * scale)
            self.assertEqual(out['deadline'], interval * scale // 1000)

    def test_overflowing_tick_and_deadline_fixtures_are_refused(self):
        clock = self.clock(0xffffffff)
        with self.assertRaisesRegex(ValueError, 'overflow 64-bit absolute ticks'):
            clock.convert_nanoseconds((1 << 64) - 1)
        with self.assertRaisesRegex(ValueError, 'overflow 64-bit absolute ticks'):
            clock.compare(0xffffffff, 0xffffffff)
        clock = self.clock(1000000000, (1 << 64) - 1)
        with self.assertRaisesRegex(ValueError, 'deadline would overflow'):
            clock.compare(1, 1)
        self.assertEqual(clock.compare(0, 1)['deadline'], (1 << 64) - 1)

    def test_ranges_and_boolean_inputs_are_rejected_before_execution(self):
        for frequency in (0, -1, 1 << 32, True, 1.5, '1000000'):
            with self.assertRaises(ValueError):
                self.clock(frequency)
        for now in (-1, 1 << 64, True, 1.5, '0'):
            with self.assertRaises(ValueError):
                self.clock(now=now)
        clock = self.clock()
        for interval, scale in ((-1, 1), (1 << 32, 1), (1, -1), (1, 1 << 32), (True, 1), (1, False)):
            with self.assertRaises(ValueError):
                clock.compare(interval, scale)
        for n in (-1, 1 << 64, True, 1.5):
            with self.assertRaises(ValueError):
                clock.convert_nanoseconds(n)

    def test_hash_mismatch_is_refused_for_loader_and_constructor(self):
        with self.assertRaisesRegex(ValueError, 'hash mismatch'):
            self.m.Clock(b'not the pinned image')
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'bad-kernelcache'
            path.write_bytes(b'not the pinned image')
            with self.assertRaisesRegex(ValueError, 'hash mismatch'):
                self.m.load_input(path)

    def test_unknown_entry_and_instruction_budget_fail_closed(self):
        clock = self.clock()
        with self.assertRaisesRegex(ValueError, 'unknown bounded clock entry'):
            clock._run('invented', 0, 0)
        old = self.m.INSTRUCTION_BUDGET
        try:
            self.m.INSTRUCTION_BUDGET = 1
            with self.assertRaisesRegex(RuntimeError, 'instruction budget exhausted'):
                clock.compare(80, 1000000)
        finally:
            self.m.INSTRUCTION_BUDGET = old


if __name__ == '__main__':
    unittest.main()
