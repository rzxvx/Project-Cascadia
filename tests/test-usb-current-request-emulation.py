#!/usr/bin/env python3
"""Actual firmware request/current instructions, with explicit offline mocks."""
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
KC = Path(os.environ.get('CASCADIA_TEST_KERNELCACHE', ROOT / 'build/firmware/kernelcache.12H321.macho'))
AVAILABLE = importlib.util.find_spec('unicorn') is not None and KC.is_file()


@unittest.skipUnless(AVAILABLE, 'needs Unicorn and local p105 12H321 kernelcache')
class USBCurrentRequest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        spec = importlib.util.spec_from_file_location('usb_current', ROOT / 'tools/power/emulate-usb-current-request.py')
        cls.m = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.m)
        cls.data = cls.m.load_input(KC)

    def test_host_driver_request_decodes_offsets_and_schedules_type_zero(self):
        e = self.m.USBCurrent(self.data)
        out = e.setup(2500, 2500)
        self.assertEqual(out['extra_offset_ma'], 2500)
        self.assertEqual(out['suspend_offset_ma'], 2500)
        self.assertEqual(out['events'], [{'mocked': 'schedule current worker', 'request_type': 0},
                                         {'mocked': 'USB request acknowledgement'}])
        self.assertFalse(out['hardware_access'])

    def test_both_vendor_request_directions_are_accepted_by_actual_receiver(self):
        for bm_request_type in (0x40, 0xc0):
            e = self.m.USBCurrent(self.data)
            self.assertEqual(e.setup(30, 40, request_type=bm_request_type)['extra_offset_ma'], 30)
            self.assertEqual(e.budget(0), 540)

    def test_signed_16_bit_offsets_are_preserved_including_negative_values(self):
        for extra, suspend in ((-32768, 32767), (32767, -32768), (-1, -600), (0, 0)):
            e = self.m.USBCurrent(self.data)
            out = e.setup(extra, suspend)
            self.assertEqual((out['extra_offset_ma'], out['suspend_offset_ma']), (extra, suspend))
            self.assertEqual(e.budget(0), max(0, 500 + suspend))
            self.assertEqual(e.budget(1), extra)

    def test_budget_types_and_positive_cap_use_actual_switch_instructions(self):
        for cap in (0, -1, 2100, 100):
            e = self.m.USBCurrent(self.data, base_ma=500, cap_ma=cap)
            e.setup(2500, 2500)
            summed = min(3000, cap) if cap > 0 else 3000
            extra = min(2500, cap) if cap > 0 else 2500
            self.assertEqual([e.budget(t) for t in range(7)],
                             [summed, extra, 500, 500, 500, summed, 0])

    def test_deferred_worker_delivers_computed_current_and_original_type(self):
        e = self.m.USBCurrent(self.data, cap_ma=2100)
        e.setup(2500, 2500)
        for kind, current in ((0, 2100), (1, 2100), (2, 500), (5, 2100), (6, 0)):
            events = e.worker(kind)
            self.assertEqual(events[0], {'mocked': 'ARMFunction provider delivery',
                                         'current_ma': current, 'request_type': kind})
            self.assertEqual([a['mocked'] for a in events[1:]],
                             ['provider release', 'controller release', 'task deallocation', 'thread cleanup'])

    def test_disabling_offset_updates_later_computation(self):
        e = self.m.USBCurrent(self.data)
        e.setup(2500, 2500)
        self.assertEqual(e.budget(0), 3000)
        e.setup(0, 0)
        self.assertEqual(e.budget(0), 500)
        self.assertEqual(e.worker()[0]['current_ma'], 500)

    def test_other_packets_delegate_without_mutating_current_offsets(self):
        for bm, request, length in ((0x40, 0x40, 1), (0xc0, 0x40, 1),
                                     (0x41, 0x40, 0), (0x40, 0x41, 0)):
            e = self.m.USBCurrent(self.data)
            e.setup(30, 40)
            out = e.setup(2500, 2500, request_type=bm, request=request, length=length)
            self.assertEqual(out['events'], [{'mocked': 'generic request delegation'}])
            self.assertEqual((out['extra_offset_ma'], out['suspend_offset_ma']), (30, 40))

    def test_unverified_images_and_unrepresentable_arguments_are_refused(self):
        with tempfile.TemporaryDirectory() as td:
            wrong = Path(td) / 'wrong'
            wrong.write_bytes(b'wrong')
            with self.assertRaises(ValueError):
                self.m.load_input(wrong)
        e = self.m.USBCurrent(self.data)
        for extra in (-32769, 32768):
            with self.assertRaises(ValueError):
                e.setup(extra, 0)
        with self.assertRaises(ValueError):
            e.budget(-1)
        with self.assertRaises(ValueError):
            e.worker(-1)


if __name__ == '__main__':
    unittest.main(verbosity=2)
