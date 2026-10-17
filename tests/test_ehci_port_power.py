"""Execute actual EHCI firmware state dispatch/register methods in fake RAM."""
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
    spec = importlib.util.spec_from_file_location('ehci_port_power', ROOT / 'scripts/emulate-ehci-port-power.py')
    emulator = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(emulator)


@unittest.skipUnless(unicorn is not None and CACHE.exists(), 'requires local verified firmware and build-image Unicorn')
class EHCIPortPowerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.data = CACHE.read_bytes()

    def test_power_on_sets_standard_power_and_wake_bits_without_acknowledging_w1c(self):
        result = emulator.experiment(self.data, 2, 0, 0x2B)
        self.assertEqual(result['register_writes'], ['0x1001', '0x301001'])
        self.assertEqual(result['return_code'], '0x0')
        self.assertFalse(result['hardware_access'])
        self.assertFalse(result['vbussource_or_voltage_verified'])

    def test_power_on_preserves_unrelated_vendor_and_status_bits(self):
        result = emulator.experiment(self.data, 2, 0, 0x80500001)
        self.assertEqual(result['register_writes'], ['0x80501001', '0x80701001'])

    def test_power_off_controller_sequence_preserves_non_power_fields(self):
        result = emulator.experiment(self.data, 0, 2, 0x30102F)
        self.assertEqual(result['register_writes'], ['0x301109', '0x300101'])
        self.assertEqual(len(result['external_calls']), 3)
        self.assertEqual(result['external_calls'][1]['stub'], '0x8084ab7c')
        self.assertEqual(result['external_calls'][1]['r0'], '0x1')

    def test_unknown_transition_and_unverified_firmware_are_refused(self):
        with self.assertRaisesRegex(ValueError, 'SHA256 mismatch'):
            emulator.experiment(b'unverified', 2, 0, 0)
        for request, current in ((1, 0), (2, 1), (2, 2)):
            with self.assertRaisesRegex(ValueError, 'only audited'):
                emulator.experiment(self.data, request, current, 0)
        with self.assertRaisesRegex(ValueError, '32-bit'):
            emulator.experiment(self.data, 2, 0, -1)


if __name__ == '__main__':
    unittest.main()
