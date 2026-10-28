#!/usr/bin/env python3
"""Synthetic offline NAND recovery tests: no MMIO, MTD, NBD or real dumps."""
import contextlib
import importlib.util
import io
from pathlib import Path
import struct
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

PATH = Path(__file__).resolve().parents[1] / 'tools/nand-ftl-scan.py'
spec = importlib.util.spec_from_file_location('nand_ftl_scan', PATH)
nand = importlib.util.module_from_spec(spec)
spec.loader.exec_module(nand)


def raw_page(lba, seq, value):
    raw = bytearray(b'\xff' * nand.PAGE)
    off = 0
    raw[off:off + 1024] = bytes([value]) * 1024
    raw[off + 1040:off + nand.CHUNK] = bytes([value]) * 3072
    meta = bytearray(16)
    meta[0] = 1
    meta[2:8] = seq.to_bytes(6, 'little')
    struct.pack_into('<I', meta, 8, lba)
    struct.pack_into('<I', meta, 12, seq >> 16)
    raw[1024:1040] = meta
    return raw


def lwvm_header(chunks=(0, 1), size=None):
    header = bytearray(4096)
    header[:16] = nand.LWVM_MAGIC
    struct.pack_into('<Q', header, 0x20, nand.NLBA * 4096)
    struct.pack_into('<I', header, 0x28, 1)
    size = len(chunks) * nand.LWVM_CHUNK if size is None else size
    struct.pack_into('<QQ', header, 0x220, 0, size)
    name = 'System'.encode('utf-16le')
    header[0x238:0x238 + len(name)] = name
    for p in range(1024):
        struct.pack_into('<H', header, 0x800 + p * 2, 0xf3ff)
    for p, logical in enumerate(chunks, 1):
        struct.pack_into('<H', header, 0x800 + p * 2, logical)
    return header


class FakeDisk:
    def __init__(self, header):
        self.header = header

    def read4k(self, lba):
        return self.header


class RecoveryTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.directory = Path(self.tmp.name)
        self.raw = self.directory / 'bus0-cau0.bin'
        self.status = Path(str(self.raw) + '.st')
        self.map = self.directory / 'map'
        self.limit = mock.patch.object(nand, 'NLBA', 8)
        self.limit.start()
        self.addCleanup(self.limit.stop)

    def dump(self, pages, statuses):
        self.raw.write_bytes(b''.join(pages))
        self.status.write_bytes(bytes(statuses))

    def scan(self):
        with contextlib.redirect_stderr(io.StringIO()):
            nand.scan(str(self.directory), str(self.map))

    def test_refresh_page_wins_over_older_good_page(self):
        self.dump([raw_page(3, 10, 0x11), raw_page(3, 20, 0x22)], [0x40, 0x42])
        self.scan()
        with nand.Disk(str(self.directory), str(self.map)) as disk:
            self.assertEqual(disk.read4k(3), b'\x22' * 4096)
            self.assertIsNone(disk.read4k(2))

    def test_unreadable_newer_page_does_not_replace_good_copy(self):
        self.dump([raw_page(3, 10, 0x11), raw_page(3, 20, 0x22)], [0x40, 0x45])
        self.scan()
        with nand.Disk(str(self.directory), str(self.map)) as disk:
            self.assertEqual(disk.read4k(3), b'\x11' * 4096)

    def test_truncated_dump_rejected_before_map_creation(self):
        self.dump([raw_page(3, 10, 1)[:-1]], [0x40])
        with self.assertRaisesRegex(ValueError, 'truncated'):
            self.scan()
        self.assertFalse(self.map.exists())

    def test_missing_or_extra_status_rejected(self):
        for statuses in ([], [0x40, 0x40]):
            self.dump([raw_page(3, 10, 1)], statuses)
            with self.assertRaisesRegex(ValueError, 'status count'):
                self.scan()

    def test_no_dump_is_error(self):
        with self.assertRaisesRegex(ValueError, 'no NAND dump'):
            self.scan()

    def test_map_is_not_overwritten(self):
        self.dump([raw_page(3, 10, 1)], [0x40])
        self.map.write_bytes(b'keep')
        with self.assertRaises(FileExistsError):
            self.scan()
        self.assertEqual(self.map.read_bytes(), b'keep')

    def test_incorrect_map_size_rejected(self):
        self.dump([raw_page(3, 10, 1)], [0x40])
        self.map.write_bytes(b'\0' * 12)
        with self.assertRaisesRegex(ValueError, 'map length'):
            nand.Disk(str(self.directory), str(self.map))

    def test_metadata_change_detects_stale_map(self):
        for changed in (raw_page(4, 10, 1), raw_page(3, 11, 1)):
            self.dump([raw_page(3, 10, 1)], [0x40])
            self.scan()
            self.raw.write_bytes(changed)
            with nand.Disk(str(self.directory), str(self.map)) as disk:
                with self.assertRaisesRegex(ValueError, 'stale map'):
                    disk.read4k(3)
            self.map.unlink()

    def test_missing_file_index_rejected(self):
        self.dump([raw_page(3, 10, 1)], [0x40])
        self.scan()
        with self.map.open('r+b') as f:
            f.seek(3 * 12)
            f.write(struct.pack('<qI', 10, 1 << 28))
        with nand.Disk(str(self.directory), str(self.map)) as disk:
            with self.assertRaisesRegex(ValueError, 'missing dump'):
                disk.read4k(3)

    def test_read_checks_status_again(self):
        self.dump([raw_page(3, 10, 1)], [0x40])
        self.scan()
        self.status.write_bytes(b'\x45')
        with nand.Disk(str(self.directory), str(self.map)) as disk:
            with self.assertRaisesRegex(ValueError, 'unreadable'):
                disk.read4k(3)

    def test_lba_and_image_ranges(self):
        for start, count in ((-1, 1), (0, -1), (8, 1), (7, 2)):
            with self.assertRaisesRegex(ValueError, 'range'):
                nand.checked_range(start, count)
        nand.checked_range(8, 0)

    def test_real_cli_help_and_invalid_command(self):
        result = subprocess.run([sys.executable, str(PATH), '--help'], capture_output=True)
        self.assertEqual(result.returncode, 0)
        result = subprocess.run([sys.executable, str(PATH), 'unknown'], capture_output=True)
        self.assertEqual(result.returncode, 2)

    def test_partition_export_preserves_exact_non_chunk_aligned_size(self):
        # Real FTL pages + map + CLI production path, with only a 1,234-byte
        # partition in physical allocation chunk 1.  No multi-GB fixture.
        with mock.patch.object(nand, 'NLBA', 5000):
            h = lwvm_header(chunks=(0,), size=1234)
            header_raw = raw_page(0, 10, 0)
            header_raw[:1024] = h[:1024]
            header_raw[1040:nand.CHUNK] = h[1024:]
            self.dump([header_raw, raw_page(4096, 20, 0x55)], [0x40, 0x42])
            self.scan()
            out = self.directory / 'system.img'
            args = [str(PATH), 'part', str(self.directory), str(self.map), str(out), 'System']
            with mock.patch.object(sys, 'argv', args), contextlib.redirect_stderr(io.StringIO()):
                nand.main()
            self.assertEqual(out.read_bytes(), b'\x55' * 1234)
            self.assertEqual(stat.S_IMODE(out.stat().st_mode), 0o600)
            with mock.patch.object(sys, 'argv', args), self.assertRaises(FileExistsError):
                nand.main()
            self.assertEqual(out.read_bytes(), b'\x55' * 1234)

    def test_failed_late_read_does_not_publish_partial_image(self):
        self.dump([raw_page(0, 10, 0x11), raw_page(1, 20, 0x22)], [0x40, 0x40])
        self.scan()
        self.assertEqual(stat.S_IMODE(self.map.stat().st_mode), 0o600)
        # First LBA exports correctly; the second fails after bytes were
        # written.  The final filename must still not exist.
        self.raw.write_bytes(raw_page(0, 10, 0x11) + raw_page(2, 20, 0x22))
        out = self.directory / 'recovered.img'
        args = [str(PATH), 'image', str(self.directory), str(self.map), str(out), '0', '2']
        with mock.patch.object(sys, 'argv', args), self.assertRaisesRegex(ValueError, 'stale map'):
            nand.main()
        self.assertFalse(out.exists())
        self.assertEqual(list(self.directory.glob('.recovered.img.*')), [])

    def test_atomic_output_refuses_racing_creator(self):
        out = self.directory / 'result'
        with self.assertRaises(FileExistsError):
            with nand.new_output(str(out)) as f:
                f.write(b'new')
                out.write_bytes(b'keep')
        self.assertEqual(out.read_bytes(), b'keep')
        self.assertEqual(list(self.directory.glob('.result.*')), [])


class LwVMTests(unittest.TestCase):
    def test_valid_partition_and_exact_byte_size(self):
        size = nand.LWVM_CHUNK + 1234
        self.assertEqual(nand.lwvm_info(FakeDisk(lwvm_header(size=size)), 'System'),
                         ([1, 2], size))

    def test_bad_or_missing_header(self):
        for header in (None, b'', b'\0' * 4096):
            with self.assertRaisesRegex(ValueError, 'LwVM header'):
                nand.lwvm_part(FakeDisk(header), 'System')

    def test_partition_count_cannot_overlap_chunk_table(self):
        for count in (0, 13, 0xffffffff):
            header = lwvm_header()
            struct.pack_into('<I', header, 0x28, count)
            with self.assertRaisesRegex(ValueError, 'partition count'):
                nand.lwvm_part(FakeDisk(header), 'System')

    def test_other_capacity_is_rejected_explicitly(self):
        for size in (0, 32000000000):
            header = lwvm_header()
            struct.pack_into('<Q', header, 0x20, size)
            with self.assertRaisesRegex(ValueError, 'unsupported LwVM media size'):
                nand.lwvm_part(FakeDisk(header), 'System')

    def test_duplicate_gap_and_out_of_range_logical_chunks(self):
        for chunks in ((0, 0), (0, 2), (1024,), ()):
            with self.assertRaisesRegex(ValueError, 'chunk'):
                nand.lwvm_part(FakeDisk(lwvm_header(chunks)), 'System')

    def test_invalid_sizes(self):
        for size in (0, 2 * nand.LWVM_CHUNK + 1):
            with self.assertRaisesRegex(ValueError, 'partition size'):
                nand.lwvm_part(FakeDisk(lwvm_header(size=size)), 'System')

    def test_missing_partition(self):
        with self.assertRaisesRegex(ValueError, 'no LwVM partition'):
            nand.lwvm_part(FakeDisk(lwvm_header()), 'Data')


if __name__ == '__main__':
    unittest.main()
