# Checked CDMA cancellation transport — unbound experiment

This clean-room Linux-C foundation implements the native cancellation transport
order with Linux error propagation and generation-bound lease checks. It has no
hardware/regmap callbacks, IRQ handler, DMA allocator, MTD binding, firmware calls
or NAND operations. It is not wired into the existing engine or kernel.

Independent private original instruction runs established 35 exact
source facts bind the 12H321 and 13G36 IOP leaves; 576 actual instruction runs use
only strict Unicorn Fake RAM/MMIO. After all 288 successful nonzero-state
cancellations, a separately injected DONE observation calls the old callback in
the actual original IRQ handler. This is a software control, not evidence that
physical hardware delivers such an event. All fixtures made zero hardware calls.

Source identities:

- 12H321 kernelcache SHA256
  `db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c`,
  embedded IOP at file offset `0xa69000`, length `0x24000`, SHA256
  `6dfa5dcc6f8cb38be41e48c0edc543b2014a026f7909c35c9ef050ffd23c22d4`.
- 13G36 kernelcache SHA256
  `8eae16f060a71d4f3790385951588f8198d0f21cdb74146f9750941a3d764125`,
  embedded IOP at file offset `0xc0f000`, length `0x23000`, SHA256
  `2d59d78ab2c60a924f0be39be631c4d4ed08b403119b8641e1bf115a0bba860d`.

No Apple firmware bytes or source tables occur in this group. The numeric
original trace fixture is pinned independently, as are the existing completion
and planner headers; those modules remain unchanged.

## Exact transport and conservative additions

For owned channels 5..8, register offsets are relative to the separately
validated CDMA controller base. The original nonzero-state leaf reads global
STATUS offset `0x10`, enables the owned channel through SET `0` if absent, reads
CONTROL at `channel << 12`, and when `CONTROL & 0x30000 == 0x10000` writes exactly
`2` and polls until that condition differs. It then clears only the owned channel
through CLR offset `8`. Original cancellation does not ACK DONE or drain IRQs.
The cleared original context field is an AES configuration pointer, not the
callback/token. This helper does not copy either private context layout.

This candidate adds positive-error rejection, ownership checks before and after
every I/O, explicit fresh bounded monotonic-microsecond cleanup timing, a hard
poll budget, clock rollback/freeze rejection, and STATUS readbacks after SET/CLR.
These readbacks are conservative checks against the same raw STATUS bit used by
the original activation path. Immediate hardware acknowledgement is not proven:
if a future backend observes lag or ambiguity, this candidate fails closed.
Raw CONTROL states other than masked `0x10000` receive no electrical meaning.
Neither `0x20000`, `0x30000`, DONE nor a cleared enable bit proves no future access.

`begin` requires an externally qualified own submitted lease, continuous
exclusive reservation, and initial inherited-controller handoff fence. The exact
controller, epoch, transfer, opaque lease cookie and channel are immutable.
Adoption is allowed only once into fresh entirely zero storage; wrong API keys
are rejected without I/O or state mutation. Old generations must not be reused
or wrapped while old events can exist. Config, callbacks, state and arguments
must remain immutable/nonaliasing, and calls must be serialized externally.
The helper cannot create these physical ownership prerequisites.

`run` takes a new cleanup timeout of 1..2,000,000 microseconds, independent of an
already expired operation deadline. Every synchronous I/O callback receives the
remaining budget. Callback errors, lost epoch/lease, clock failure, timeout,
missing readback or post-I/O deadline overrun latch `HOLD`, retain the exact lease
and all buffers, and disallow automatic retries. The wrapper cannot preempt a
callback that violates its documented bound. IRQ-handler serialization and
reservation cannot be inferred from a register sample.

A successful transport sequence enters `WAIT_FENCE` and returns `-EINPROGRESS`.
`wait` never returns zero. There is deliberately no buffer-release/fence-accept
API and no adapter presenting this result as the engine's `dma_cancel == 0`.
A future backend must separately prove controller no-future-memory-access,
IRQ/callback drain, ordering barriers, exclusive current ownership and IOP
handoff. NAND status, persistence and durability remain completely outside this
helper. Late DONE observations cannot complete, ACK or release anything here.

## Offline verification

`python3 tests/test-experimental-ppn-cancel.py` compiles and runs
the actual C with strict regular-memory callbacks. It passes **9,118 assertions**
and **288 independent original successful-path write-trace comparisons**. The
same ASan/UBSan run passes in the existing bounded Ubuntu22 image. Cases include
all callback-position negative/positive errors, epoch changes and late returns;
permanent busy, frozen/rollback/wrapped clocks, ignored SET/CLR, foreign keys,
64-bit identity truncation, channel limits, missing callbacks, reused state,
late DONE and no retry/release. Foreign global channel bits remain unchanged.
The callbacks use no system/device/IRQ/IO APIs.

A local object verifier executes the captured actual Linux6.12 ARMv7 kernel compile flags
with **`-Werror`**, using readonly kernel headers and readonly source. It passes as
ELF32 ARM little-endian EABI5; header hashes before/after match. The object has
only the toolchain unwind/stack-protector unresolved symbols, no device calls.
`checkpatch.pl --no-tree --file` reports zero errors and zero warnings for C/H.

No controller/DMA/IRQ timing or physical ownership was validated on an iPad.
The host production services and attached iPad remain untouched.

Portable tests default to ASan/UBSan and temporary output; --no-sanitize is an
explicit optional override. Existing public completion/planner headers are
hash-checked dependencies; no duplicate modules or normal build hooks are added.
