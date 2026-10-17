#!/usr/bin/env python3
"""D1946 source callback and actual generic notification adapter tests."""
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
KC = Path(os.environ.get('CASCADIA_TEST_KERNELCACHE', ROOT / 'build/firmware/kernelcache.12H321.macho'))
AVAILABLE = importlib.util.find_spec('unicorn') is not None and KC.is_file()


@unittest.skipUnless(AVAILABLE, 'needs Unicorn and local p105 12H321 kernelcache')
class SourceEventTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        spec = importlib.util.spec_from_file_location('events', ROOT / 'tools/power/emulate-source-events.py')
        cls.m = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.m)
        cls.data = cls.m.load_input(KC)

    def fixture(self, current=500, **kwargs):
        e = self.m.SourceEvents(self.data, **kwargs)
        e.set_input_fixture(current)
        return e

    def test_input_setter_caches_actual_programmed_code_at_field_2a(self):
        for request, code in ((100, 2), (200, 10), (500, 34), (1000, 72)):
            e = self.m.SourceEvents(self.data)
            self.assertEqual(e.set_input_fixture(request), code)
            self.assertEqual(e.registers[0x3d], code)

    def test_registered_second_mask_dispatches_actual_notification_one(self):
        for event in (0x10, 0x610):
            e = self.fixture()
            out = e.event(event, bytes.fromhex('000100000000'))
            self.assertEqual(out['notifications'][0]['mask'], 1)
            self.assertEqual(out['transactions'], [])

    def test_first_mask_reads_status_and_detects_cached_vs_live_code_case(self):
        out = self.fixture().event(8, bytes.fromhex('000100000000'), live_input_code=2)
        self.assertEqual(out['notifications'][0]['mask'], 6)
        self.assertEqual(out['transactions'], [{'direction': 'read', 'register': '0x0a', 'bytes': '000100000000'},
                                               {'direction': 'read', 'register': '0x3d', 'bytes': '02'}])

    def test_each_condition_is_required_for_notification_six(self):
        for current, status, live in ((100, 1, 2), (500, 0, 2), (500, 2, 2), (500, 1, 3)):
            out = self.fixture(current).event(8, bytes((0, status, 0, 0, 0, 0)), live)
            self.assertEqual(out['notifications'][0]['mask'], 2)
            self.assertTrue(all(t['direction'] == 'read' for t in out['transactions']))

    def test_missing_owner_suppresses_generic_notification(self):
        e = self.fixture(owner_present=False)
        self.assertEqual(e.event(0x10)['notifications'], [])
        self.assertEqual(e.event(8)['notifications'], [])

    def test_failed_status_and_input_reads_still_generate_notification(self):
        for failing in (0, 1):
            e = self.fixture()
            # Explicit synthetic zero stack: failed reads must not be confused
            # with valid status. Apple's callback nevertheless emits mask 2.
            e.mu.mem_write(self.m.d.STACK, bytes(0x10000))
            e.fail_transfer = failing
            out = e.event(8, bytes.fromhex('000100000000'), live_input_code=2)
            self.assertIn('error', out['transactions'][failing])
            self.assertEqual(out['notifications'][0]['mask'], 2)
            self.assertTrue(all(t['direction'] == 'read' for t in out['transactions']))

    def test_unverified_images_and_invalid_events_are_refused(self):
        with tempfile.TemporaryDirectory() as td:
            bad = Path(td) / 'wrong'
            bad.write_bytes(b'wrong')
            with self.assertRaises(ValueError):
                self.m.load_input(bad)
        e = self.fixture()
        for invalid in (-1, 0x900):
            with self.assertRaises(ValueError):
                e.event(invalid)
        with self.assertRaises(ValueError):
            e.event(8, bytes(5))


if __name__ == '__main__':
    unittest.main(verbosity=2)
