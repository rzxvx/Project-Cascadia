"""ID-routing evidence uses real firmware and keeps electrical/API gaps visible."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
CACHE = ROOT / "build/firmware/kernelcache.12H321.macho"
try:
    import unicorn
except ModuleNotFoundError:
    unicorn = None
if unicorn:
    spec = importlib.util.spec_from_file_location("accessory_routing", ROOT / "scripts/emulate-accessory-routing.py")
    emulator = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(emulator)


@unittest.skipUnless(unicorn is not None and CACHE.exists(), "requires official local firmware and build-image Unicorn")
class AccessoryRoutingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.data = CACHE.read_bytes()

    def test_independent_normal_cable_fixture_selects_usb0_without_role_claim(self):
        for predicate in (0, 1):
            result = emulator.experiment(self.data, bytes.fromhex("100c00000000"), platform_predicate=predicate)
            self.assertEqual(result["output_fourcc"], ["usb0", None])
            self.assertEqual(result["stubbed_platform_predicate"], [])
            self.assertFalse(result["apple_cable_type_known"])
            self.assertFalse(result["physical_pin_mapping_verified"])

    def test_separate_accessory_decoder_has_high_bit_routing_and_reserved_case(self):
        for first, expected in ((0x40, ["urt1", "urt1"]), (0x80, ["jtag", "jtag"]), (0xC0, [None, None])):
            with self.subTest(first=first):
                result = emulator.experiment(self.data, bytes([first]) + b"\0" * 5, "accessory-modes")
                self.assertEqual(result["output_fourcc"], expected)

    def test_unverified_firmware_and_invalid_ids_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "SHA256 mismatch"):
            emulator.experiment(b"unverified", b"\0" * 6)
        with self.assertRaisesRegex(ValueError, "exactly six"):
            emulator.experiment(self.data, b"\0" * 5)
        with self.assertRaisesRegex(ValueError, "invalid offline"):
            emulator.experiment(self.data, b"\0" * 6, "vbus-enable")


if __name__ == "__main__":
    unittest.main()
