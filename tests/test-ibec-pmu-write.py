import importlib.util
import os
from pathlib import Path
import unittest
ROOT = Path(__file__).resolve().parents[1]
IBEC = Path(os.environ.get('CASCADIA_TEST_IBEC', str(ROOT / 'build/firmware/iBEC.dec')))
AVAILABLE = importlib.util.find_spec('unicorn') is not None and IBEC.is_file()


@unittest.skipUnless(AVAILABLE, 'requires Unicorn and hash-pinned local iBEC')
class OriginalWriteHelpers(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        spec = importlib.util.spec_from_file_location('b', ROOT / 'tools/power/emulate-ibec-pmu-write.py')
        cls.b = importlib.util.module_from_spec(spec); spec.loader.exec_module(cls.b)
        cls.data = cls.b.load_input(IBEC)

    def run_fixture(self, helper, **kwargs): return self.b.WriteExperiment(self.data).call(helper, **kwargs)

    def test_both_helpers_success(self):
        for helper in ('charger', 'generic'):
            out = self.run_fixture(helper)
            self.assertEqual(out['return_code'], 0)
            self.assertEqual(out['final_fake_register_3f'], 0x40)
            self.assertEqual([t['address_byte'] for t in out['transactions']], [0x78, 0x79])
            self.assertFalse(out['hardware_access'])

    def test_charger_helper_mismatched_readback_returns_success(self):
        out = self.run_fixture('charger', readback=0xbf)
        self.assertEqual(out['return_code'], 0)
        self.assertEqual(out['transactions'][-1]['value_if_success'], 0xbf)

    def test_generic_helper_mismatched_readback_returns_minus1(self):
        self.assertEqual(self.run_fixture('generic', readback=0xbf)['return_code'], 0xffffffff)

    def test_failed_write_then_successful_read(self):
        for helper, expected in (('charger', 0), ('generic', 0xffffffff)):
            out = self.run_fixture(helper, write_error=0xffffffff)
            self.assertEqual(out['return_code'], expected)
            self.assertEqual(out['final_fake_register_3f'], 0xbf)
            self.assertEqual(len(out['transactions']), 2)

    def test_generic_failed_zero_read_can_report_success(self):
        out = self.run_fixture('generic', value=0, read_error=0xffffffff)
        self.assertEqual(out['return_code'], 0)
        self.assertFalse(out['transactions'][-1]['output_written'])

    def test_charger_read_error_return_is_preserved(self):
        self.assertEqual(self.run_fixture('charger', read_error=0xffffffff)['return_code'], 0xffffffff)

    def test_without_readback_return_difference(self):
        self.assertEqual(self.run_fixture('charger', verify=False, write_error=0xffffffff)['return_code'], 0xffffffff)
        self.assertEqual(self.run_fixture('generic', verify=False, write_error=0xffffffff)['return_code'], 0)

    def test_guards_and_single_use(self):
        for kwargs in ({'value': True}, {'verify': 1}, {'readback': 256}, {'write_error': -1}, {'read_error': True}):
            with self.assertRaises(ValueError): self.run_fixture('charger', **kwargs)
        with self.assertRaises(ValueError): self.run_fixture('unknown')
        for pointer, size in ((self.b.STACK - 1, 1), (self.b.STACK + 0x10000, 1)):
            with self.assertRaises(RuntimeError): self.b.WriteExperiment.require_stack(pointer, size)
        probe = self.b.WriteExperiment(self.data); probe.call('charger')
        with self.assertRaises(RuntimeError): probe.call('charger')
        with self.assertRaises(ValueError): self.b.WriteExperiment(b'wrong image')


if __name__ == '__main__': unittest.main()
