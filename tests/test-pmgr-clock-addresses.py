"""Packed P105 clocks and exact pre-fix mapper control; no firmware/device."""
import contextlib
import hashlib
import importlib.util
import io
from pathlib import Path
import struct
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("mapper", ROOT / "scripts/adt-pmgr-map.py")
mapper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mapper)


def node(name, props, children):
    props = {"name": name.encode() + b"\0", **props}
    raw = struct.pack("<II", len(props), len(children))
    for key, value in props.items():
        raw += key.encode().ljust(32, b"\0") + struct.pack("<I", len(value)) + value + b"\0" * (-len(value) % 4)
    return raw + b"".join(children)


class Clocks(unittest.TestCase):
    def test_real_historical_mapper_misplaces_USBREG_clock(self):
        raw = (ROOT / "tests/fixtures/p105-pmgr-clock/historical-44-layout-map.py").read_bytes()
        self.assertEqual(hashlib.sha1(b"blob " + str(len(raw)).encode() + b"\0" + raw).hexdigest(),
                         "1887bed05c8b485fbde22aeb4fce35c6a29ad122")
        historical = {"__name__": "historical_control"}
        exec(compile(raw, "historical-map.py", "exec"), historical)
        row = struct.pack("<7I", 42 << 24, 0, 40, 38, 45, 0, 0) + b"USBREG".ljust(16, b"\0")
        tree = node("device-tree", {}, [node("arm-io", {}, [node("pmgr", {"device-clocks": row}, [])])])
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "synthetic.raw"
            path.write_bytes(tree)
            old_args = sys.argv
            try:
                for entry, expected in ((historical["main"], "3f1000b4"), (mapper.main, "3f1000ec")):
                    sys.argv = ["mapper", str(path)]
                    output = io.StringIO()
                    with contextlib.redirect_stdout(output):
                        entry()
                    line = next(line for line in output.getvalue().splitlines() if line.startswith("USBREG "))
                    self.assertIn(expected, line)
            finally:
                sys.argv = old_args

    def test_packed_direct_clocks_and_encoded_entries_are_distinguished(self):
        self.assertEqual(mapper.clock_registers(0x00332d01), ((0x3f10003c, 0x3f1000ec, 0x3f100104), None))
        self.assertEqual(mapper.clock_registers(0), ((), None))
        self.assertEqual(mapper.clock_registers(0x6d), ((), "encoded:6d"))
        self.assertEqual(mapper.clock_registers(0x3400), ((), "invalid"))
        self.assertEqual(mapper.clock_registers(45, 36), ((), "unverified"))
        for packed in (-1, 1 << 32):
            with self.assertRaises(ValueError):
                mapper.clock_registers(packed)


if __name__ == "__main__":
    unittest.main()
