"""Bounds and dispatch interpretation checks for offline ADT evidence."""
import importlib.util
from pathlib import Path
import struct
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("usb_adt_extract", ROOT / "scripts/extract-usb-adt.py")
extractor = importlib.util.module_from_spec(spec)
spec.loader.exec_module(extractor)


class AdtEvidenceTests(unittest.TestCase):
    def test_malformed_tag_cannot_loop_or_read_past_input(self):
        malformed = (b"ATAD" + struct.pack("<II", 0, 0),
                     b"ATAD" + struct.pack("<II", 100, 16),
                     b"ATAD" + struct.pack("<II", 12, 1))
        for tag in malformed:
            with self.subTest(tag=tag), self.assertRaises(ValueError):
                header = b"3gmI" + struct.pack("<I", 20 + len(tag)) + b"\0" * 12
                extractor.img3_data(header + tag)

    def test_ciphertext_padding_is_kept_until_after_decryption(self):
        # Apple's true DATA length can differ from the AES ciphertext length.
        data = b"3gmI" + struct.pack("<I", 48) + b"\0" * 12 + b"ATAD" + struct.pack("<II", 28, 13) + b"x" * 16
        cipher, size = extractor.img3_data(data)
        self.assertEqual(len(cipher), 16)
        self.assertEqual(size, 13)

    def test_declared_total_and_empty_ciphertext_are_rejected(self):
        for data in (b"3gmI" + b"\0" * 16,
                     b"3gmI" + struct.pack("<I", 32) + b"\0" * 12 + b"ATAD" + struct.pack("<II", 12, 0)):
            with self.subTest(data=data), self.assertRaises(ValueError):
                extractor.img3_data(data)

    def test_adt_rejects_negative_offsets_padding_and_large_counts(self):
        with self.assertRaises(ValueError):
            extractor.dtparse.read_u32(b"\0" * 8, -4)
        truncated_property = b"name\0".ljust(32, b"\0") + struct.pack("<I", 1) + b"x"
        with self.assertRaises(ValueError):
            extractor.dtparse.parse_property(truncated_property, 0, "/")
        with self.assertRaises(ValueError):
            extractor.dtparse.parse_node(struct.pack("<II", 0, 0xffffffff), 0)

    def test_deep_adt_is_rejected_before_python_recursion_limit(self):
        # Empty-property nodes are legal in the binary grammar; adversarial
        # nesting must not depend on Python's process-wide recursion limit.
        data = struct.pack("<II", 0, 1) * 150 + struct.pack("<II", 0, 0)
        with self.assertRaisesRegex(ValueError, "bounded recursion"):
            extractor.dtparse.parse_node(data, 0)

    def test_wide_tree_and_oversized_properties_are_bounded(self):
        # Under the per-node child limit, but above the aggregate safe budget.
        leaf = struct.pack("<II", 0, 0)
        branch = struct.pack("<II", 0, 17) + leaf * 17
        tree = struct.pack("<II", 0, 1000) + branch * 1000
        with self.assertRaisesRegex(ValueError, "bounded total node"):
            extractor.dtparse.parse_node(tree, 0)
        giant = b"blob\0".ljust(32, b"\0") + struct.pack("<I", 0x7fffffff)
        with self.assertRaisesRegex(ValueError, "bounded property size"):
            extractor.dtparse.parse_property(giant, 0, "/")
        with self.assertRaises(ValueError):
            extractor.dtparse.parse_node(leaf, -1)

    def test_arm_io_exports_hsic_without_unrelated_calibration_data(self):
        arm_io = {"props": {"name": b"arm-io\0", "hsic-enabled": b"",
                            "unrelated-calibration": b"secret-looking-test-data"},
                  "children": []}
        result = extractor.select_nodes(arm_io)
        self.assertIn("hsic-enabled", result[0]["props"])
        self.assertNotIn("unrelated-calibration", result[0]["props"])

    def test_duplicate_property_dispatch_is_not_silently_overwritten(self):
        prop = b"name\0".ljust(32, b"\0") + struct.pack("<I", 4) + b"pmu\0"
        with self.assertRaisesRegex(ValueError, "duplicate property"):
            extractor.dtparse.parse_node(struct.pack("<II", 2, 0) + prop + prop, 0)

    def test_charger_dispatch_resolves_pmu_without_inventing_register_writes(self):
        pmu = {"props": {"name": b"pmu\0", "AAPL,phandle": bytes.fromhex("00000028")}, "children": []}
        charger = {"props": {"name": b"charger\0", "function-set_charger": bytes.fromhex("0000002867726863")}, "children": []}
        root = {"props": {"name": b"device-tree\0"}, "children": [pmu, charger]}
        evidence = extractor.select_nodes(root)
        dispatch = evidence[1]["function_refs"]["function-set_charger"]
        self.assertEqual(dispatch["provider_path"], "/device-tree/pmu")
        self.assertEqual(dispatch["fourcc"], "chrg")
        self.assertEqual(dispatch["phandle_le"], "0x28000000")
        self.assertEqual(dispatch["args_le_u32"], [])
        self.assertIn("not proven register writes", dispatch["interpretation"])


if __name__ == "__main__":
    unittest.main()
