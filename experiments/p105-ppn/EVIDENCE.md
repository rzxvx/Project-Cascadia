> Isolated source experiment. Nothing selects this code in the normal kernel. Hardware behavior has not been tested. Local staging paths below identify the original evidence, not build hooks.

# Private, unbound PPN engine candidate — review checkpoint

This is a clean-room, GPL-2.0-only **protocol orchestration foundation**. No Linux
MTD callback, device binding, MMIO accessor, executable writer/probe, firmware
upload, mutable command allowlist or NAND operation on a device is provided.
It is exported for source review under `experiments/` and has no runtime installation hook.

## What is implemented

- Explicit SLC row encoding and physical CE/CAU/block/page/field-width checks.
  The caller must supply independently established geometry and an exclusive
  owned block interval. The example geometry in tests is SYNTHETIC.
- One full 16,384-byte data + 64-byte metadata page, four 4,096+16 chunks.
  The original descriptor's repeated `0x0b000000` words are preserved as four
  opaque chunk-envelope words; no guessed NOP/timing meaning is assigned.
- The genuine single-page sequencer envelope and separate data/meta DMA
  channel requests (5/6 or 7/8), with bounded FIFO/event polls and W1C handling.
- One row-aligned SLC-block erase through `0x6a/0xd7`, status phases and `0x77`
  transaction end. The physical CE is selected explicitly; foreign CE results
  are rejected. Generic raw-NAND `0x80/0x10` or `0x60/0xd0` are absent.
- Callback errors remain errors, including final DMA timeouts. Positive
  callback return values fail with EPROTO rather than looking like success.
- DMA ownership uses unique transfer leases. A partially accepted failed start
  must return its own lease; failed start with NULL guarantees no DMA accepted.
  Completion does not release ownership until the corresponding lease is
  released. No channel-wide cancel interface exists. Cancel touches only live
  owned transfers; release requires proven quiescence.
- Failed cancellation retains both caller buffers conservatively and the
  relevant lease; result.unquiesced_dma prohibits freeing those buffers.
- Any attempted-operation failure blocks the engine. There is no unpoison/
  retry/reset function or destructive recovery callback.
- Error-list snapshots are copied after DMA quiescence, each exactly 512 bytes,
  count ≤127 and exact single-row/request/physical-CE context. Unknown, duplicate
  or excessive entries fail closed. Raw statuses are preserved. EF3380 is
  intentionally absent: its acknowledgement/state effect is unresolved.

Every result retains `durability_unknown=true`. A return value of zero means
successful **transport orchestration only**, and must not be wired to successful
MTD write, fsync, UBI commit or durable block-device completion.

The caller must zero-initialize the engine once, hold the single controller lock
through every operation, and reserve the DMA transfers/physical interface under
its own lifetime discipline. There is no installed backend. A real port still
needs pinned sequencer setup, CDMA descriptor/coherency semantics and certified
bus timing. Callback assumptions are explicit API contracts, not evidence that
our iPad or an arbitrary kernel driver meets them.

## Verified original evidence and independent comparison

The private `research/ppn-protocol` package inspects the hash-pinned official
12H321 kernelcache/embedded IOP. Static checks passed **117+45+131** instruction,
literal, jump-table and namespace bindings. Original execution is confined to
Unicorn's mapped synthetic memory; instruction/MMIO hooks reject unknown paths.
No host device register can be addressed.

- 433 AP constructor-prefix/decoder pairs against independently computed
  coordinates; callback is never executed. Original ARM division helpers run.
- 144 complete original IOP6744 erase-orchestration cases with strict helper
  callbacks, queue boundaries, per-entry statuses and completion cleanup.
- 74 original IOP93b8 write runs, including **actual drain a108** and **actual
  list reconciler6488**, virtual MMIO, fake DMA/EE/EF APIs and independent trace
  checks. This revealed final DMAwait failure can retain return1 in the original
  response model, while failed bulk-prep enters REQUIRE without cancellation.
  These are conditional software traces, not claims of observed hardware loss.
- The C candidate is compiled in plain and ASan/UBSan profiles. Each passed
  **1,910** bounds/adversarial/fault cases, including failure at every ordinary
  callback on the successful program/erase path and failed partial starts/cancel.
- Both C bus profiles exactly match all **25** successful original single-page
  register/DMA actions, using the independently produced original Unicorn JSON.
  The private Python runner does not construct an expected trace from the C.
  Native host and the existing bounded Ubuntu22 image both passed.

Neither original trace nor fake DMA establishes actual cancellation safety,
SLC-vs-MLC block exclusivity, erase mode transition, factory/retired BBT policy,
EF3380 semantics, power-cut durability, or independent boot without intact iOS.
These remain separate integration gates. Linux6.12 UBI rejects ordinary MLC;
use true PPN SLC blocks with proven geometry rather than a false emulation flag.
For the synthetic 16KiB/128-page SLC layout, PEB=2MiB and conservative no-subpage
UBI VID/data offsets are16KiB/32KiB; LEB=2,064,384 bytes. This is not a measured
geometry of our unit, whose storage capacity is still unknown.

## Reproduction

Use only the existing private original trace generated by the reviewed emulator:

```
python3 test_engine_original_oracle.py --original-trace ../ppn-write-emulator/write-trace.json
```

The runner compiles reviewed local C sources into a private temporary directory,
runs process-local fakes and compares the existing original trace. It opens no
hardware or kernel NBD/MTD interface. Run under the existing resource-limited
image with network disabled and both inputs read-only for the second profile.
SHA256 identities are stored in CHECKPOINT.json, alongside firmware identities;
no Apple firmware bytes/disassembly belong in a public export.

## v2 review corrections

`pause_poll` requires observable monotonic clock progress after every successful
1µs delay. A frozen `now_us` combined with a successful no-op delay now fails
immediately with EIO rather than looping forever. Callback timing is checked
both before and after ordinary register operations, after DMA starts/waits and
feature copies, after the final successful transaction end and before reporting
transport completion. A late successful callback therefore cannot become success.
A software caller cannot preempt a callback violating the explicit bounded
synchronous API contract; an eventual backend must provide genuinely bounded
I/O/phase waits and microsecond-granularity monotonic time.

DMA cancellation gets a distinct 2,000,000µs cleanup budget even if the operation
expired. The budget is passed to the own-lease callback and elapsed time is checked
after it. Cancel failure, clock regression or a late successful cancel retain the
lease and both caller buffers conservatively. Completed DMA leases are released
without cancelling them after a late callback; ownership never moves to a foreign
channel transfer. These facts concern the staged C API, not hardware cancellation.

Six added actual-C fixtures cover frozen-clock FIFO/event loops, late DMAwait,
late cancel, late NANDcommand and late final NANDend. Plain+ASan/UBSan tests pass
native and Ubuntu22; all25 actions of both original successful bus traces remain
unchanged. C/header pass pinned Linux6.12 checkpatch with0errors/0warnings.
Formatting used privately extracted Ubuntu `astyle3.1-3build1`, SHA256 of archive
74b43cc09a610b467e0e62bad7f0dfc761af42878461ddb94aa99dfb71812da7,
matching the existing host apt metadata. Nothing was system-installed, activated
or overwritten. The reviewed v1 candidate remains untouched.

After a transport error the physical controller/NAND queue state can be unknown,
even if DMA quiescence was proven. Blocking prevents retry; no reset, ACK3380,
reinitialization, wear/BBT update or persistent-completion claim is supplied.
