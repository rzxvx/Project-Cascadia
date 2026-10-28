#!/usr/bin/env python3
"""Synthetic PPN decoder boundaries/oracle; no firmware, MMIO, ioctl or device."""

from dataclasses import FrozenInstanceError
import importlib.util
from pathlib import Path
import random
import struct
import sys
import unittest


PATH = Path(__file__).resolve().parents[1] / 'tools/nand/ppn_error_list.py'
spec = importlib.util.spec_from_file_location('ppn_error_list', PATH)
ppn = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = ppn
spec.loader.exec_module(ppn)


def buffer(pages=(), *, count=None, padding=0):
    data = bytearray([padding] * 512)
    struct.pack_into('<I', data, 0, len(pages) if count is None else count)
    for index, page in enumerate(pages):
        struct.pack_into('<I', data, 4 + index * 4, page)
    return data


def decode(payload, request=(), ce=0, status=0):
    return ppn.decode_error_list(payload, request, ce_index=ce,
                                 context_status=status)


def oracle(pages, records, ce):
    # Independent O(n^2) reference, using original Python tuples rather than
    # decoder output, bytes offsets, sets/dictionaries, or decoder constants.
    keys = [(record[0], record[1]) for record in records]
    if any(keys[i] in keys[:i] for i in range(len(keys))):
        return None
    if len(pages) > 127 or any(pages[i] in pages[:i] for i in range(len(pages))):
        return None
    result = []
    for page in pages:
        matches = [record for record in records if record[:2] == (page, ce)]
        if len(matches) != 1:
            return None
        result.append(matches[0])
    return result


class DecoderTests(unittest.TestCase):
    def test_zero_count_empty_request(self):
        self.assertEqual(decode(buffer()).entries, ())

    def test_all_page_and_ce_boundaries_are_raw(self):
        for ce in (0, 255):
            for page in (0, 0xffffffff):
                with self.subTest(ce=ce, page=page):
                    entry = ppn.RequestEntry(page, ce, 255)
                    result = decode(buffer([page]), [entry], ce, 254)
                    self.assertEqual(result.entries, (entry,))
                    self.assertEqual((result.ce_index, result.context_status), (ce, 254))

    def test_127_complete_entries(self):
        pages = [i * 131071 for i in range(127)]
        request = [ppn.RequestEntry(page, 7, i & 255)
                   for i, page in enumerate(pages)]
        self.assertEqual(decode(buffer(pages), request, 7).entries, tuple(request))

    def test_length_boundaries(self):
        for length in (0, 1, 4, 511, 513, 1024):
            with self.subTest(length=length), self.assertRaises(ppn.DecodeError):
                decode(bytes(length))

    def test_payload_type_strict(self):
        for payload in (None, True, 512, '0' * 512, [0] * 512,
                        memoryview(bytes(512))):
            with self.subTest(type=type(payload)), self.assertRaises(ppn.DecodeError):
                decode(payload)

    def test_count_overflow_before_request_iteration(self):
        # Invalid count must be rejected before looking at any request record.
        for count in (128, 129, 65535, 0xffffffff):
            with self.subTest(count=count), self.assertRaisesRegex(ppn.DecodeError, 'count'):
                decode(buffer(count=count), [object()])

    def test_count_little_endian(self):
        data = buffer([42])
        self.assertEqual(data[:4], bytes([1, 0, 0, 0]))
        self.assertEqual(decode(data, [ppn.RequestEntry(42, 0, 0)]).entries[0].page, 42)
        data[:4] = bytes([0, 0, 0, 1])
        with self.assertRaisesRegex(ppn.DecodeError, 'count'):
            decode(data)

    def test_result_order_preserved(self):
        request = [ppn.RequestEntry(i, 0, i) for i in (1, 2, 3)]
        self.assertEqual([r.page for r in decode(buffer([3, 1]), request).entries], [3, 1])

    def test_unused_padding_is_opaque(self):
        request = [ppn.RequestEntry(9, 0, 0)]
        self.assertEqual(decode(buffer([9], padding=255), request).entries, tuple(request))
        self.assertEqual(decode(buffer(padding=255)).entries, ())

    def test_duplicate_result(self):
        with self.assertRaisesRegex(ppn.DecodeError, 'duplicate result'):
            decode(buffer([9, 9]), [ppn.RequestEntry(9, 0, 0)])

    def test_duplicate_request_key_even_for_empty_result(self):
        for statuses in ((0, 0), (0, 255)):
            with self.subTest(statuses=statuses), self.assertRaisesRegex(ppn.DecodeError, 'duplicate request'):
                decode(buffer(), [ppn.RequestEntry(9, 0, status) for status in statuses])

    def test_same_page_other_ce_allowed_and_selected_exactly(self):
        request = [ppn.RequestEntry(9, 1, 123), ppn.RequestEntry(9, 2, 234)]
        self.assertEqual(decode(buffer([9]), request, 2).entries, (request[1],))

    def test_foreign_ce_is_not_a_match(self):
        with self.assertRaisesRegex(ppn.DecodeError, 'no exact'):
            decode(buffer([9]), [ppn.RequestEntry(9, 1, 0)], 2)

    def test_missing_page_refuses_all_partial_matches(self):
        with self.assertRaisesRegex(ppn.DecodeError, 'no exact'):
            decode(buffer([1, 2]), [ppn.RequestEntry(1, 0, 0)])

    def test_every_status_byte_preserved_without_or(self):
        for status in range(256):
            with self.subTest(status=status):
                entry = ppn.RequestEntry(8, 0, status)
                result = decode(buffer([8]), [entry], status=255)
                self.assertEqual(result.entries[0].raw_status, status)
                self.assertEqual(result.context_status, 255)
                self.assertEqual(entry.raw_status, status)

    def test_invalid_integer_fields(self):
        for field, bits in (('page', 32), ('ce_index', 8), ('raw_status', 8)):
            for value in (-1, 1 << bits, True, False, 0.0, '0', None):
                kwargs = dict(page=0, ce_index=0, raw_status=0)
                kwargs[field] = value
                with self.subTest(field=field, value=value), self.assertRaises(ppn.DecodeError):
                    ppn.RequestEntry(**kwargs)

    def test_context_fields_strict(self):
        for field in ('ce_index', 'context_status'):
            for value in (-1, 256, True, False, 0.0, None):
                kwargs = dict(ce_index=0, context_status=0)
                kwargs[field] = value
                with self.subTest(field=field, value=value), self.assertRaises(ppn.DecodeError):
                    ppn.decode_error_list(buffer(), (), **kwargs)

    def test_request_and_record_types_strict(self):
        for request in (None, {}, set(), iter([]), True, bytes(512)):
            with self.subTest(type=type(request)), self.assertRaises(ppn.DecodeError):
                decode(buffer(), request)
        for entry in (None, True, {}, (1, 0, 0), [1, 0, 0]):
            with self.subTest(entry=entry), self.assertRaises(ppn.DecodeError):
                decode(buffer(), [entry])

    def test_tampered_record_revalidated(self):
        entry = ppn.RequestEntry(1, 0, 0)
        object.__setattr__(entry, 'page', True)
        with self.assertRaises(ppn.DecodeError):
            decode(buffer(), [entry])

    def test_snapshots_do_not_follow_mutable_inputs(self):
        payload = buffer([1])
        entry = ppn.RequestEntry(1, 0, 4)
        request = [entry]
        result = decode(payload, request)
        payload[:] = b'\xff' * 512
        request.clear()
        object.__setattr__(entry, 'raw_status', 255)
        self.assertEqual(result.entries, (ppn.RequestEntry(1, 0, 4),))
        with self.assertRaises(FrozenInstanceError):
            result.ce_index = 1
        with self.assertRaises(FrozenInstanceError):
            result.entries[0].raw_status = 1

    def test_normalized_request_not_limited_to_list_buffer_capacity(self):
        request = [ppn.RequestEntry(i, 0, i % 256) for i in range(300)]
        self.assertEqual(decode(buffer([299]), request).entries, (request[299],))

    def test_independent_pseudorandom_oracle(self):
        rng = random.Random(0x50504e)
        for case in range(600):
            ce = rng.randrange(4)
            records = [(rng.randrange(32), rng.randrange(4), rng.randrange(256))
                       for _ in range(rng.randrange(30))]
            mode = case % 4
            if mode == 0:
                # Guaranteed valid requests/results mixed with other CEs.
                records = list(dict(((p, c), (p, c, s)) for p, c, s in records).values())
                pages = [p for p, c, s in records if c == ce]
                rng.shuffle(pages)
                pages = pages[:rng.randrange(len(pages) + 1)]
            else:
                pages = [rng.randrange(32) for _ in range(rng.randrange(15))]
            expected = oracle(pages, records, ce)
            request = [ppn.RequestEntry(*record) for record in records]
            with self.subTest(case=case):
                if expected is None:
                    with self.assertRaises(ppn.DecodeError):
                        decode(buffer(pages, padding=165), request, ce, 255)
                else:
                    actual = decode(buffer(pages, padding=165), request, ce, 255)
                    self.assertEqual([(r.page, r.ce_index, r.raw_status) for r in actual.entries], expected)


if __name__ == '__main__':
    unittest.main()
