"""Run real THS7383 routing instructions; assert independently listed fixtures."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
CACHE = ROOT / 'build/firmware/kernelcache.12H321.macho'
try:
    import unicorn
except ModuleNotFoundError:
    unicorn = None
if unicorn:
    spec = importlib.util.spec_from_file_location('tristar_routing', ROOT / 'scripts/emulate-tristar-routing.py')
    emulator = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(emulator)


@unittest.skipUnless(unicorn is not None and CACHE.exists(), 'requires local verified firmware and build-image Unicorn')
class TristarRoutingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.data = CACHE.read_bytes()

    def test_thirty_orientation_and_selector_fixtures(self):
        # Results were independently transcribed from the audited branch graph,
        # rather than inferred from the emulator implementation.
        fixtures = [(None, None, '00'), ('idbs', None, '00'), ('apwr', None, '03'),
                    (None, 'idbs', '00'), ('urt0', None, None), ('apwq', None, None),
                    ('usb0', None, None), ('idbs', 'urt0', None),
                    ('urt1', None, '01'), ('jtag', None, '02')]
        count = 0
        for first, second, byte in fixtures:
            for orientation in (0, 1, 2):
                with self.subTest(first=first, second=second, orientation=orientation):
                    result = emulator.experiment(self.data, first, second, orientation)
                    count += 1
                    if first in ('urt1', 'jtag') and orientation != 1:
                        self.assertEqual(result['return_code'], '0xe3ff82e6')
                        self.assertEqual(result['i2c_writes'], [])
                    elif byte is None:
                        self.assertEqual(result['return_code'], '0xe00002e6')
                        self.assertEqual(result['i2c_writes'], [])
                    else:
                        self.assertEqual(result['return_code'], '0x0')
                        self.assertEqual(result['i2c_writes'], [{'register': 2, 'value': byte}])
                    self.assertFalse(result['hardware_access'])
                    self.assertFalse(result['vbus_source_or_voltage_verified'])
                    self.assertEqual(len(result['orientation_getter_calls']),
                                     1 if first in ('urt1', 'jtag') else 0)
        self.assertEqual(count, 30)

    def test_unverified_firmware_and_unsupported_arguments_are_refused(self):
        with self.assertRaisesRegex(ValueError, 'SHA256 mismatch'):
            emulator.experiment(b'unverified', None, None, 0)
        for value in (-1, 3, True, '1'):
            with self.assertRaisesRegex(ValueError, 'orientation'):
                emulator.experiment(self.data, None, None, value)
        for selector in ('abcd', 1, b'apwr'):
            with self.assertRaisesRegex(ValueError, 'selectors'):
                emulator.experiment(self.data, selector, None, 0)


if __name__ == '__main__':
    unittest.main()
