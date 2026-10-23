"""Actual THS7383 firmware readers against deliberately asymmetric fake replies."""
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
    spec = importlib.util.spec_from_file_location("tristar_id_emulator", ROOT / "scripts/emulate-tristar-id.py")
    emulator = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(emulator)


@unittest.skipUnless(unicorn is not None and CACHE.exists(), "requires local official firmware and build-image Unicorn")
class TristarIdTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.data = CACHE.read_bytes()
        cls.reply = bytes.fromhex("66554433221175")

    def test_real_firmware_reverses_six_bytes_for_either_status_orientation(self):
        for status in (0x80, 0xC0, 0xFF):
            with self.subTest(status=status):
                result = emulator.experiment(self.data, "id-response", self.reply, status=status)
                self.assertEqual(result["return_code"], "0x0")
                self.assertEqual(result["output"], "112233445566")
                self.assertEqual([(t["register"], t["count"]) for t in result["transactions"]],
                                 [("0x39", 7), ("0xc", 1)])
                self.assertFalse(result["role_or_authentication_verified"])

    def test_not_ready_and_invalid_marker_never_publish_uninitialized_id(self):
        for status in (0, 0x40, 0x7F):
            with self.subTest(status=status):
                result = emulator.experiment(self.data, "id-response", self.reply, status=status)
                self.assertEqual(result["return_code"], "0xe00002d9")
                self.assertEqual(result["output"], "a5" * 6)
        result = emulator.experiment(self.data, "id-response", self.reply[:-1] + b"\x74")
        self.assertEqual(result["return_code"], "0xe00002c6")
        self.assertEqual(result["output"], "a5" * 6)
        self.assertEqual(len(result["stubbed_logs"]), 1)

    def test_either_transfer_failure_preserves_output_and_short_circuits(self):
        for failure in (0, 1):
            result = emulator.experiment(self.data, "id-response", self.reply, fail_transfer=failure)
            self.assertEqual(result["return_code"], "0xe00002c0")
            self.assertEqual(result["output"], "a5" * 6)
            self.assertEqual(len(result["transactions"]), failure + 1)

    def test_system_id_marker_and_bitpacking_from_firmware(self):
        result = emulator.experiment(self.data, "system-id", bytes.fromhex("743f01"))
        self.assertEqual(result["return_code"], "0x0")
        self.assertEqual(result["output"], "fd00")
        invalid = emulator.experiment(self.data, "system-id", bytes.fromhex("753f01"))
        self.assertEqual(invalid["return_code"], "0xe00002c6")
        self.assertEqual(invalid["output"], "a5a5")

    def test_invalid_inputs_and_unpinned_firmware_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "SHA256 mismatch"):
            emulator.experiment(b"unverified", "id-response", self.reply)
        with self.assertRaisesRegex(ValueError, "exactly 7"):
            emulator.experiment(self.data, "id-response", self.reply[:-1])
        with self.assertRaisesRegex(ValueError, "only the two"):
            emulator.experiment(self.data, "write-register", self.reply)


if __name__ == "__main__":
    unittest.main()
