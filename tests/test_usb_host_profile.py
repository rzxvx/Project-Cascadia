"""Meaningful safeguards for generating an unproven host research profile."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("usb_host_profile", ROOT / "scripts/prepare-usb-host.py")
profile = importlib.util.module_from_spec(spec)
spec.loader.exec_module(profile)


class HostProfileTests(unittest.TestCase):
    def test_profile_enables_storage_and_hid_without_duplicate_peripheral(self):
        text = profile.host_config((ROOT / "config/p105ap.config").read_text())
        self.assertNotIn("CONFIG_USB_DWC2_PERIPHERAL=y", text)
        for symbol in ("USB_DWC2_HOST", "USB_STORAGE", "SCSI", "BLK_DEV_SD", "USB_HID"):
            self.assertIn(f"CONFIG_{symbol}=y\n", text)
            self.assertNotIn(f"# CONFIG_{symbol} is not set", text)
        self.assertNotIn("CONFIG_USB_GADGET=y", text)

    def test_only_dwc2_role_changes_no_power_source(self):
        original = (ROOT / "dts/p105ap.dts").read_text()
        generated = profile.host_dts(original)
        expected = original.replace('dr_mode = "peripheral";', 'dr_mode = "host";')
        self.assertEqual(generated[-len(expected):], expected)
        self.assertNotIn("vbus-supply", generated)

    def test_unreviewed_upstream_role_or_vbus_fails_closed(self):
        original = (ROOT / "dts/p105ap.dts").read_text()
        for replacement in ('dr_mode = "otg";', 'dr_mode = "peripheral"; vbus-supply = <&new_rail>;'):
            with self.assertRaises(ValueError):
                profile.host_dts(original.replace('dr_mode = "peripheral";', replacement))

    def test_cannot_overwrite_reference_inputs_or_previous_results(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "config").mkdir()
            (root / "dts").mkdir()
            config = "CONFIG_USB_DWC2_PERIPHERAL=y\n"
            dts = (ROOT / "dts/p105ap.dts").read_text()
            (root / "config/p105ap.config").write_text(config)
            (root / "dts/p105ap.dts").write_text(dts)
            with self.assertRaises(ValueError):
                profile.prepare(root, root / "config")
            output = root / "build/usb-host-research/profile"
            profile.prepare(root, output)
            with self.assertRaises(ValueError):
                profile.prepare(root, output)
            self.assertEqual((root / "config/p105ap.config").read_text(), config)
            self.assertEqual((root / "dts/p105ap.dts").read_text(), dts)


if __name__ == "__main__":
    unittest.main()
