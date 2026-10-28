#!/usr/bin/env python3
"""Synthetic multi-list invariants/oracle; no firmware/device/kernel API."""

from dataclasses import FrozenInstanceError
from pathlib import Path
import random
import struct
import sys
import unittest


TOOLS = Path(__file__).resolve().parents[1] / 'tools/nand'
sys.path.insert(0, str(TOOLS))
from ppn_error_list import DecodeError, RequestEntry
from ppn_observations import Observation, RequestSnapshot, reconcile_observations


def scratch(pages=(), count=None):
    data = bytearray([165] * 512)
    struct.pack_into('<I', data, 0, len(pages) if count is None else count)
    for i, page in enumerate(pages):
        struct.pack_into('<I', data, 4 + 4 * i, page)
    return data


def request(generation=0, records=((1, 0, 128), (2, 0, 0))):
    return RequestSnapshot(generation, [RequestEntry(*record) for record in records])


def observe(snapshot, ordinal=0, pages=(1,), ce=0, status=255):
    return Observation(snapshot, ordinal, scratch(pages), ce, status)


class ObservationTests(unittest.TestCase):
    def test_no_observations_and_empty_request(self):
        view = reconcile_observations(request(records=()), [])
        self.assertEqual((view.pages, view.observations), ((), ()))

    def test_absent_pages_always_unknown(self):
        snap = request()
        for observations in ([], [observe(snap, pages=())], [observe(snap)]):
            with self.subTest(observations=len(observations)):
                view = reconcile_observations(snap, observations)
                self.assertEqual(view.pages[1].presence, 'unknown')
                self.assertEqual(view.pages[1].observations, ())
                self.assertEqual(view.pages[1].request_entry.raw_status, 0)

    def test_overlap_preserves_individual_raw_statuses_without_or(self):
        snap = request()
        first = observe(snap, ordinal=8, status=1)
        second = observe(snap, ordinal=3, status=4)
        view = reconcile_observations(snap, [first, second])
        self.assertEqual(view.pages[0].presence, 'listed')
        self.assertEqual([(o.ordinal, o.context_status, o.raw_status)
                          for o in view.pages[0].observations], [(8, 1, 128), (3, 4, 128)])
        self.assertEqual([o.ordinal for o in view.observations], [8, 3])

    def test_equal_payload_distinct_ordinals_kept(self):
        snap = request()
        view = reconcile_observations(snap, [observe(snap, 0), observe(snap, 1)])
        self.assertEqual(len(view.pages[0].observations), 2)

    def test_duplicate_ordinal_refused_even_empty_or_different_payload(self):
        snap = request()
        for other in (observe(snap), observe(snap, pages=(2,)), observe(snap, pages=())):
            with self.subTest(pages=other.decoded.entries), self.assertRaisesRegex(DecodeError, 'ordinal'):
                reconcile_observations(snap, [observe(snap), other])

    def test_generation_mismatch_and_stale_refused(self):
        expected = request(7)
        for generation in (0, 6, 8, 0xffffffffffffffff):
            with self.subTest(generation=generation), self.assertRaisesRegex(DecodeError, 'generation'):
                reconcile_observations(expected, [observe(request(generation))])

    def test_same_generation_different_snapshot_refused(self):
        expected = request(7)
        for records in (((1, 0, 129), (2, 0, 0)), ((2, 0, 0), (1, 0, 128)),
                        ((1, 0, 128),), ((1, 0, 128), (3, 0, 0))):
            with self.subTest(records=records), self.assertRaisesRegex(DecodeError, 'different request'):
                reconcile_observations(expected, [observe(request(7, records))])

    def test_other_ce_in_same_request_bound_independently(self):
        snap = request(records=((1, 0, 7), (1, 255, 9)))
        view = reconcile_observations(snap, [observe(snap), observe(snap, 1, ce=255)])
        self.assertEqual([p.presence for p in view.pages], ['listed', 'listed'])
        self.assertEqual([p.observations[0].raw_status for p in view.pages], [7, 9])

    def test_unknown_page_and_foreign_ce_rejected(self):
        snap = request()
        for pages, ce in (((3,), 0), ((1,), 1), ((1, 3), 0)):
            with self.subTest(pages=pages, ce=ce), self.assertRaisesRegex(DecodeError, 'no exact'):
                observe(snap, pages=pages, ce=ce)

    def test_duplicate_page_and_request_key_delegated(self):
        with self.assertRaisesRegex(DecodeError, 'duplicate result'):
            observe(request(), pages=(1, 1))
        with self.assertRaisesRegex(DecodeError, 'duplicate request'):
            request(records=((1, 0, 1), (1, 0, 2)))

    def test_count_and_length_boundaries(self):
        snap = request(records=tuple((i, 0, i & 255) for i in range(127)))
        valid = Observation(snap, 0, scratch(tuple(range(127))), 0, 0)
        self.assertEqual(len(valid.decoded.entries), 127)
        for count in (128, 0xffffffff):
            with self.subTest(count=count), self.assertRaises(DecodeError):
                Observation(snap, 0, scratch(count=count), 0, 0)
        for length in (0, 511, 513):
            with self.subTest(length=length), self.assertRaises(DecodeError):
                Observation(snap, 0, bytes(length), 0, 0)

    def test_synthetic_id_boundaries_and_bool_rejection(self):
        for value in (0, 0xffffffffffffffff):
            snap = request(value)
            self.assertEqual(observe(snap, value).ordinal, value)
        for value in (-1, 1 << 64, True, False, 0.0, '0', None):
            with self.subTest(value=value):
                with self.assertRaises(DecodeError): request(value)
                with self.assertRaises(DecodeError): observe(request(), value)

    def test_context_values_validate_all_bytes_without_interpretation(self):
        snap = request(records=((1, 255, 255),))
        for status in range(256):
            value = Observation(snap, status, scratch((1,)), 255, status)
            result = reconcile_observations(snap, [value]).pages[0].observations[0]
            self.assertEqual((result.ce_index, result.context_status, result.raw_status), (255, status, 255))
        for value in (-1, 256, True, False, None, 0.0):
            with self.subTest(value=value):
                with self.assertRaises(DecodeError): observe(request(), ce=value)
                with self.assertRaises(DecodeError): observe(request(), status=value)

    def test_strict_normalized_types(self):
        for value in (None, True, (), {}, iter(())):
            with self.subTest(value=value), self.assertRaises(DecodeError):
                Observation(value, 0, scratch(), 0, 0)
        for value in (None, True, {}, set(), iter(())):
            with self.subTest(value=value), self.assertRaises(DecodeError):
                reconcile_observations(request(), value)
        for value in (None, True, (), {}, object()):
            with self.subTest(value=value), self.assertRaises(DecodeError):
                reconcile_observations(request(), [value])

    def test_reused_scratch_and_input_mutations_do_not_replace_old_observation(self):
        snap = request()
        buf = scratch((1,))
        first = Observation(snap, 0, buf, 0, 1)
        buf[:] = scratch((2,))
        second = Observation(snap, 1, buf, 0, 4)
        originals = [first, second]
        view = reconcile_observations(snap, originals)
        buf[:] = b'\xff' * 512
        originals.clear()
        self.assertEqual([p.observations[0].ordinal for p in view.pages], [0, 1])
        self.assertEqual([p.observations[0].context_status for p in view.pages], [1, 4])
        self.assertEqual(first.payload[4:8], (1).to_bytes(4, 'little'))

    def test_copied_request_records_and_result_immutable(self):
        entry = RequestEntry(1, 0, 7)
        entries = [entry]
        snap = RequestSnapshot(0, entries)
        observation = observe(snap)
        view = reconcile_observations(snap, [observation])
        entries.clear()
        object.__setattr__(entry, 'raw_status', 255)
        self.assertEqual(view.pages[0].request_entry.raw_status, 7)
        for value, name, replacement in ((view, 'pages', ()), (view.pages[0], 'observations', ()),
                                          (view.observations[0], 'payload', bytes(512)),
                                          (view.snapshot, 'generation', 1)):
            with self.subTest(name=name), self.assertRaises(FrozenInstanceError):
                setattr(value, name, replacement)

    def test_tampered_observation_fields_revalidated(self):
        for name, replacement in (('ordinal', True), ('ce_index', True), ('context_status', True),
                                  ('payload', bytes(511)), ('snapshot', None)):
            value = observe(request())
            object.__setattr__(value, name, replacement)
            with self.subTest(name=name), self.assertRaises(DecodeError):
                reconcile_observations(request(), [value])

    def test_tampered_cached_decoded_result_not_trusted(self):
        value = observe(request())
        object.__setattr__(value, 'decoded', object())
        view = reconcile_observations(request(), [value])
        self.assertEqual(view.pages[0].observations[0].raw_status, 128)

    def test_expected_snapshot_revalidated_even_without_observations(self):
        for name, value in (('generation', True), ('entries', None),
                            ('entries', (RequestEntry(1, 0, 0), RequestEntry(1, 0, 1)))):
            snap = request()
            object.__setattr__(snap, name, value)
            with self.subTest(name=name), self.assertRaises(DecodeError):
                reconcile_observations(snap, [])

    def test_view_does_not_retain_caller_observation_objects(self):
        snap = request()
        original = observe(snap)
        view = reconcile_observations(snap, [original])
        object.__setattr__(original, 'context_status', 0)
        object.__setattr__(original, 'payload', bytes(512))
        object.__setattr__(snap, 'generation', 99)
        self.assertEqual(view.snapshot.generation, 0)
        self.assertEqual(view.observations[0].context_status, 255)
        self.assertEqual(view.pages[0].observations[0].context_status, 255)

    def test_independent_linear_oracle(self):
        rng = random.Random(0x4f425350504e)
        for case in range(500):
            # Generate unique normalized keys without using production output.
            tuples = [(p, ce, rng.randrange(256)) for p in range(rng.randrange(15))
                      for ce in range(3) if rng.randrange(2)]
            rng.shuffle(tuples)
            generation = rng.randrange(1 << 64)
            snap = request(generation, tuples)
            original_lists = []
            for ordinal in range(rng.randrange(8)):
                ce = rng.randrange(3)
                pages = [p for p, c, s in tuples if c == ce and rng.randrange(2)]
                rng.shuffle(pages)
                original_lists.append((ordinal, ce, rng.randrange(256), pages))
            values = [observe(snap, ordinal, pages, ce, status)
                      for ordinal, ce, status, pages in original_lists]
            view = reconcile_observations(snap, values)
            # Oracle scans original tuples/lists, no dictionaries/set matching,
            # decoder fields, generated payload offsets or result reuse.
            with self.subTest(case=case):
                self.assertEqual(len(view.pages), len(tuples))
                for record, actual in zip(tuples, view.pages):
                    page, ce, raw = record
                    expected = []
                    for ordinal, listed_ce, status, pages in original_lists:
                        for index, listed in enumerate(pages):
                            if listed == page and listed_ce == ce:
                                expected.append((ordinal, index, ce, status, raw))
                    self.assertEqual([(o.ordinal, o.list_index, o.ce_index, o.context_status, o.raw_status)
                                      for o in actual.observations], expected)
                    self.assertEqual(actual.presence, 'listed' if expected else 'unknown')

    def test_independent_adversarial_binding_oracle(self):
        rng = random.Random(0x42494e44494e47)
        for case in range(240):
            generation = rng.randrange(1 << 63)
            original = [(p, 0, rng.randrange(256)) for p in range(5)]
            expected = request(generation, original)
            metadata = []
            values = []
            for ordinal in range(3):
                current_generation = generation
                current_request = list(original)
                current_ordinal = ordinal
                mode = case % 6 if ordinal == 2 else 0
                if mode == 1: current_generation -= 1
                if mode == 2: current_generation += 1
                if mode == 3:
                    p, c, s = current_request[0]
                    current_request[0] = (p, c, s ^ 128)
                if mode == 4: current_request.reverse()
                if mode == 5: current_ordinal = 0
                snap = request(current_generation, current_request)
                values.append(observe(snap, current_ordinal, pages=(0, 2)))
                metadata.append((current_generation, current_request, current_ordinal))
            # Independent comparison is over original tuples, not production
            # snapshot equality, decoder objects or exception text.
            valid = True
            previous_ordinals = []
            for g, records, ordinal in metadata:
                if g != generation or records != original or ordinal in previous_ordinals:
                    valid = False
                previous_ordinals.append(ordinal)
            with self.subTest(case=case):
                if valid:
                    view = reconcile_observations(expected, values)
                    self.assertEqual(len(view.pages[0].observations), 3)
                else:
                    with self.assertRaises(DecodeError):
                        reconcile_observations(expected, values)


if __name__ == '__main__':
    unittest.main()
