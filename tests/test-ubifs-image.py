"""Offline file-builder safety checks; no mtd-utils, mount or device needed."""
import importlib.util
from pathlib import Path
import os
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("ubifs_image", ROOT / "scripts/prepare-ubifs-image.py")
image = importlib.util.module_from_spec(spec)
spec.loader.exec_module(image)


class ImageContract(unittest.TestCase):
    def test_explicit_whole_page_geometry(self):
        geo = image.geometry(dict(purpose="offline-image-only", min_io=16384,
                                  peb_size=2097152, volume_lebs=64))
        self.assertEqual((geo["subpage_size"], geo["vid_offset"],
                          geo["data_offset"], geo["leb_size"]),
                         (16384, 16384, 32768, 2064384))

    def test_invalid_or_implicit_geometry_is_rejected(self):
        good = dict(purpose="offline-image-only", min_io=16384,
                    peb_size=2097152, volume_lebs=64)
        bad = [None, [], {}, {**good, "purpose": "install"},
               {**good, "min_io": True}, {**good, "min_io": 16383},
               {**good, "peb_size": 2097153}, {**good, "volume_lebs": -1},
               {**good, "volume_lebs": 65537}, {**good, "device": "/dev/mtd0"}]
        for value in bad:
            with self.subTest(value=value), self.assertRaises(ValueError):
                image.geometry(value)

    def test_kernel_crc_convention(self):
        self.assertEqual(image.media_crc(b"123456789"), 0x340bc6d9)

    def test_source_modes_are_part_of_the_identity(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / "init"
            source.write_text("#!/bin/sh\n")
            first = image.rootfs_digest(root)
            source.chmod(0o700)
            self.assertNotEqual(image.rootfs_digest(root), first)

    def test_symlink_is_not_followed_and_special_file_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "link").symlink_to("/does-not-exist")
            self.assertEqual(len(image.rootfs_digest(root)), 64)
            os.mkfifo(root / "named-pipe")
            with self.assertRaises(ValueError):
                image.rootfs_digest(root)


if __name__ == "__main__":
    unittest.main()
