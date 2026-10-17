# Synthetic PPN error-list decoder

`tools/nand/ppn_error_list.py` is an independent, pure Python analysis module.
It accepts synthetic fixtures only and has no file/device access, firmware
calls, address dereferences, MMIO, opcode dispatch or driver integration.
It adds no writable NAND capability. Its input is a normalized model, not
a claim to parse the complete Apple outer-command/PPN-descriptor ABI.

The layout evidence is recorded in
[the pinned 12H321 field inventory](p105-ppn-error-list-evidence.md).
It observes a 512-byte scratch, a little-endian u32 count, and page-address
words followed by unused bytes. The derived parser count bound is 127;
this remains **our buffer-capacity bound, not a firmware/hardware limit**.

## API and explicit context

`RequestEntry(page, ce_index, raw_status)` is a frozen normalized record.
The fields must be exact Python integers in u32/u8/u8 range. Boolean,
floating-point, negative and overflowing values are rejected.

`decode_error_list(payload, request, *, ce_index, context_status)` accepts
an exact 512-byte `bytes`/`bytearray`, plus a list/tuple of those records.
Both context values must be explicit u8 integers. CE indices are opaque;
no relation to a physical CE pin, bus, MTD die, or our unit's capacity is
assumed. `context_status` is supplied by the caller, not a field decoded
from the list. Unknown status values, including 255, remain raw.

The decoder copies its inputs, rejects counts above 127 before indexing,
and requires every result page to match exactly one `(page, ce_index)`
request key. Duplicate result pages and duplicate request keys are rejected,
including ambiguous request records with different statuses. The same page
on another CE may exist in a request, but cannot satisfy the selected CE.
Duplicate request keys are rejected even for an empty result list.

Zero count is valid. Page values 0 and `0xffffffff` remain raw u32 values;
neither is assigned a sentinel or geometry meaning. Trailing unused padding
is opaque and ignored. The normalized request may have more than 127 records;
127 bounds this result buffer only, not an inferred firmware queue size.

The immutable `DecodedErrorList` preserves list order, the separate caller
context, and each matched record's original status. It never ORs context
flags into request flags, silently ignores missing pages, classifies retry
or retirement, or returns partial matches on malformed input. `DecodeError`
is a `ValueError` subclass. Inputs must not be concurrently mutated while
the function copies them; the result retains no caller-owned records.

## Offline validation

Run `python3 -B tests/test-ppn-error-list.py -v`. There are 22 unittest cases,
including exact length boundaries, count 0/1/127/128/UINT32_MAX, byte-order
reversal, foreign CE, missing/duplicate keys, all 256 raw status values,
integer/type rejection, caller-input mutation and immutable result records.
Six hundred deterministic pseudorandom fixtures are compared against an
independently constructed linear-search oracle, which uses original tuples
rather than decoder bytes, output, dictionaries or field constants.

The tests passed on the host and existing Ubuntu22 build image with no
network, device mounts or firmware input. These results establish parser
validation behavior only. DMA ownership/lifetime, stale events, full ABI,
queue-size initialization, acknowledgement order, ECC generation, programming
order, NAND durability and power-cut recovery remain unresolved. A raw
descriptor adapter, firmware reader or writable driver is separate work.
