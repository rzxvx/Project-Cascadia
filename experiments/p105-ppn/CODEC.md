# Isolated PPN Linux-owned journal page codec

This closes the **byte serialization** gap in the v3 offline BBT transaction model.
It does not implement a NAND writer, an Apple-compatible journal, controller I/O,
MTD registration, a filesystem, or evidence of physical durability. Source is
independent GPL-2.0-only Linux-owned code. All buffers are ordinary immutable
process/kernel RAM; the experiment remains unbound and default off.

## Version 1 byte schema

Two distinct full 16,384-byte pages are required: body and manifest. Every integer
is explicitly unsigned little endian; no native C struct is copied to a page.
The fixed 128-byte header has the following layout.

| Offset | Bytes | Meaning |
|---:|---:|---|
| 0 | 8 | Body `PPNLBOD1`; manifest `PPNLCMT1` |
| 8 | 2 | Schema version: 1 |
| 10 | 2 | Kind: body 1; manifest 2 |
| 12 | 4 | Header length: 128 |
| 16 | 4 | Full page length: 16384 |
| 20 | 4 | Payload length: body 8192; manifest 0 |
| 24 | 4 | Used length: body 8320; manifest 128 |
| 28 | 4 | Flags: zero, unknown values rejected |
| 32 | 8 | Media identity |
| 40 | 8 | Owner identity |
| 48 | 8 | Record epoch, nonzero |
| 56 | 8 | Generation, nonzero; no implicit wrap |
| 64 | 8 | Physical slot identity |
| 72 | 8 | Alias domain identity |
| 80 | 4 | Declared blocks: 1 through 65536 |
| 84 | 4 | Normalized intent: NONE 0; PROGRAM 1; ERASE 2 |
| 88 | 4 | Pending block; FFFFFFFF only for NONE, otherwise below blocks |
| 92 | 4 | v3 normalized body CRC32C |
| 96 | 4 | Body: zero; manifest: v3 normalized manifest CRC32C |
| 100 | 4 | Whole-page CRC32C, this field logically zero during calculation |
| 104 | 8 | Body domain 314254424E505050; manifest domain 3154494D4D4F4350 |
| 112 | 4 | Normalized bitmap length: 8192 on both pages |
| 116 | 12 | Reserved zero bytes |

The body bitmap occupies bytes 128 through 8319. All unused bytes/bits beyond the
configured block count must be zero. Bytes 8320 through 16383 of the body, and
128 through 16383 of the manifest, must be FF. These padding bytes are schema
constants; seeing FF does **not** prove a physical erased NAND page or slot.
The 65536-block ceiling is a bounded model workspace, not measured p105 geometry.

Normalized CRCs match the frozen v3 transaction functions exactly. The body hashes
its domain, six u64 identity/version fields, three u32 geometry/intent fields,
and canonical 8192-byte bitmap. The manifest hashes its domain, six u64 fields,
and linked body CRC. A pair additionally compares the full duplicated identity,
geometry, intent and body CRC bytes. Page CRC covers the complete fixed page,
including reserved bytes and padding. CRC32C is corruption detection, not
cryptographic authentication or proof of NAND commitment.

## API and trust boundary

Encoding requires the current config lease epoch and valid, immutable snapshot.
It returns byte **plans**, never permission to submit them. The transaction state
machine still requires its externally witnessed erase/body/manifest receipts.
Ownership flags are not set or promoted by this codec. Decoding accepts historical
nonzero record epochs so reboot recovery remains possible. Its config checks media,
owner, geometry, physical slot and alias identities, but does not establish that
those identities describe exclusively owned hardware.

Body decoding yields an explicitly uncommitted view. Pair decoding yields only
snapshot, body CRC and manifest CRC. Outputs contain **no** `complete_read`,
`body_present`, `manifest_present`, `empty_slot_erased`, `absent_manifest_erased`,
`durable` or `quiesced` flags. A future reader must independently supply full-page
read/ECC completion and erased-page/slot evidence to the v3 recovery caller.
Malformed or missing manifests are errors; the codec never turns them into blank
slots. The hardware source-of-truth and exclusivity gates remain unmet.

Calls are serialized by the caller. All arguments must describe completely readable
ordinary RAM; input objects are immutable and do not overlap each other or output.
Exact page lengths and disjoint ranges are checked before parsing. Byte pages may
be unaligned. Output remains byte-for-byte unchanged on every failure. Snapshot
and output structs belong in owned heap memory, not the ARM kernel stack. C uses
small scalar locals only. The schema does not define an OOB layout, page pairing,
physical slot placement, factory bad-block handling, or a public storage format.
Do not ship it as a stable on-media ABI before those requirements are resolved.

## Offline validation

`python3 tests/test-experimental-ppn-codec.py` compiles and invokes the real codec together with the
hash-pinned v3 CRC/recovery source. A separate Python table-based CRC and explicit
`struct.pack` byte oracle construct complete pages. Native ABI sizes are checked
against actual compiled C. The fixed 32768-byte golden pair has SHA256
`fc4ce149300de196c59c39f52a05bf1c385635bd1ab59437f6afed9a621ee533`.

Fixtures cover every one-byte mutation in both full pages (32768 corruption
cases), every header field with a recomputed page CRC, exact sizes/nulls/overflowed
size_t/aliases/unaligned buffers, separate valid but mismatched records, historical
epochs, generation UINT64_MAX, both slots, three intents, 1/7/8/9/65535/65536-block
boundaries, noncanonical bitmap tails and independent pseudo-random vectors.
An actual v3 recovery fixture rejects decoded bytes without external evidence,
then accepts the same bytes only after the test independently supplies read/blank
flags. All tests use process-local memory; there are no device accesses or signals.
Native and Ubuntu22 runs, ASan/UBSan, and ARM EABI5 compile are required. ARM object
compilation is not execution, an endianness hardware test, or a real kernel/NAND test.
