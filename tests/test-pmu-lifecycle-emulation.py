#!/usr/bin/env python3
"""Actual firmware lifecycle branches with explicitly mocked surrounding services."""
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
KC = Path(os.environ.get('CASCADIA_TEST_KERNELCACHE', ROOT / 'build/firmware/kernelcache.12H321.macho'))
AVAILABLE = importlib.util.find_spec('unicorn') is not None and KC.is_file()


@unittest.skipUnless(AVAILABLE, 'needs Unicorn and local p105 12H321 kernelcache')
class PMULifecycle(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        spec = importlib.util.spec_from_file_location('life', ROOT / 'tools/power/emulate-pmu-lifecycle.py')
        cls.m = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.m)
        cls.data = cls.m.load_input(KC)

    def test_wake_preserves_other_bits_and_runs_event_and_mask_transactions(self):
        for control in (0, 0x3f, 0x40, 0x80, 0xbf, 0xff):
            e = self.m.Lifecycle(self.data, control=control)
            e.registers[0x1c] = 0xa4
            out = e.dispatch('wake')
            self.assertEqual(out['return_code'], 0)
            self.assertEqual(e.registers[0x1c], 0xa5)
            self.assertEqual(e.registers[0x3f], control & 0x7f)
            self.assertEqual(out['transactions'][0], {'direction': 'read', 'register': '0x01',
                                                     'bytes': '000000000000000000'})
            self.assertEqual(out['transactions'][1]['register'], '0x12')
            self.assertEqual(len(out['transactions'][1]['bytes']), 18)

    def test_quiesce_sets_bit80_without_claiming_analog_meaning(self):
        for control in (0, 0x3f, 0x40, 0x80, 0xbf, 0xff):
            e = self.m.Lifecycle(self.data, control=control)
            e.dispatch('quiesce')
            self.assertEqual(e.registers[0x3f], control | 0x80)
            self.assertEqual(e.registers[0x1c], 0x12)
            self.assertIn({'direction': 'write', 'register': '0x12',
                           'bytes': '88ffffffffffaeffff'}, e.transactions)

    def test_sleep_is_a_distinct_dispatch_branch(self):
        e = self.m.Lifecycle(self.data)
        out = e.dispatch('sleep')
        self.assertEqual(out['return_code'], 0)
        self.assertEqual(out['transactions'], [])
        self.assertIn('mutex lock', [h['mocked'] for h in out['helpers']])
        self.assertEqual(e.registers[0x3f], 0xbf)

    def test_variant_field_changes_additional_transactions(self):
        e = self.m.Lifecycle(self.data, variant_field=0)
        e.registers[0xad] = 0
        out = e.dispatch('wake')
        self.assertIn('0xad', [t['register'] for t in out['transactions']])
        e = self.m.Lifecycle(self.data, variant_field=0)
        e.dispatch('quiesce')
        self.assertEqual(e.registers[0x1b], 0xa3)

    def test_failed_wake_read_is_not_safe_to_copy_into_linux(self):
        # With failed 3f read the Apple path uses the old stack byte left by
        # its 1c transaction, then writes 3f anyway. This is evidence of an
        # unchecked-error limitation, not a permitted Linux fallback policy.
        e = self.m.Lifecycle(self.data, fail_transfer=4)
        out = e.dispatch('wake')
        self.assertEqual(out['return_code'], 0)
        self.assertIn('error', out['transactions'][4])
        self.assertEqual(out['transactions'][4]['register'], '0x3f')
        self.assertEqual(out['transactions'][5], {'direction': 'write', 'register': '0x3f', 'bytes': '13'})
        self.assertNotEqual(e.registers[0x3f], 0xbf & 0x7f)

    def test_unverified_input_and_unknown_actions_are_refused(self):
        with tempfile.TemporaryDirectory() as td:
            bad = Path(td) / 'wrong'
            bad.write_bytes(b'wrong build')
            with self.assertRaises(ValueError):
                self.m.load_input(bad)
        with self.assertRaises(ValueError):
            self.m.Lifecycle(self.data).dispatch('charge')


if __name__ == '__main__':
    unittest.main(verbosity=2)
