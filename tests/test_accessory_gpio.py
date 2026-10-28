"""Execute actual GPIOFunction decode/output; no physical GPIO backend."""
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
    spec = importlib.util.spec_from_file_location('accessory_gpio', ROOT / 'scripts/emulate-accessory-gpio.py')
    emulator = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(emulator)


@unittest.skipUnless(unicorn is not None and CACHE.exists(), 'requires local verified firmware and build-image Unicorn')
class AccessoryGPIOTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.data = CACHE.read_bytes()

    def test_p105_switch_descriptor_decodes_to_line132_and_preserves_request(self):
        for request in (0, 1):
            with self.subTest(request=request):
                result = emulator.experiment(self.data, 0x1004, 0x101, request)
                self.assertTrue(result['initialized'])
                self.assertEqual(result['retained_selector'], 132)
                self.assertEqual(result['synthetic_gpio_requests'], [
                    {'software_line': 132, 'level': 0},
                    {'software_line': 132, 'level': request}])
                self.assertFalse(result['hardware_access'])
                self.assertFalse(result['vbus_source_or_voltage_verified'])

    def test_synthetic_comparison_flags_reverse_output_polarity(self):
        for request, output in ((0, 1), (1, 0)):
            with self.subTest(request=request):
                result = emulator.experiment(self.data, 0x1004, 1, request)
                self.assertEqual(result['synthetic_gpio_requests'], [
                    {'software_line': 132, 'level': 1},
                    {'software_line': 132, 'level': output}])

    def test_real_initializer_refuses_invalid_pin_and_out_of_range_port(self):
        for descriptor in (0x1008, 0x1C00):
            with self.subTest(descriptor=descriptor):
                result = emulator.experiment(self.data, descriptor, 0x101, 1)
                self.assertFalse(result['initialized'])
                self.assertEqual(result['synthetic_gpio_requests'], [])
                self.assertEqual(result['retained_selector'], descriptor)

    def test_opt_in_real_provider_writes_fake_register_at_line132_offset(self):
        result = emulator.experiment(self.data, 0x1004, 0x101, 1, model_provider=True)
        self.assertEqual(result['synthetic_register_writes'], [
            {'offset': '0x210', 'value': '0x292'},
            {'offset': '0x210', 'value': '0x293'}])
        self.assertEqual(result['synthetic_gpio_requests'], [
            {'software_line': 132, 'level': 0}, {'software_line': 132, 'level': 1}])
        self.assertEqual([call['stub'] for call in result['external_calls']],
                         ['0x80df0874', '0x80df05a4', '0x80df05b4', '0x80df0874'] * 2)
        self.assertFalse(result['hardware_access'])
        self.assertFalse(result['vbus_source_or_voltage_verified'])

    def test_opt_in_provider_preserves_shadow_fields_and_opposite_polarity(self):
        for shadow, initial, asserted in ((0, '0x2', '0x3'), (0xFFFF, '0xff92', '0xff93'),
                                         (0xABCD, '0xab82', '0xab83')):
            with self.subTest(shadow=shadow):
                result = emulator.experiment(self.data, 0x1004, 0x101, 1,
                                             model_provider=True, initial_shadow=shadow)
                self.assertEqual(result['synthetic_register_writes'], [
                    {'offset': '0x210', 'value': initial}, {'offset': '0x210', 'value': asserted}])
        opposite = emulator.experiment(self.data, 0x1004, 1, 1, model_provider=True)
        self.assertEqual(opposite['synthetic_register_writes'], [
            {'offset': '0x210', 'value': '0x293'}, {'offset': '0x210', 'value': '0x292'}])

    def test_opt_in_provider_is_not_called_for_invalid_descriptor(self):
        for pinport in (0x1008, 0x1C00):
            with self.subTest(pinport=pinport):
                result = emulator.experiment(self.data, pinport, 0x101, 1, model_provider=True)
                self.assertEqual(result['synthetic_register_writes'], [])
                self.assertEqual(result['external_calls'], [])

    def test_unverified_firmware_and_unaudited_mode_inputs_are_refused(self):
        with self.assertRaisesRegex(ValueError, 'SHA256 mismatch'):
            emulator.experiment(b'unverified', 0x1004, 0x101, 1)
        for pin in (-1, 0x10000, True):
            with self.assertRaisesRegex(ValueError, 'pinport'):
                emulator.experiment(self.data, pin, 0x101, 1)
        for flags in (0, 2, 0x201, True):
            with self.assertRaisesRegex(ValueError, 'flags'):
                emulator.experiment(self.data, 0x1004, flags, 1)
        for value in (-1, 2, True):
            with self.assertRaisesRegex(ValueError, 'request'):
                emulator.experiment(self.data, 0x1004, 0x101, value)
        with self.assertRaisesRegex(ValueError, 'boolean'):
            emulator.experiment(self.data, 0x1004, 0x101, 1, model_provider=1)
        for shadow in (-1, 0x10000, True):
            with self.assertRaisesRegex(ValueError, 'shadow'):
                emulator.experiment(self.data, 0x1004, 0x101, 1, initial_shadow=shadow)


if __name__ == '__main__':
    unittest.main()
