"""Actual shared ADT parser against malformed and legal synthetic trees."""
import importlib.util
from pathlib import Path
import struct
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("dtparse", ROOT / "src/dtparse.py")
dtparse = importlib.util.module_from_spec(spec)
spec.loader.exec_module(dtparse)


def prop(name, data):
    return name.encode().ljust(32, b"\0") + struct.pack("<I", len(data)) + data + b"\0" * (-len(data) % 4)


class Bounds(unittest.TestCase):
    def test_legal_unaligned_property_and_child_are_preserved(self):
        child = struct.pack("<II", 1, 0) + prop("name", b"pmgr\0")
        raw = struct.pack("<II", 2, 1) + prop("name", b"device-tree\0") + prop("bytes", b"abc") + child
        tree, end = dtparse.parse_node(raw, 0)
        self.assertEqual(end, len(raw))
        self.assertEqual(tree["props"]["bytes"], b"abc")
        self.assertEqual(tree["children"][0]["props"]["name"], b"pmgr\0")

    def test_negative_offsets_and_incomplete_padding_refuse(self):
        with self.assertRaises(ValueError):
            dtparse.read_u32(b"\0" * 8, -4)
        with self.assertRaises(ValueError):
            dtparse.parse_node(struct.pack("<II", 0, 0), -1)
        with self.assertRaisesRegex(ValueError, "padding"):
            dtparse.parse_property(prop("name", b"x")[:-3], 0, "/")

    def test_duplicate_dispatch_is_not_overwritten(self):
        raw = struct.pack("<II", 2, 0) + prop("name", b"pmu\0") + prop("name", b"other\0")
        with self.assertRaisesRegex(ValueError, "duplicate property"):
            dtparse.parse_node(raw, 0)

    def test_deep_input_fails_before_python_recursion_limit(self):
        raw = struct.pack("<II", 0, 1) * 150 + struct.pack("<II", 0, 0)
        with self.assertRaisesRegex(ValueError, "bounded recursion"):
            dtparse.parse_node(raw, 0)

    def test_aggregate_node_budget_bounds_legal_per_node_counts(self):
        leaf = struct.pack("<II", 0, 0)
        branch = struct.pack("<II", 0, 17) + leaf * 17
        with self.assertRaisesRegex(ValueError, "bounded total node"):
            dtparse.parse_node(struct.pack("<II", 0, 1000) + branch * 1000, 0)

    def test_oversized_property_and_child_count_refuse(self):
        giant = b"blob\0".ljust(32, b"\0") + struct.pack("<I", 0x7fffffff)
        with self.assertRaisesRegex(ValueError, "bounded property size"):
            dtparse.parse_property(giant, 0, "/")
        with self.assertRaises(ValueError):
            dtparse.parse_node(struct.pack("<II", 0, 0xffffffff), 0)


if __name__ == "__main__":
    unittest.main()
