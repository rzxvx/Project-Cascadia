#!/usr/bin/env python3
"""Original source VBUS getter: error propagation and no physical conversion."""
import importlib.util
import os
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
KC = Path(os.environ.get('CASCADIA_TEST_KERNELCACHE', ROOT / 'build/firmware/kernelcache.12H321.macho'))
AVAILABLE = importlib.util.find_spec('unicorn') is not None and KC.is_file()


@unittest.skipUnless(AVAILABLE, 'requires Unicorn and hash-pinned local 12H321 kernelcache')
class GetterTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        spec = importlib.util.spec_from_file_location('getter', ROOT / 'tools/power/check-source-vbus-getter.py')
        cls.g = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.g)
        cls.data = cls.g.adc.events.load_input(KC)

    def sample(self, **kwargs):
        return self.g.SourceVBUS(self.data).sample(**kwargs)

    def test_actual_getter_wrapper_scaler_chain(self):
        for raw in (0, 1, 2047, 2048, 3072, 4094, 4095):
            with self.subTest(raw=raw):
                out = self.sample(raw=raw)
                self.assertEqual(out['return_code'], 0)
                self.assertEqual(out['output_u32'], 88 * raw)
                self.assertEqual([c['channel'] for c in out['calls']], [15, 15])
                self.assertEqual(out['calls'][0]['actual_callback'], '0x80ece491')
                self.assertEqual(out['transactions'], [])

    def test_raw_errors_preserve_arbitrary_prior_output(self):
        for error in (1, 0xe00002c0, 0xe00002d6, 0xffffffff):
            for prior in (0x80000000, 0, 270336, 0xdeadbeef):
                out = self.sample(raw=3072, raw_error=error, sentinel=prior)
                self.assertEqual(out['return_code'], error)
                self.assertEqual(out['output_u32'], prior)
                self.assertTrue(out['output_untouched'])
                self.assertEqual(len(out['calls']), 2)

    def test_gate_error_skips_conversion(self):
        for prior in (0x80000000, 270336):
            out = self.sample(raw=3072, gate_error=0xe00002d6, sentinel=prior)
            self.assertEqual(out['return_code'], 0xe00002d6)
            self.assertEqual(out['output_u32'], prior)
            self.assertEqual(len(out['calls']), 1)
            self.assertFalse(out['hardware_access'])

    def test_unexpected_hardware_paths_refused(self):
        probe = self.g.SourceVBUS(self.data)
        for entry in (self.g.d.READ, self.g.d.WRITE, 0x80ecd9c8):
            with self.assertRaises(RuntimeError):
                probe.invoke(entry, (self.g.d.PMU, 0, 0, 0))

    def test_bad_inputs_and_hash_refused(self):
        for kwargs in ({'raw': True}, {'raw': -1}, {'raw': 4096},
                       {'raw': 0, 'raw_error': True}, {'raw': 0, 'gate_error': -1},
                       {'raw': 0, 'sentinel': 0x100000000}):
            with self.assertRaises(ValueError):
                self.sample(**kwargs)
        with self.assertRaises(ValueError):
            self.g.SourceVBUS(b'wrong firmware')

    def test_fresh_fixture_required(self):
        probe = self.g.SourceVBUS(self.data)
        probe.sample(0)
        with self.assertRaises(RuntimeError):
            probe.sample(1)


if __name__ == '__main__':
    unittest.main()
