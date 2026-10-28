"""Actual firmware in fake PMU: enable masks, voltage bounds and transfer errors."""
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
    spec = importlib.util.spec_from_file_location("usb_ldo_emulator", ROOT / "scripts/emulate-usb-accessory-ldo.py")
    emulator = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(emulator)


@unittest.skipUnless(unicorn is not None and CACHE.exists(), "requires local official firmware and existing build-image Unicorn")
class AccessoryLdoTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.data = CACHE.read_bytes()

    def test_both_accessory_enable_masks_preserve_all_other_registers(self):
        for index, register in ((5, "0x21"), (22, "0x26")):
            for enable in (0, 1):
                with self.subTest(index=index, enable=enable):
                    result = emulator.experiment(self.data, index, enable)
                    self.assertEqual(result["return_code"], "0x0")
                    self.assertEqual(set(result["changed_synthetic_registers"]), {register})
                    change = result["changed_synthetic_registers"][register]
                    self.assertEqual(int(change["before"], 16) ^ int(change["after"], 16), 4)
                    self.assertEqual(bool(int(change["after"], 16) & 4), bool(enable))
                    self.assertEqual([t["direction"] for t in result["transactions"]], ["read", "write"])
                    self.assertFalse(result["vbus_verified"])

    def test_5v_request_is_rejected_without_any_fake_bus_access(self):
        result = emulator.experiment(self.data, 5, 1, voltage=5000)
        self.assertEqual(result["return_code"], "0xe00002c2")
        self.assertEqual(result["transactions"], [])
        self.assertEqual(result["changed_synthetic_registers"], {})
        valid = emulator.experiment(self.data, 5, 1, voltage=3300)
        self.assertEqual(valid["return_code"], "0x0")
        self.assertEqual(valid["changed_synthetic_registers"]["0x34"]["after"], "0xd0")

    def test_transfer_failures_stop_the_firmware_and_change_nothing(self):
        for failure, count in ((0, 1), (1, 2)):
            with self.subTest(failure=failure):
                result = emulator.experiment(self.data, 5, 1, fail_transfer=failure)
                self.assertEqual(result["return_code"], "0xe00002c0")
                self.assertEqual(len(result["transactions"]), count)
                self.assertEqual(result["changed_synthetic_registers"], {})

    def test_shared_option_uses_7c_and_unverified_firmware_is_rejected(self):
        result = emulator.experiment(self.data, 5, 1, options=1)
        self.assertEqual(set(result["changed_synthetic_registers"]), {"0x7c"})
        self.assertEqual(result["changed_synthetic_registers"]["0x7c"]["after"], "0xab")
        with self.assertRaisesRegex(ValueError, "SHA256 mismatch"):
            emulator.experiment(b"unverified firmware", 5, 1)


if __name__ == "__main__":
    unittest.main()
