#!/usr/bin/env python3
"""Actual source backoff consumer/cap bookkeeping and isolated deadline block."""
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
KC = Path(os.environ.get('CASCADIA_TEST_KERNELCACHE', ROOT / 'build/firmware/kernelcache.12H321.macho'))
AVAILABLE = importlib.util.find_spec('unicorn') is not None and KC.is_file()


@unittest.skipUnless(AVAILABLE, 'needs Unicorn and local p105 12H321 kernelcache')
class SourceBackoff(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        spec = importlib.util.spec_from_file_location('backoff', ROOT / 'tools/power/emulate-source-backoff.py')
        cls.m = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.m)
        cls.data = cls.m.events.load_input(KC)

    def cap_candidates(self, out):
        return [c['candidate_ma'] for c in out['calls'] if c.get('actual') == 'cap helper']

    def test_actual_current_threshold_and_quantized_boundaries(self):
        for previous, expected in ((0, None), (100, None), (101, 0), (199, 0),
                                    (200, 100), (275, 100), (300, 200), (501, 400),
                                    (999, 888), (1000, 900), (2100, 2000)):
            out = self.m.Backoff(self.data).consume(6, previous, 100, 200)
            self.assertEqual(self.cap_candidates(out), [] if expected is None else [expected])
            self.assertEqual(out['stored_cap_ma'], expected or 0)
            self.assertEqual(out['transactions'], [])

    def test_deadline_strictly_excludes_equality_and_later_time(self):
        for now, expected in ((199, [400]), (200, []), (201, [])):
            out = self.m.Backoff(self.data).consume(6, 500, now, 200)
            self.assertEqual(self.cap_candidates(out), expected)

    def test_unsigned_64_bit_time_comparison_handles_high_word_boundaries(self):
        deadline = 0x100000000
        for now, expected in ((deadline - 1, [400]), (deadline, []), (deadline + 1, [])):
            out = self.m.Backoff(self.data).consume(6, 500, now, deadline)
            self.assertEqual(self.cap_candidates(out), expected)
        out = self.m.Backoff(self.data).consume(6, 500, 0xffffffffffffffff, 1)
        self.assertEqual(self.cap_candidates(out), [])

    def test_inactive_owner_and_empty_pending_mask_do_not_adjust_or_debounce(self):
        for mask, power in ((6, 0), (0, 1)):
            out = self.m.Backoff(self.data).consume(mask, 500, 100, 200, power_state=power)
            self.assertEqual(self.cap_candidates(out), [])
            self.assertNotIn('debounce scheduling', [a.get('mocked') for a in out['calls']])

    def test_independent_notification_bits_route_to_correct_consumers(self):
        for mask, cap, debounce in ((1, [], [0]), (2, [], [1]), (4, [400], []), (6, [400], [1])):
            out = self.m.Backoff(self.data).consume(mask, 500, 100, 200)
            self.assertEqual(self.cap_candidates(out), cap)
            self.assertEqual([c['argument'] for c in out['calls'] if c.get('mocked') == 'debounce scheduling'], debounce)

    def test_actual_cap_bookkeeping_and_timer_are_conditional_on_change(self):
        for prior, timer_count in ((0, 1), (400, 0)):
            out = self.m.Backoff(self.data).consume(6, 500, 100, 200, prior_cap_ma=prior)
            self.assertEqual(out['stored_cap_ma'], 400)
            timers = [c for c in out['calls'] if c.get('mocked') == 'cap timer delivery']
            self.assertEqual(len(timers), timer_count)
            if timers:
                self.assertEqual((timers[0]['interval'], timers[0]['scale']), (300, 1000000000))
        # A candidate zero can clear a prior cap. Its eventual charger effect
        # is not modeled, so it must not be mislabeled a safe 100 mA floor.
        out = self.m.Backoff(self.data).consume(6, 101, 100, 200, prior_cap_ma=400)
        self.assertEqual(out['stored_cap_ma'], 0)

    def test_actual_deadline_block_stores_fields_and_adds_clock_interval_with_carry(self):
        for now, interval in ((100, 25), (0xfffffffe, 500000000),
                               (0xffffffffffffffff, 1), (0x100000001, 0)):
            out = self.m.Backoff(self.data).deadline(900, -100, now, interval)
            self.assertEqual(out['stored_current_ma'], 900)
            self.assertEqual(out['stored_delta_ma'], -100)
            self.assertEqual(out['stored_time_ticks'], now)
            self.assertEqual(out['stored_deadline_ticks'], (now + interval) & 0xffffffffffffffff)
            self.assertEqual(out['calls'][1], {'mocked': 'clock_interval_to_absolutetime_interval',
                                              'interval': 500, 'scale': 1000000, 'fixture_ticks': interval})
            self.assertFalse(out['hardware_access'])

    def test_invalid_ranges_and_images_are_refused(self):
        e = self.m.Backoff(self.data)
        for args in ((8, 500, 1, 2), (6, -1, 1, 2), (6, 500, -1, 2), (6, 500, 1, 1 << 64)):
            with self.assertRaises(ValueError):
                e.consume(*args)
        with self.assertRaises(ValueError):
            e.deadline(900, 10001, 1, 2)
        with self.assertRaises(ValueError):
            e.deadline(900, -100, 1, -1)
        with tempfile.TemporaryDirectory() as td:
            wrong = Path(td) / 'wrong'
            wrong.write_bytes(b'wrong firmware')
            with self.assertRaises(ValueError):
                self.m.events.load_input(wrong)


if __name__ == '__main__':
    unittest.main(verbosity=2)
