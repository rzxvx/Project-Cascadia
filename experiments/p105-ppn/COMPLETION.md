# Unbound Linux-owned CDMA completion lifecycle

Unbound default-off candidate, 2026-10-12. This group executes no real MMIO, DMA,
interrupt routing, devices, firmware, NAND operation or filesystem mutation.
`ppn_cdma_completion.c/.h` are GPL-2.0-only clean-room Linux C. They neither copy
Apple channel-state structures nor qualify a writable MTD backend.

## Concrete piece closed

The existing original-backed `ppn_cdma_build_plan()` produces a non-AES complete
batch but had no completion/lifetime model. This candidate stores that actual
planner's result and binds one immutable Linux-owned controller/epoch/transfer/
opaque-lease/channel identity. It returns a narrowly specified ACK/disable plan,
then requires exact action and externally qualified quiescence receipts before
`ppn_cdma_completion_wait()` can return zero. There is no cancel, reset, reuse,
IRQ-route or device binding API. The default zero object confirms nothing.

The existing stage engine is unchanged. A strict private test uses its actual
`ppn_stage_program()` and its unique data/OOB lease callbacks, with this candidate
inside Fake `dma_start`/`dma_wait`. A missing or failed completion never becomes
success through cleanup. The Fake cancellation callback expressly lacks a
qualified physical cancel operation, so it returns `-EOPNOTSUPP`: the real engine
blocks, retains the affected leases/buffers and reports `unquiesced_dma`. This
shows the intended adapter interface without pretending hardware is connected.

## Normalized input, not a hardware ABI

The Linux allocator/controller owner must provide nonzero, never-reused
controller, epoch, transfer and lease-cookie values and retain the exclusive
controller/channel reservation continuously through the final fence. An object
address or channel alone is insufficient as a transfer identity. The tuple does
not come from an Apple IRQ packet: the actual controller IRQ cannot prove which
Linux generation owns it. Snapshot generation/old-interrupt fencing remains a
real backend obligation.

Before begin, external evidence must establish exclusive ownership, IOP/previous
DMA handoff quiescence and prior-IRQ drain. Merely setting these booleans in C does
not establish those hardware facts. None has been measured for this target.
Inputs/outputs/state must not alias, inputs are immutable during calls, and calls
are serialized by the controller owner. In-memory structs are not wire/flash
formats. State storage must be freshly all-zero and is initialized once; reused
ACTIVE, WAIT_ACTION, WAIT_FENCE, FINISHED or HOLD storage is rejected unchanged.

An API request key identifying another object is rejected unchanged with
`-EINVAL`. A stale/foreign actual observation or lost current exclusivity instead
latches `HOLD/-ESTALE`, retaining the current identity and lease; it never ACKs,
retires or releases a newly owned transfer. `HOLD` cannot be reset through this
API. Unknown/failed quiescence is a buffer-lifetime fault, not a bad-block proof.

## Original-backed decision leaf

Pinned inputs and extraction identities are in the [native CDMA binding](../../docs/research/p105-ppn-cdma-binding.md). `emulate-completion-boundary.py` executes the same
original 12H321/13G36 helper instructions under strict process-local Unicorn
MMIO/RAM, extended with signed-length and pending/remaining boundaries. It passed
274 original runs, including 112 IRQ observations. No physical addresses access
host memory; they only select Unicorn Fake RAM.

| Observation | Candidate behavior |
| --- | --- |
| CONTROL `0x40000` set, including with DONE | DMA error `-EIO`, HOLD, no ACK/disable/finish |
| `0x100000` spurious, or no `0x80000` | `-EAGAIN`, ACTIVE, no positive action; bounded caller wait remains separate |
| DONE, raw pending nonzero | exact W1C ACK plan only; after applied ACK, HOLD/`-EOPNOTSUPP`, no guessed refill |
| DONE, pending zero, remaining nonzero | exact W1C ACK plan only; after applied ACK, HOLD/`-EPROTO`, no finish |
| DONE, pending zero, remaining exactly zero | exact W1C `0x80000` at `channel << 12`, then disable `BIT(channel)` at global offset8; WAIT_FENCE, still no finish |

Only channels5..8 from the existing planner are accepted. Unknown non-event
CONTROL bits are not ORed into an ACK or given invented failure meanings. Error
precedence is checked before spurious/completion. The original partial/refill
branch depends on additional `CONTROL&0x30000` and SG state; that full ABI is not
imported here. `pending` is kept a raw normalized progress word; this candidate
only qualifies its zero case, not a descriptor-count or byte-unit interpretation.

The original handler uses a signed `remaining <= 0` shortcut (12H321
`0xd2cc/0xd2ce`). Both versions' actual virtual executions therefore call finish
for synthetic remaining `0x80000000` or `0xffffffff` with pending zero. The
planner restricts actual length to `<=0x7fffffff`; these values are inconsistent
progress. Linux deliberately requires **remaining exactly zero**, plans no
disable, and never confirms finish for those inputs. The independent test oracle
records this stricter behavior explicitly, rather than changing original traces.
This is not evidence that real hardware produces these counter values.

Spurious/no-DONE original paths reach Apple's REQUIRE endpoint. This candidate
conservatively models them as no completion; it does not infer a failed DMA from
no event. A caller must bound its polling/wait and retain buffers on timeout.

## Action receipts and quiescence gate

The action output is not authority for an arbitrary register write. A future
backend must revalidate its exact exclusive lease BEFORE each operation, ACK
first, stop on ACK failure, then disable only its own channel. It must preserve
ownership through barriers and drain. This candidate never executes that plan.

Receipts must match the entire planned identity, observation number, offsets,
W1C word, disable bit and attempted-action mask. ACK failure is accepted only
when disable was not attempted; negative transport errors remain failures,
positive errors are rejected as protocol faults. Missing, foreign, altered or
replayed receipts never finish and cannot trigger a blind retry.

A fully successful action receipt advances only to WAIT_FENCE. A separate exact
identity/observation receipt must establish all four external facts:

1. Reservation is still exclusive for this generation.
2. No DMA can access the loaned buffers/descriptors in future.
3. This generation's interrupt/callback work is drained, including late events.
4. Relevant memory/device barriers and completion visibility have finished.

Incomplete flags return `-EAGAIN` unchanged, preserving ownership for a bounded
external fence wait. Unknown flags, stale identities or a failed fence latch
HOLD. These are normalized backend witnesses, not facts deduced from a callback,
ACK, disable write or status0. Physical witnesses remain absent. In particular,
no fence result asserts NAND program, address-bound final status, bad-block
retirement, flush, SLC ownership or power-loss durability.

FINISHED allows `dma_wait` zero; it does not call `dma_release`. An engine adapter
still follows its existing release/lease ownership contract. Cancellation of a
stalled/error/refill transfer requires another actually qualified bounded
quiesce implementation, which this module intentionally cannot fabricate.

## Checks and reproduction

Frozen dependencies copied byte-for-byte from the reviewed existing experiment
are pinned in `dependency-hashes.json`; the test refuses dependency drift.
`test-completion.py` constructs a separate oracle from actual original traces
and checks ACK/callback/disable evidence independently before compiling C.

- 1,204 actual C assertions, including 112 original IRQ comparisons.
- 44 real frozen-engine/data/OOB-lease integration cases across both buses.
- Native and existing Ubuntu22 image: AddressSanitizer/UBSan PASS.
- Actual Linux6.12 ARM kernel compilation: ELF32 ARM little-endian EABI5, no
  compiler warnings; kernel headers are readonly and unchanged before/after.
- Linux checkpatch: C/H both0 errors/0 warnings.
- ARM object unresolved symbols only planner/memory/unwind helpers, no I/O API.

The C regressions cover exact W1C/offset/bit fields, missing/foreign/duplicate
receipts, stale controller/epoch/transfer/lease/channel, lost exclusivity,
no/spurious/error events, unsupported refill, positive callback errors,
ACK failure without disable, disable failures, all missing fence-flag subsets,
unsigned maximal progress, cross-data/OOB confirmation, premature fence and
reinitialization. The engine tests verify that no partial action/fence result
causes lease release or `transport_complete`; NAND durability stays unknown even
for its all-success process-local fixture.

Run `python3 tests/test-experimental-ppn-completion.py` from the repository
root. It uses the unchanged, hash-checked public engine/planner/fixture sources,
normalized original trace metadata and temporary process-local outputs, with
ASan/UBSan enabled. Original firmware emulation remains a private optional
comparison requiring the explicitly pinned official images. Real Linux6.12
ARM-object compilation passed without warnings. No MTD/backend is enabled.

Fence flags are positive confirmations covering the continuously held interval;
an incomplete receipt permits further bounded waiting, never lease release.
A known loss of exclusivity must be supplied as an error (for example ESTALE),
not omitted and later relabeled confirmed. Actual backend proof is absent here.

The [IOP handoff audit](../../docs/research/p105-iop-handoff.md) independently
shows why a sleep response, software-running flag or IOP clock release cannot
replace this module's separate DMA/IRQ lifetime fence. CDMA and IOP gates are
distinct even in the actual native controller graph.
