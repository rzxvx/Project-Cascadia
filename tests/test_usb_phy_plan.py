"""The PHY evidence tool must reject drift and never advertise deployability."""
import importlib.util
from pathlib import Path
import struct
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("usb_phy_plan", ROOT / "scripts/usb-phy-plan.py")
planner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(planner)


class PhyPlanTests(unittest.TestCase):
    def test_macho_command_bounds_and_negative_offsets(self):
        with self.assertRaises(ValueError):
            planner.macho(b"\0" * 28, -1)
        header = struct.pack("<7I", 0xFEEDFACE, 0, 0, 0, 1, 8, 0)
        for command in (struct.pack("<2I", 1, 0), struct.pack("<2I", 1, 100)):
            with self.subTest(command=command), self.assertRaises(ValueError):
                planner.macho(header + command)

    def test_wrong_firmware_rejected_before_address_interpretation(self):
        with self.assertRaisesRegex(ValueError, "kernelcache SHA256 mismatch"):
            planner.plan(b"unverified firmware", b"unverified ADT")

    def test_real_pinned_evidence_resolves_hsic_but_keeps_power_and_role_conditions(self):
        cache = ROOT / "build/firmware/kernelcache.12H321.macho"
        tree = ROOT / "build/usb-host-research/adt-evidence-v3/DeviceTree.p105ap.raw"
        if not cache.exists() or not tree.exists():
            self.skipTest("official firmware evidence is deliberately not shipped in Git")
        result = planner.plan(cache.read_bytes(), tree.read_bytes())
        self.assertFalse(result["executable"])
        self.assertFalse(result["device_access"])
        self.assertFalse(result["vbus_enable_known"])
        self.assertEqual(result["source"]["phy_file_offset"], "0xc63000")
        conditional = result["observed_enable_host"][-1]
        self.assertEqual(conditional["condition"], "AppleUSBPhy hsic_enabled at object+0x50 == 0")
        self.assertTrue(result["hsic_state"]["adt_hsic_enabled_property_present"])
        self.assertTrue(result["hsic_state"]["default_firmware_skips_register_0x64_bit1"])
        self.assertFalse(result["hsic_state"]["live_state_verified"])
        roles = {r["apple_cable_type"]: r for r in result["controller_role_evidence"]["cable_types"]}
        self.assertEqual(roles["USBHost"]["ipad_role"], "peripheral")
        self.assertEqual(roles["USBHost"]["symbol_global"], "0x804daedc")
        self.assertEqual(roles["USBDevice"]["ipad_role"], "host")
        self.assertEqual(roles["USBDevice"]["usb_complex_control_bit0"], 1)
        self.assertEqual(roles["USBDeviceNeedsAuthentication"]["symbol_global"], "0x804daee8")
        self.assertFalse(roles["USBDevice"]["role_usable_or_vbus_verified"])
        rails = result["accessory_rail_evidence"]["rails"]
        self.assertEqual(rails[0]["voltage_maximum_firmware_units"], 3300)
        self.assertEqual(rails[0]["enable_register"], "0x21")
        self.assertEqual(rails[0]["enable_mask"], "0x4")
        self.assertEqual(rails[1]["enable_register"], "0x26")
        self.assertFalse(rails[0]["is_verified_5v_vbus_source"])
        tristar = result["tristar_identification_evidence"]
        self.assertEqual(tristar["instance_vtable"], "0x80889298")
        self.assertEqual(tristar["id_report"]["ioaccessory_manager_target"], "0x807139b8")
        self.assertEqual(tristar["usb_dock_bridge"]["instance_vtable"], "0x80888998")
        self.assertEqual(tristar["usb_dock_bridge"]["target_thumb"], "0x8088586d")
        self.assertFalse(tristar["usb_dock_bridge"]["complete_power_permission_policy_verified"])
        self.assertEqual(tristar["id_response"]["register_read_bytes"], 7)
        self.assertFalse(tristar["camera_adapter_id_known"])
        self.assertFalse(tristar["data_routing_modes_are_adapter_ids"])
        unknown = [t for t in tristar["state_transitions"] if t["status"].startswith("UNRESOLVED")]
        self.assertEqual(len(unknown), 2)
        delays = [step for step in result["observed_enable_host"] if step["operation"] == "delay_us"]
        self.assertEqual([step["value"] for step in delays], [20, 1000])
        for step in delays:
            self.assertEqual(step["condition"], "object powerLevel >= 2")


if __name__ == "__main__":
    unittest.main()
