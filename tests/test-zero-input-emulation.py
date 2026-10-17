#!/usr/bin/env python3
"""Actual firmware zero-input branches, fake PMU; no connected device."""
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]
KC = Path(os.environ.get('CASCADIA_TEST_KERNELCACHE', ROOT / 'build/firmware/kernelcache.12H321.macho'))
AVAILABLE = importlib.util.find_spec('unicorn') is not None and KC.is_file()
z = None


@unittest.skipUnless(AVAILABLE, 'needs Unicorn and local pinned kernelcache')
class ActualZeroInput(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        global z
        spec = importlib.util.spec_from_file_location('zero', ROOT / 'tools/power/emulate-zero-input.py')
        z = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(z)
        cls.data = z.events.load_input(KC)

    def run_fixture(self, old=500, desired=0, failed=()):
        return z.ZeroInput(self.data, failed).apply(old, desired)

    def test_success_calls_actual_zero_setter_twice(self):
        out = self.run_fixture()
        self.assertEqual([c['requested_ma'] for c in out['calls'] if c.get('actual') == 'D1946 input setter'], [0, 0])
        self.assertEqual([(t['direction'], t['register']) for t in out['transactions']],
                         [('read', '0x3d'), ('read', '0x1b'), ('write', '0x1b'), ('write', '0x3d')] * 2)
        self.assertEqual(out['stored_current_ma'], 0)
        self.assertEqual(out['stored_delta_ma'], -500)
        self.assertEqual((out['stored_now_ticks'], out['stored_deadline_ticks']), (100, 600))
        self.assertEqual(out['final_fake_registers'], {'0x1b': 0xa5, '0x1c': 0x93, '0x3d': 0})
        self.assertFalse(out['hardware_access'])

    def test_cached_zero_skips_io_and_postapply_callback(self):
        out = self.run_fixture(0)
        self.assertEqual(out['transactions'], [])
        self.assertFalse(any(c.get('actual') == 'D1946 input setter' for c in out['calls']))
        self.assertEqual(out['final_fake_registers']['0x3d'], 0x54)

    def test_nonzero_effective_budgets_require_separate_timer_context(self):
        for desired in (1, 74, 75, 99, 100):
            with self.subTest(desired=desired), self.assertRaises(ValueError):
                self.run_fixture(500, desired)

    def test_previous_current_boundaries(self):
        for old in (1, 99, 100, 101, 2100, 3262):
            with self.subTest(old=old):
                out = self.run_fixture(old)
                self.assertEqual(out['stored_current_ma'], 0)
                self.assertEqual(out['stored_delta_ma'], -old)

    def test_first_register_write_failure_ignored_and_second_retries(self):
        out = self.run_fixture(failed=(3,))
        returns = [c['return_code'] for c in out['calls'] if c.get('actual') == 'apply setter return']
        self.assertEqual(returns, [0xe00002c0, 0])
        self.assertEqual(out['stored_current_ma'], 0)
        self.assertEqual(out['final_fake_registers']['0x3d'], 0)

    def test_second_register_write_failure_preserves_generic_current(self):
        out = self.run_fixture(failed=(7,))
        self.assertEqual(out['stored_current_ma'], 500)
        self.assertEqual(out['stored_deadline_ticks'], 0)
        self.assertEqual(out['final_fake_registers']['0x3d'], 0)  # first write succeeded
        self.assertTrue(any(c.get('mocked') == 'logging' for c in out['calls']))
        self.assertFalse(any(c.get('mocked') == 'source virtual slot +0x50' for c in out['calls']))

    def test_both_zero_writes_fail_not_safe_zero_proof(self):
        out = self.run_fixture(failed=(3, 7))
        self.assertEqual(out['stored_current_ma'], 500)
        self.assertEqual(out['final_fake_registers']['0x3d'], 0x54)
        self.assertEqual(out['source_cached_input_code'], 0)
        self.assertEqual(sum('error' in t for t in out['transactions']), 2)

    def test_every_single_transfer_failure_is_bounded(self):
        for ordinal in range(8):
            with self.subTest(ordinal=ordinal):
                out = self.run_fixture(failed=(ordinal,))
                self.assertEqual(sum('error' in t for t in out['transactions']), 1)
                self.assertEqual(len(out['transactions']), 8)

    def test_input_guards_and_single_use(self):
        for old, desired in ((True, 0), (-1, 0), (3263, 0), (1, True), (1, -1), (1, 100)):
            with self.assertRaises(ValueError): self.run_fixture(old, desired)
        for failures in ((True,), (-1,), (8,), '3'):
            with self.assertRaises(ValueError): z.ZeroInput(self.data, failures)
        probe = z.ZeroInput(self.data)
        probe.apply(500)
        with self.assertRaises(RuntimeError): probe.apply(500)

    def test_hash_guard(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'wrong.macho'
            path.write_bytes(b'untrusted image')
            with self.assertRaises(ValueError): z.events.load_input(path)


if __name__ == '__main__': unittest.main()
