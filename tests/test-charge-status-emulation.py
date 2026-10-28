#!/usr/bin/env python3
"""Actual status consumer with fake reads and blocked recovery callbacks."""
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
KC = Path(os.environ.get('CASCADIA_TEST_KERNELCACHE', ROOT / 'build/firmware/kernelcache.12H321.macho'))
AVAILABLE = importlib.util.find_spec('unicorn') is not None and KC.is_file()


@unittest.skipUnless(AVAILABLE, 'needs Unicorn and local p105 12H321 kernelcache')
class ChargeStatus(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        spec = importlib.util.spec_from_file_location('status', ROOT / 'tools/power/emulate-charge-status.py')
        cls.m = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.m)
        cls.data = cls.m.events.load_input(KC)

    def recovery(self, out):
        return [c['mocked'] for c in out['calls'] if c['mocked'].startswith('backend')]

    def test_status_bits_aggregate_and_temperature_reason(self):
        for bit, aggregate, reason in ((0, 0, None), (2, 1, None), (4, 1, None),
                                       (8, 0, None), (16, 0, 'HighOrLowTemperature'),
                                       (32, 0, None), (0xff, 1, 'HighOrLowTemperature')):
            out = self.m.Status(self.data).consume(bit, 1000)
            self.assertEqual(out['aggregate_boolean'], aggregate)
            self.assertEqual(out['reason'], reason)
            self.assertEqual([c['direction'] for c in out['transactions']], ['read'])
            self.assertFalse(out['hardware_access'])
        out = self.m.Status(self.data).consume(16, 1000, prior_reason=123)
        self.assertEqual(out['reason'], 123)

    def test_external_cached_flag_and_clock_failure_block_recovery(self):
        for kwargs in ({'external': False}, {'cached_flag': True}, {'clock_error': 1}):
            out = self.m.Status(self.data).consume(32, 1000, first_timeout=699, **kwargs)
            self.assertEqual(self.recovery(out), [])
        out = self.m.Status(self.data).consume(32, 0, first_timeout=699)
        self.assertEqual(self.recovery(out), [])

    def test_timeout_first_observation_and_strict_300_boundary(self):
        out = self.m.Status(self.data).consume(32, 1000)
        self.assertEqual(out['first_timeout'], 1000)
        self.assertEqual(self.recovery(out), [])
        for now, count in ((1299, 0), (1300, 0), (1301, 1)):
            out = self.m.Status(self.data).consume(32, now, first_timeout=1000)
            self.assertEqual(len(self.recovery(out)), count)
            self.assertEqual(out['first_timeout'], 0 if count else 1000)

    def test_capacity_90_branch_bypasses_later_inhibit_and_attachment_gates(self):
        # This is evidence about the old driver's policy, not an endorsement
        # of bypassing faults or source limits in a future Linux charger.
        out = self.m.Status(self.data).consume(32, 1000, current_capacity=89,
                    first_timeout=699, attached_at=0, inhibit=True)
        self.assertEqual(self.recovery(out), ['backend slot +0x74 recovery blocked'])
        for capacity in (90, 94, 95, 100):
            out = self.m.Status(self.data).consume(32, 1000, current_capacity=capacity,
                        first_timeout=699, attached_at=0, inhibit=True)
            self.assertEqual(self.recovery(out), [])

    def test_end_capacity_inhibit_and_attachment_deadline_boundaries(self):
        for capacity, count in ((89, 1), (90, 1), (94, 1), (95, 0), (100, 0)):
            out = self.m.Status(self.data).consume(8, 1000, current_capacity=capacity)
            self.assertEqual(len(self.recovery(out)), count)
        for kwargs in ({'inhibit': True}, {'attached_at': 0}):
            self.assertEqual(self.recovery(self.m.Status(self.data).consume(8, 1000, **kwargs)), [])
        for now, count in ((54999, 1), (55000, 0), (55001, 0)):
            out = self.m.Status(self.data).consume(8, now, attached_at=1000)
            self.assertEqual(len(self.recovery(out)), count)

    def test_timeout_has_priority_over_end_when_both_are_present(self):
        out = self.m.Status(self.data).consume(40, 1000, first_timeout=699)
        self.assertEqual(self.recovery(out), ['backend slot +0x74 recovery blocked'])
        self.assertEqual(out['first_timeout'], 0)

    def test_failed_read_is_not_a_healthy_fault_sample(self):
        out = self.m.Status(self.data, fail_transfer=0).consume(0xff, 1000)
        self.assertEqual(out['aggregate_boolean'], 0)
        self.assertIsNone(out['reason'])
        self.assertEqual(self.recovery(out), [])
        self.assertIn('error', out['transactions'][0])

    def test_actual_clock_getter_returns_stable_raw_pmu_counter(self):
        for counter in (0, 1, 300, 0xffff, 0x10000, 0x80000000, 0xffffffff):
            out = self.m.Status(self.data).clock_counter(counter)
            self.assertEqual(out['returned_counter'], counter)
            self.assertEqual(len(out['transactions']), 2)
            self.assertEqual([r['register'] for r in out['transactions']], ['0x5d', '0x5d'])
            self.assertEqual([r['direction'] for r in out['transactions']], ['read', 'read'])
            self.assertFalse(out['hardware_access'])

    def test_invalid_ranges_and_unpinned_input_are_refused(self):
        e = self.m.Status(self.data)
        for kwargs in ({'status_byte': 256, 'now': 1}, {'status_byte': 0, 'now': -1},
                       {'status_byte': 0, 'now': 1, 'maximum_capacity': 0},
                       {'status_byte': 0, 'now': 1, 'current_capacity': 10001}):
            with self.assertRaises(ValueError):
                e.consume(**kwargs)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'bad-kernelcache'
            path.write_bytes(b'not the pinned Apple image')
            with self.assertRaisesRegex(ValueError, 'hash mismatch'):
                self.m.events.load_input(path)


if __name__ == '__main__':
    unittest.main()
