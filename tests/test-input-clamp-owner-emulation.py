#!/usr/bin/env python3
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest
STAGE = Path(__file__).resolve().parents[1]
REPO = Path(os.environ.get('CASCADIA_RESEARCH_REPO', STAGE))
KC = Path(os.environ.get('CASCADIA_TEST_KERNELCACHE', REPO / 'build/firmware/kernelcache.12H321.macho'))
AVAILABLE = importlib.util.find_spec('unicorn') is not None and KC.is_file()


@unittest.skipUnless(AVAILABLE, 'requires Unicorn and local hash-pinned kernelcache')
class ActualClampOwner(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        spec = importlib.util.spec_from_file_location('c', STAGE / 'tools/power/emulate-input-clamp-owner.py')
        cls.c = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.c)
        cls.data = cls.c.events.load_input(KC)

    def run_fixture(self, **kwargs): return self.c.ClampOwner(self.data).experiment(**kwargs)

    def test_missing_and_wrongtype_floor_zero(self):
        for kind in ('absent', 'wrong_type'):
            out = self.run_fixture(kind=kind, value=500)
            self.assertEqual(out['stored_value'], 0)
            self.assertEqual(out['transactions'], [])
            self.assertFalse(out['hardware_access'])

    def test_boot_argument_mapping_and_precedence(self):
        for boot, expected in ((0, 0), (1, 500), (2, 2), (100, 100), (0xffffffff, 0xffffffff)):
            out = self.run_fixture(boot=boot, kind='data', value=2100)
            self.assertEqual(out['stored_value'], expected)
            self.assertEqual(len(out['calls']), 1)

    def test_osdata_floor_exact_four_bytes(self):
        for value in (0, 1, 500, 2100, 0xffffffff):
            self.assertEqual(self.run_fixture(kind='data', value=value)['stored_value'], value)

    def test_malformed_reported_floor_length_defaults500(self):
        for length in (0, 1, 3, 5, 8):
            out = self.run_fixture(kind='data', value=2100, length=length)
            self.assertEqual(out['stored_value'], 500)
            self.assertFalse(any(c['mocked'] == 'OSData bytes' for c in out['calls']))

    def test_cap_default_and_property_override(self):
        for kind in ('absent', 'wrong_type'):
            self.assertEqual(self.run_fixture(target='cap', kind=kind)['stored_value'], 1000)
        self.assertEqual(self.run_fixture(target='cap', kind='data', value=2100)['stored_value'], 2100)

    def test_cap_no_length_check_in_bounded_fixture(self):
        for length in (0, 1, 3, 4, 8):
            out = self.run_fixture(target='cap', kind='data', value=2100, length=length)
            self.assertEqual(out['stored_value'], 2100)
            self.assertFalse(any(c['mocked'] == 'OSData length' for c in out['calls']))

    def test_only_selected_field_mutates(self):
        probe = self.c.ClampOwner(self.data)
        probe.w32(probe.OWNER + 0x14c, 1234)
        probe.w32(probe.OWNER + 0x24c, 4000)
        probe.experiment(kind='data', value=100)
        import struct
        self.assertEqual(struct.unpack('<I', probe.mu.mem_read(probe.OWNER + 0x14c, 4))[0], 1234)
        self.assertEqual(struct.unpack('<I', probe.mu.mem_read(probe.OWNER + 0x24c, 4))[0], 4000)

    def test_guards_hash_and_single_use(self):
        for kwargs in ({'boot': True}, {'length': -1}, {'value': 1 << 32}, {'target': 'unknown'}, {'target': 'cap', 'boot': 1}):
            with self.assertRaises(ValueError): self.run_fixture(**kwargs)
        probe = self.c.ClampOwner(self.data); probe.experiment()
        with self.assertRaises(RuntimeError): probe.experiment()
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'wrong'; path.write_bytes(b'wrong')
            with self.assertRaises(ValueError): self.c.events.load_input(path)


if __name__ == '__main__': unittest.main()
