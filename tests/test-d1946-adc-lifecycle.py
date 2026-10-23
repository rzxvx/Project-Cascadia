#!/usr/bin/env python3
"""Actual pinned firmware ADC lifecycle against bounded scheduler/PMU fixtures."""
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
KC = Path(os.environ.get('CASCADIA_TEST_KERNELCACHE', ROOT / 'build/firmware/kernelcache.12H321.macho'))
AVAILABLE = importlib.util.find_spec('unicorn') is not None and KC.is_file()


@unittest.skipUnless(AVAILABLE, 'needs Unicorn and local p105 12H321 kernelcache')
class Lifecycle(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        spec = importlib.util.spec_from_file_location('lifecycle', ROOT / 'tools/power/emulate-d1946-adc-lifecycle.py')
        cls.m = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.m)
        cls.data = cls.m.events.load_input(KC)

    def fixture(self, **kwargs):
        return self.m.Acquisition(self.data, **kwargs)

    def test_actual_irq_completion_reads_raw_and_cleans_control(self):
        for channel in (0, 4, 15, 21, 27):
            for raw in (0, 1, 2048, 4095):
                out = self.fixture().acquire(channel, raw)
                self.assertEqual((out['return_code'], out['output'], out['state']), (0, raw, 0))
                self.assertEqual(out['control_43'], channel)
                self.assertEqual(out['transactions'], [
                    {'direction': 'write', 'register': '0x43', 'bytes': f'{channel | 0x20:02x}'},
                    {'direction': 'read', 'register': '0x01', 'bytes': '002000000000000000'},
                    {'direction': 'read', 'register': '0x44', 'bytes': bytes((raw & 15, raw >> 4)).hex()},
                    {'direction': 'write', 'register': '0x43', 'bytes': f'{channel:02x}'}])
                self.assertEqual([c['state'] for c in out['calls'] if c['mocked'] == 'commandWakeup'], [2, 0])
                self.assertFalse(out['hardware_access'])

    def test_settle_mode_is_channel_three_or_qualifier_then_conversion_deadline(self):
        for channel, qualifier, expected in ((3, False, [80, 1000]),
                (15, True, [80, 1000]), (15, False, [1000])):
            out = self.fixture().acquire(channel, 1, qualifier=qualifier)
            deadlines = [c for c in out['calls'] if c['mocked'] == 'clock_interval_to_deadline']
            self.assertEqual([c['interval'] for c in deadlines], expected)
            self.assertEqual([c['scale'] for c in deadlines], [1000000] * len(expected))
            if len(expected) == 2:
                self.assertEqual([t['bytes'] for t in out['transactions'] if t['direction'] == 'write'],
                                 [f'{channel | 0x40:02x}', f'{channel | 0x60:02x}', f'{channel:02x}'])

    def test_timeout_unrelated_interrupt_and_restart_do_not_publish_raw(self):
        for mode, expected_results in (('timeout', [1]), ('unrelated', [0, 1]),
                                      ('interrupted', [2, 1]), ('restart', [3, 1])):
            out = self.fixture().acquire(15, 4095, mode)
            self.assertEqual((out['return_code'], out['state'], out['control_43']), (0xe00002d6, 0, 15))
            self.assertTrue(out['output_untouched'])
            self.assertNotIn('0x44', [t['register'] for t in out['transactions']])
            self.assertEqual([c['result'] for c in out['calls'] if c['mocked'] == 'commandSleep deadline'], expected_results)

    def test_callback_only_completes_a_waiting_conversion(self):
        for state in range(4):
            out = self.fixture().callback(state)
            self.assertEqual(out['state'], 2 if state == 1 else state)
            self.assertEqual(out['return_code'], 0)
            self.assertEqual(out['transactions'], [])
            self.assertEqual(len(out['calls']), 1 if state == 1 else 0)

    def test_masked_adc_event_is_pending_and_masks_register_instead_of_callback(self):
        out = self.fixture().acquire(15, 0, masked=True)
        self.assertEqual((out['return_code'], out['event_pending']), (0xe00002d6, 1))
        self.assertIn({'direction': 'write', 'register': '0x13', 'bytes': '20'}, out['transactions'])
        self.assertEqual([c['state'] for c in out['calls'] if c['mocked'] == 'commandWakeup'], [0])

    def test_completion_state_wins_even_when_wait_returns_timeout(self):
        out = self.fixture().acquire(27, 4095, 'timeout-with-completion')
        self.assertEqual((out['return_code'], out['output'], out['state']), (0, 4095, 0))

    def test_original_start_event_read_raw_read_and_cleanup_failures_are_distinct(self):
        expected = ((0, 4095, 15), (0xe00002d6, 0xc0ffee, 15),
                    (0xe00002c0, 0xc0ffee, 15), (0, 4095, 0x2f))
        for failure, values in enumerate(expected):
            out = self.fixture(fail_transfer=failure).acquire(15, 4095)
            self.assertEqual((out['return_code'], out['output'], out['control_43']), values)
            self.assertEqual(out['state'], 0)
            self.assertIn('error', out['transactions'][failure])

    def test_special_channels_reject_sign_bit_without_rejecting_other_channels(self):
        for channel in (5, 9, 15):
            out = self.fixture().acquire(channel, 2048, bad_first_byte=True)
            self.assertEqual(out['return_code'], 0 if channel == 15 else 0xe00002e8)
            self.assertEqual(out['output_untouched'], channel != 15)
            self.assertEqual((out['state'], out['control_43']), (0, channel))

    def test_power_gate_waits_for_idle_and_wake_resets_state_without_pmu_io(self):
        out = self.fixture().power_gate(False)
        self.assertEqual((out['return_code'], out['state'], out['sleep_type']), (0, 3, 2))
        self.assertEqual(out['transactions'], [])
        out = self.fixture().power_gate(True, 3)
        self.assertEqual((out['return_code'], out['state'], out['sleep_type']), (0, 0, 0))
        self.assertEqual(out['transactions'], [])
        busy = self.fixture()
        with self.assertRaisesRegex(RuntimeError, 'indefinite ADC state wait'):
            busy.power_gate(False, 1)
        self.assertEqual(busy.state(), 1)
        self.assertEqual(busy.transactions, [])

    def test_range_hash_and_bounded_wait_guards(self):
        for channel, raw in ((-1, 0), (28, 0), (15, -1), (15, 4096), (0x10003, 0)):
            with self.assertRaises(ValueError):
                self.fixture().acquire(channel, raw)
        with self.assertRaises(ValueError):
            self.fixture().acquire(15, 0, 'invented')
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'bad-kernelcache'
            path.write_bytes(b'not the pinned Apple image')
            with self.assertRaisesRegex(ValueError, 'hash mismatch'):
                self.m.events.load_input(path)


if __name__ == '__main__':
    unittest.main()
