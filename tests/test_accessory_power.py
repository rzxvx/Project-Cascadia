"""Assert actual firmware accessory callback ordering and ID voltage boundaries."""
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
    spec = importlib.util.spec_from_file_location('accessory_power', ROOT / 'scripts/emulate-accessory-power.py')
    emulator = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(emulator)


@unittest.skipUnless(unicorn is not None and CACHE.exists(), 'requires local verified firmware and build-image Unicorn')
class AccessoryPowerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.data = CACHE.read_bytes()

    def test_callback_order_and_independent_values(self):
        for values in ((0, 0, 0), (1, 0, 1), (1, 1, 0)):
            with self.subTest(values=values):
                result = emulator.power_callbacks(self.data, *values)
                self.assertEqual(result['callbacks'], [
                    {'function': 'function-acc_pwr', 'arguments': [values[0]]},
                    {'function': 'function-acc_sw_en', 'arguments': [values[1]]},
                    {'function': 'function-acc_sleep_pwr', 'arguments': [values[2]]}])
                self.assertEqual(result['cached_enable'], values[0])
                self.assertEqual(result['return_code'], '0x0')
                self.assertFalse(result['hardware_access'])
                self.assertFalse(result['vbus_source_or_voltage_verified'])

    def test_provider_errors_short_circuit_without_rollback(self):
        for errors, count, code in (((0xE00002BC, 0, 0), 1, '0xe00002bc'),
                                    ((0, 0xE00002C0, 0), 2, '0xe00002c0'),
                                    ((0, 0, 0xE00002C1), 3, '0xe00002c1')):
            with self.subTest(errors=errors):
                result = emulator.power_callbacks(self.data, 1, 1, 1, errors=errors)
                self.assertEqual(len(result['callbacks']), count)
                self.assertEqual(result['return_code'], code)
                # Actual method updates the cache before the first callback;
                # a failed provider result does not establish rail state.
                self.assertEqual(result['cached_enable'], 1)

    def test_absent_optional_providers_are_skipped(self):
        result = emulator.power_callbacks(self.data, 1, 1, 1, optional=(False, False))
        self.assertEqual(result['callbacks'], [{'function': 'function-acc_pwr', 'arguments': [1]}])
        self.assertEqual(result['return_code'], '0x0')

    def test_voltage_id_boundary_fixtures(self):
        for byte, cached, voltage, new_class, replans in (
                (0x00, 3, None, 0, 1), (0x3F, 0, None, 3, 1),
                (0x40, 3, 2500, 4, 1), (0x4F, 3, 2500, 4, 1),
                (0x50, 3, 2600, 5, 1), (0xAF, 3, 3100, 10, 1),
                (0xBF, 3, 3200, 11, 1), (0xC0, 3, 3300, 12, 1),
                (0xFF, 3, 3300, 15, 1), (0x4F, 4, None, 4, 0)):
            with self.subTest(byte=byte, cached=cached):
                result = emulator.id_voltage(self.data, byte, cached)
                self.assertEqual(result['new_class'], new_class)
                self.assertEqual(len(result['replans']), replans)
                expected = [] if voltage is None else [
                    {'function': 'function-acc_pwr', 'arguments': [1, voltage]}]
                self.assertEqual(result['callbacks'], expected)
                self.assertFalse(result['hardware_access'])
                self.assertFalse(result['vbus_source_or_voltage_verified'])

    def test_voltage_change_preserves_cached_enable(self):
        result = emulator.id_voltage(self.data, 0x40, 3, cached_enable=0)
        self.assertEqual(result['callbacks'], [{'function': 'function-acc_pwr', 'arguments': [0, 2500]}])

    def test_unverified_firmware_and_invalid_input_are_refused(self):
        with self.assertRaisesRegex(ValueError, 'SHA256 mismatch'):
            emulator.power_callbacks(b'unverified', 0, 0, 0)
        with self.assertRaisesRegex(ValueError, 'SHA256 mismatch'):
            emulator.id_voltage(b'unverified', 0, 0)
        for value in (-1, 2, True, '1'):
            with self.assertRaisesRegex(ValueError, 'enable'):
                emulator.power_callbacks(self.data, value, 0, 0)
        for value in (-1, 256, True):
            with self.assertRaisesRegex(ValueError, 'ID byte'):
                emulator.id_voltage(self.data, value, 0)
        with self.assertRaisesRegex(ValueError, 'cached class'):
            emulator.id_voltage(self.data, 0, 16)
        with self.assertRaisesRegex(ValueError, 'three'):
            emulator.power_callbacks(self.data, 1, 0, 0, errors=(0, 0))
        with self.assertRaisesRegex(ValueError, 'provider result'):
            emulator.power_callbacks(self.data, 1, 0, 0, errors=(-1, 0, 0))
        with self.assertRaisesRegex(ValueError, 'boolean'):
            emulator.power_callbacks(self.data, 1, 0, 0, optional=(0, 1))


if __name__ == '__main__':
    unittest.main()
