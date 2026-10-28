# Isolated Linux-owned BBT transaction candidate

This independently designed C state machine prepares **in-memory plans only**.
It has no callbacks, devices, files, MMIO, DMA, firmware, MTD hooks or program/
erase capability. It is not an Apple BBT/VFL parser or an on-flash C ABI. The
current read-only driver and frozen protocol engine remain unchanged.

The external source-of-truth durability and ownership gate is **unfulfilled**
on our target. Setting a fixture boolean true only tests conditional software
behavior. Neither an engine returning0 nor a CRC/readback supplies that proof.

## Concrete stages

1. Default zero config fails ownership validation. A separately proven Linux
   pool has explicit block count, media/owner IDs, current lease epoch and two
   journal-slot physical/alias IDs. Slot aliases or duplicate physical IDs fail.
2. Empty host-quarantine state cannot start an operation until Genesis is itself
   journaled. Empty bitmap does not assert that physical/factory bad blocks are
   absent; a future MTD frontend must union this host bitmap with measured
   physical status. There is no implicit target formatting.
3. Preparation requires an exact owned alternate-slot erased/quiesced/durable
   receipt bound to the *next* generation. It creates an immutable snapshot with
   monotonically increasing64-bit generation, bad bitmap and optional operation
   intent. No committed state changes yet.
4. Exact BODY_DURABLE receipt moves to WAIT_MANIFEST. A separate manifest
   fingerprint can be requested only now. Unknown durability/quiescence,
   foreign/stale identity, wrong stage/CRC or modified prepared bytes poison
   writable use. The source has no mechanism for manufacturing a hardware receipt.
5. Exact MANIFEST_DURABLE receipt publishes the prepared snapshot. A committed
   intent enters ACTIVE; Genesis/completion checkpoint enters READY. The caller
   must retain the previous committed slot until this boundary.
6. Successful operation completion needs independent flash durability and DMA
   quiescence before an intent-clear plan. Confirmed retirement is a distinct
   policy witness; generic failure cannot mark a physical block bad. Unknown
   outcome/failed quiescence retains the persistent intent and read-only state.

The future body and manifest each need their own entire16KiB program page;
partial-page and same-page commit-bit assumptions are excluded. This candidate
contains only normalized records and CRC domain/version definitions. A real
explicit-endian codec, spare metadata, erase/reuse policy and hardware backend
have not been implemented. CRC32C detects accidental corruption and binds
plans; it is not cryptographic authenticity or chip durability.

## Conservative recovery

Both slot reads must have completed. Blank slot/manifest absence additionally
requires an explicit erased-page witness; absence cannot stand for a short or
failed read. A proven blank marker allows ignoring an uncommitted torn body.
Any complete body+manifest with CRC failure fails read-only, **even when an
older valid NONE copy exists**: otherwise a damaged committed intent could be
forgotten. Foreign owner/media/slot/alias, malformed geometry/intent, set bitmap
bits beyond declared geometry and equal-generation ambiguity also fail closed.

A selected open intent returns a readable snapshot while retaining READONLY.
It is not relabelled physically bad, avoiding implicit loss of acknowledged
data during a later UBI scan. GenerationUINT64_MAX cannot wrap or permit new
plans. Past record epochs can be recovered after reboot; new runtime receipts
must match the new lease epoch. This is an epoch namespace, not an Apple token.

The block numbers are coordinates within a separately verified Linux-owned pool.
Mapping them to physical bank/block/alias domains and proving exclusion of
boot/iOS/VFL/FTL/BBT regions is **outside** this module. `ownership_proven` must
cover those conditions and truthful whole-pool provenance; our current hardware
does not meet that gate. No writable attach or mount is enabled here.

## Validation

- 4075 actual C API calls pass natively, in the bounded Ubuntu22 image and with
 ASan/UBSan. Counts are API calls, not4075 hardware experiments.
- A separate Python CRC table/explicit byte stream and persisted-copy oracle
 track runtime publication separately from saved body/manifest states.
- Every transaction boundary is interrupted in the model, including erased slot,
 body-only, body acknowledgment and persisted manifest with lost acknowledgment.
- Wrong confirmations/epoch/owner/media/slot/alias/CRC, unknown durability and
 quiescence failure retain the old published state and poison writing.
- A damaged *committed intent* never falls back to an older READY copy.
- 128 deterministic program/erase/retirement cycles verify the bitmap independently;
 block65535 exercises the explicit65536-block workspace bound. That maximum is
 a memory limit, not a device-capacity assumption.
- ARM EABI5 cross-compilation succeeds with-Wall-Wextra-Werror; the ARM object
 was not run on the target. Linux6.12 checkpatch reports0 errors/warnings.

Workspace is about16KiB and must be heap allocated, not placed on kernel stack.
Only owned workspace, immutable normalized snapshots and receipts may be passed.
No C struct is to be written raw to flash. Source is exported as an isolated default-off experiment after root review; no actual hardware test or iPad mutation occurred.

## Root-review corrections in v2

Initializers are fresh-zeroed-storage only. Every nonzero byte in an existing
workspace produces-EALREADY without changing any byte: Genesis, Ready,
WaitBody, WaitManifest, Active and Readonly all preserve retained state. Config,
recovery copies and output workspace overlap checks run before any memset;
manifest-output overlap is rejected as well. Config/records/receipts are immutable
inputs, and all calls require caller serialization. No reset API is provided.
Manifest retrieval now latches READONLY if its prepared snapshot was corrupted,
instead of returning a recoverable-looking error while keeping the waiting state.

The exact hash-pinned frozen private-v1 C source fails the new reinitialization
regression. The v2 candidate passes those controls; its4075 calls also include
output alias and prepared-corruption retrieval regressions. Native/image/ASan+
UBSan and ARM compile-only checks passed. This is not a physical power-cut test.

## Genesis correction in v3

create_empty now takes TWO explicit SLOT_ERASED receipts, each bound to the
configured media/owner/current lease epoch/physical slot/alias domain and
Genesis generation0. Both must prove durability and quiescence and contain no
body/manifest fingerprint. Null, one-slot-only, stale, foreign, wrong-stage,
unknown-quiescence or wrong-generation receipts fail closed. Receipt/config/
workspace overlap is rejected; all inputs remain immutable and serialized.

The new-workspace/existing-intent fixture derives blank-slot evidence from the
independent persisted-copy model. A fresh RAM object plus one erased alternate
slot cannot bootstrap over the other slot's committed intent; recovery returns
its pending block in READONLY. Exact frozen-v2 source with its REAL two-argument
ABI reproduces the earlier incorrect READY/gen1 RAM state while the persisted
intent isgen2; no adapted prototype or candidate inversion is used for control.
The v3 real three-argument API rejects that bootstrap. Current hardware still
supplies neither whole-pool ownership nor any required physical receipt.

Portable reproduction: `python3 tests/test-experimental-ppn-bbt.py`. This is a
Linux-owned design, not a recovered Apple journal. No backend can presently
supply the required physical ownership/durability/blank-slot witnesses.
