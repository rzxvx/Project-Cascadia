#!/usr/bin/env python3
"""Optional offline firmware tests; requires local 12H321 cache and Unicorn."""
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
KC = Path(os.environ.get('CASCADIA_TEST_KERNELCACHE',
                         ROOT / 'build/firmware/kernelcache.12H321.macho'))
AVAILABLE = importlib.util.find_spec('unicorn') is not None and KC.is_file()


@unittest.skipUnless(AVAILABLE, 'needs unicorn and the locally extracted p105 12H321 kernelcache')
class D1946Firmware(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        spec = importlib.util.spec_from_file_location('d1946', ROOT / 'tools/power/emulate-d1946.py')
        cls.m = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.m)
        cls.data = KC.read_bytes()
        if cls.m.hashlib.sha256(cls.data).hexdigest() != cls.m.EXPECTED_SHA256:
            raise RuntimeError('wrong local firmware hash; refusing fixed addresses')

    def test_charge_programming_preserves_thermal_limits_and_control_bits(self):
        for current, code in ((40, 1), (500, 12), (1000, 25), (2520, 63), (5000, 63)):
            with self.subTest(current=current):
                e = self.m.Experiment(self.data)
                self.assertEqual(e.call(0x80ed0768, 1, current, 0), 0)
                self.assertEqual([e.registers[r] for r in (0x4b, 0x4d, 0x4f, 0x51, 0x53)],
                                 [0, 0, code, code, code])
                self.assertEqual(e.registers[0x3f], 0xbf)

    def test_charge_zero_is_separate_from_nonzero_small_request(self):
        for current, control in ((0, 0xff), (1, 0xbf), (39, 0xbf)):
            with self.subTest(current=current):
                e = self.m.Experiment(self.data)
                self.assertEqual(e.call(0x80ed0768, 1, current, 0), 0)
                self.assertEqual(e.registers[0x3f], control)

    def test_hardware_ceiling_and_thermal_limit_are_both_applied(self):
        e = self.m.Experiment(self.data, initial_charge_control=0x9f,
                             thermal_limits=(0, 0, 10, 63, 20))
        self.assertEqual(e.call(0x80ed0768, 1, 2520, 0), 0)
        self.assertEqual([e.registers[r] for r in (0x4b, 0x4d, 0x4f, 0x51, 0x53)],
                         [0, 0, 10, 31, 20])
        self.assertEqual(e.registers[0x3f], 0x9f)

    def test_input_low_current_mode_and_unrelated_bits(self):
        for current, low in ((0, True), (74, True), (75, False), (100, False)):
            with self.subTest(current=current):
                e = self.m.Experiment(self.data)
                self.assertEqual(e.call(0x80ed086c, current, 0, 0), 0)
                self.assertEqual(e.registers[0x1b], 0xa5 if low else 0xa1)
                self.assertEqual(e.registers[0x1c], 0x93 if low else 0x13)

    def test_input_no_calibration_compensation(self):
        for current, code, computed in ((100, 2, 100), (500, 34, 500),
                                         (1000, 72, 975), (1500, 110, 1450),
                                         (2100, 158, 2050)):
            with self.subTest(current=current):
                e = self.m.Experiment(self.data)
                self.assertEqual(e.call(0x80ed086c, current, 0, 0), 0)
                self.assertEqual(e.registers[0x3d], code)
                self.assertEqual(e.call(0x80ed0a98, code), computed)

    def test_status_reason_and_boolean_masks_execute_actual_firmware(self):
        for mask in (0, 1, 2, 4, 8, 16, 32, 64, 128, 255):
            with self.subTest(mask=mask):
                sample = self.m.Experiment(self.data).status((0, mask, 0, 0, 0, 0))
                self.assertEqual(sample['return_code'], 0)
                self.assertEqual(sample['arg1_boolean'], bool(mask & 6))
                self.assertEqual(sample['arg2_boolean'], bool(mask & 8))
                self.assertEqual(sample['arg3_boolean'], bool(mask & 32))
                self.assertEqual(sample['reason'], 'HighOrLowTemperature' if mask & 16 else None)
                self.assertEqual(sample['transactions'],
                                 [{'direction': 'read', 'register': '0x0a',
                                   'bytes': bytes((0, mask, 0, 0, 0, 0)).hex()}])

    def test_status_other_bytes_do_not_trigger_mapped_reason(self):
        sample = self.m.Experiment(self.data).status((255, 0, 255, 255, 255, 255))
        self.assertEqual([sample[k] for k in ('arg1_boolean', 'arg2_boolean', 'arg3_boolean')], [0, 0, 0])
        self.assertIsNone(sample['reason'])

    def test_failed_status_read_can_return_success_in_apple_code(self):
        # Actual firmware ignores the prerequisite transfer result. A future
        # Linux implementation must propagate it; do not copy this behavior.
        sample = self.m.Experiment(self.data, fail_transfer=0).status((0, 16, 0, 0, 0, 0))
        self.assertEqual(sample['return_code'], 0)
        self.assertIsNone(sample['reason'])
        self.assertIn('error', sample['transactions'][0])

    def test_usb_criterion_status_and_qualification(self):
        for status in (0, 8, 64, 72):
            for qualifier in (False, True):
                with self.subTest(status=status, qualifier=qualifier):
                    sample = self.m.Experiment(self.data).criteria((status, 0, 0, 0, 0, 0), qualifier)
                    expected = ['USB'] if status & 8 and (not qualifier or status & 64) else []
                    self.assertEqual(sample['criteria'], expected)
                    self.assertEqual(sample['transactions'],
                                     [{'direction': 'read', 'register': '0x0a',
                                       'bytes': bytes((status, 0, 0, 0, 0, 0)).hex()}])

    def test_firewire_symbol_depends_only_on_its_status_mask(self):
        for status in (0, 1, 2, 4, 6, 8, 16, 255):
            with self.subTest(status=status):
                sample = self.m.Experiment(self.data).criteria((status, 255, 255, 255, 255, 255))
                self.assertEqual('FireWire' in sample['criteria'], bool(status & 16))

    def test_fixed_address_tool_rejects_wrong_firmware(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'not-the-firmware'
            path.write_bytes(b'not a 12H321 kernelcache')
            result = subprocess.run([sys.executable, str(ROOT / 'tools/power/emulate-d1946.py'),
                                     str(path)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 2)
            self.assertIn('hash does not match', result.stderr)

    def test_apple_setter_does_not_check_initial_transfer_error(self):
        # This records a limitation of the Apple sequence, not behavior to
        # replicate in Linux: its initial failed read is followed by writes.
        e = self.m.Experiment(self.data, fail_transfer=0)
        self.assertEqual(e.call(0x80ed0768, 1, 1000, 0), 0)
        self.assertIn('error', e.transactions[0])
        self.assertTrue(any(t['direction'] == 'write' for t in e.transactions[1:]))
        self.assertEqual(e.registers[0x3f], 0)  # unread stack data replaced preserved bits


if __name__ == '__main__':
    unittest.main()
