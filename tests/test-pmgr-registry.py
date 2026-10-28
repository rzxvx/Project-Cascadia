#!/usr/bin/env python3
"""Original Performance publisher/lookup/factory; an explicit fake registry."""
import hashlib
import importlib.util
import json
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class Registry(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            import unicorn
            import capstone
        except ImportError:
            raise unittest.SkipTest('Unicorn/Capstone required')
        if not (ROOT/'build/firmware/kernelcache.12H321.macho').is_file() or not (
                ROOT/'build/usb-host-research/adt-evidence/DeviceTree.p105ap.raw').is_file():
            raise unittest.SkipTest('separately obtained pinned official inputs required')
        spec = importlib.util.spec_from_file_location('clkg', ROOT/'scripts/emulate-pmgr-clkg.py')
        cls.source = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.source)
        raw = (ROOT/'tests/fixtures/pmgr-registry/oracle.json').read_bytes()
        if hashlib.sha256(raw).hexdigest() != '4fa66fde785f4d3dda8f0ed4a499860d46fbfb4e6554c289485e444735ccee8a':
            raise AssertionError('frozen independent oracle changed')
        cls.expected = json.loads(raw)
        cls.result = cls.source.verify(registry=True)

    def test_exact_original_pipeline_oracle(self):
        self.assertEqual(self.result, self.expected)
        self.assertEqual(len(self.result['positive']['states']), 4)

    def test_actual_publisher_then_matched_subclass_factory_order(self):
        positive = self.result['positive']
        self.assertEqual(positive['published_keys'], ['IOFunctionParent10000000'])
        self.assertEqual(positive['factory_vptr'], '0x804d4468')
        self.assertEqual([e[1] for e in positive['init_events'] if e[0] == 'actual_source_stage'],
                         ['0x80b8c0b8', '0x804b1d2c', '0x80b8c168', '0x804b01ac'])
        for state in positive['states']:
            self.assertEqual(state['return'], 0)
        self.assertFalse(positive['hardware_access'])

    def test_wrong_absent_or_missing_phandle_never_fabricates_parent(self):
        self.assertEqual(len(self.result['negative']), 3)
        for case in self.result['negative']:
            self.assertIn('no matching parent', case['blocked'])
            self.assertFalse(case['states'])
            self.assertFalse([e for e in case['init_events'] if e[0] == 'fake_pmgr_write'])
        # Native startup can return0 even though its marker publication failed.
        missing = self.result['negative'][2]
        self.assertEqual(missing['registration_return'], 0)
        self.assertFalse(missing['published_keys'])

    def test_existing_default_proof_is_exactly_unchanged(self):
        original = json.loads((ROOT/'tests/fixtures/pmgr-clkg/oracle.json').read_text())
        self.assertEqual(self.source.verify(), original)

    def test_helper_rejects_unpinned_or_mismatched_context_before_emulation(self):
        spec = importlib.util.spec_from_file_location('registry_helper', ROOT/'scripts/emulate-pmgr-registry.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        m = self.source
        correct = [m.e, m.data, m.adt, m.table, m.MIN, m.MAX, dict(m.rows)]
        for at, value in ((1, b'unverified'), (2, b'unverified'), (3, b''), (4, 55), (5, 129), (6, {})):
            args = list(correct)
            args[at] = value
            with self.assertRaises(ValueError):
                module.verify_with_sources(*args)


if __name__ == '__main__':
    unittest.main(verbosity=2)
