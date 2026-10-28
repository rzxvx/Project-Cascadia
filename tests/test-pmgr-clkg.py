#!/usr/bin/env python3
"""Pinned original factory/clkG bridge; fake registry/CommandGate/PMGR only."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class TrueClockGate(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            import unicorn
            import capstone
        except ImportError:
            raise unittest.SkipTest('Unicorn/Capstone required for original instructions')
        cls.cache = ROOT/'build/firmware/kernelcache.12H321.macho'
        cls.tree = ROOT/'build/usb-host-research/adt-evidence/DeviceTree.p105ap.raw'
        if not cls.cache.is_file() or not cls.tree.is_file():
            raise unittest.SkipTest('separately obtained pinned official P105 sources required')
        spec = importlib.util.spec_from_file_location('clkg', ROOT/'scripts/emulate-pmgr-clkg.py')
        cls.module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.module)
        oracle = (ROOT/'tests/fixtures/pmgr-clkg/oracle.json').read_bytes()
        cls.oracle = json.loads(oracle)
        cls.result = cls.module.verify(cls.cache, cls.tree)

    def test_exact_original_instruction_oracle(self):
        self.assertEqual(self.result, self.oracle)
        self.assertEqual(sum(len(case['states']) for case in self.result['cases']), 9)

    def test_true_factory_retains_original_typed_parent(self):
        for case in self.result['cases']:
            self.assertEqual(case['factory_vptr'], '0x804d4468')
            self.assertEqual(case['factory_return'], 0)
            self.assertEqual(case['init_return'], 1)
            for state in case['states']:
                self.assertEqual(state['return'], 0)
            self.assertFalse(case['hardware_access'])

    def test_wrong_cpui_source_type_refuses_raw_clock_request(self):
        case = self.result['wrong_cpui_control']
        self.assertEqual(case['factory_vptr'], '0x804d45a8')
        self.assertEqual(case['states'][0]['return'], 0xe00002c7)
        self.assertFalse([e for e in case['states'][0]['events'] if e[0] == 'fake_pmgr_write'])

    def test_failed_initialization_or_parent_cast_never_attempts_clock(self):
        self.assertEqual(self.result['base_init_failure']['init_return'], 0)
        self.assertFalse(self.result['base_init_failure']['states'])
        # Apple's true typed init can return1 despite null metacast parent.
        failed = self.result['parent_cast_failure']
        self.assertEqual(failed['init_return'], 1)
        self.assertEqual(failed['retained_parent_cast'], 0)
        self.assertFalse(failed['states'])

    def test_invalid_or_unbounded_requests_refused(self):
        for requests in ([], [[129, 1]], [[89, 2]], [[89, 1]]*65):
            with self.assertRaises(ValueError):
                self.module.experiment(requests)

    def test_unpinned_inputs_rejected_before_emulation(self):
        with tempfile.TemporaryDirectory() as td:
            wrong = Path(td)/'wrong-image'
            wrong.write_bytes(b'unverified')
            with self.assertRaises(ValueError):
                self.module.load_sources(wrong, self.tree)
            with self.assertRaises(ValueError):
                self.module.load_sources(self.cache, wrong)


if __name__ == '__main__':
    unittest.main(verbosity=2)
