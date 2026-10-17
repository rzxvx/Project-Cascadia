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
class ActualVoltageOwner(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        spec = importlib.util.spec_from_file_location('v', STAGE / 'tools/power/emulate-voltage-owner.py')
        cls.v = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.v)
        cls.data = cls.v.events.load_input(KC)

    def run_fixture(self, **kwargs): return self.v.VoltageOwner(self.data).experiment(**kwargs)

    def test_actual_callback_storage_forward_and_zero_io(self):
        out = self.run_fixture(value=4000, prior_limit=4200)
        self.assertEqual(out['stored_limit'], 4000)
        self.assertEqual(out['calls'][-1]['owner_limit_at_call'], 4000)
        self.assertEqual(out['callback_return'], 0)
        self.assertEqual(out['transactions'], [])
        self.assertFalse(out['hardware_access'])

    def test_equal_and_above_cutoff(self):
        for sample in (4000, 4001, 0xffffffff):
            self.assertEqual(self.run_fixture(sample=sample)['current_ceiling_raw'], 0)

    def test_below_cutoff_remains_unlimited_sentinel_without_table(self):
        self.assertEqual(self.run_fixture(sample=3999)['current_ceiling_raw'], 0xffffffff)

    def test_zero_limit_disables_software_cutoff(self):
        out = self.run_fixture(value=0, prior_limit=4000, sample=0xffffffff)
        self.assertEqual(out['stored_limit'], 0)
        self.assertEqual(out['current_ceiling_raw'], 0xffffffff)

    def test_absent_and_wrongtype_keep_prior(self):
        for kind in ('absent', 'wrong_type'):
            out = self.run_fixture(kind=kind, prior_limit=4200, sample=4000)
            self.assertEqual(out['stored_limit'], 4200)
            self.assertEqual(out['current_ceiling_raw'], 0xffffffff)
            self.assertFalse(any(c['mocked'] == 'OSNumber value' for c in out['calls']))

    def test_backend_failure_does_not_rollback_owner(self):
        out = self.run_fixture(value=4000, prior_limit=4200, forward_error=0xe00002c0)
        self.assertEqual(out['stored_limit'], 4000)
        self.assertEqual(out['callback_return'], 0xe00002c0)
        self.assertEqual(out['current_ceiling_raw'], 0)

    def test_uint_boundaries_are_arithmetic_only(self):
        self.assertEqual(self.run_fixture(value=1, sample=1)['current_ceiling_raw'], 0)
        self.assertEqual(self.run_fixture(value=0xffffffff, sample=0xfffffffe)['current_ceiling_raw'], 0xffffffff)

    def test_field_ownership_stays_separate(self):
        probe = self.v.VoltageOwner(self.data)
        probe.w32(probe.OWNER + 0x248, 1234)
        probe.w32(probe.OWNER + 0x138, 100)
        probe.w32(probe.OWNER + 0x14c, 2100)
        probe.experiment()
        self.assertEqual([probe.u32(probe.OWNER + offset) for offset in (0x248, 0x138, 0x14c)], [1234, 100, 2100])

    def test_input_guards_and_single_use(self):
        for kwargs in ({'value': True}, {'sample': -1}, {'prior_limit': 1 << 32}, {'forward_error': False}, {'kind': 'data'}):
            with self.assertRaises(ValueError): self.run_fixture(**kwargs)
        probe = self.v.VoltageOwner(self.data)
        probe.experiment()
        with self.assertRaises(RuntimeError): probe.experiment()

    def test_hash_guard(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'wrong'
            path.write_bytes(b'wrong')
            with self.assertRaises(ValueError): self.v.events.load_input(path)


if __name__ == '__main__': unittest.main()
