# THS IRQ-to-signature connection: executable boundary, not a software token fence

This narrow group chooses the explicitly permitted **qualified hardware-boundary**
outcome. It connects the actual existing THS regmap transport and AuthCPAID
signature parser in executable C fixtures. It does not install/request an IRQ,
create a live worker/timer, perform hardware access, change a driver, or invent
a successful FIFO drain. An asynchronous production bridge remains denied
until a real hardware response-retirement contract is established.

## Actual C composition and counterexample

`test-pipeline.py` SHA-pins and links the complete current production sources:

- `cascadia-ths-idbus.c`: `bd6da8ec07730a79c5d3bfc9b936085bd67016c02bfc70337a0b69978f5af61c`.
- `cascadia-authcp-aid.c`: `382cc9dd37bd18d37e9466830fe109c0b29bbe1bdab6885bf65acdff2584b529`.
- Original synthetic THS32/32 signer oracle:
  `78b0091f7f704676495e3121d378eba9d6abfe8234318f686af8aacb37e48e75`.

The two original positive sequences (v2 nine frames/v3 eight frames) compose
byte-exactly: actual Linux C produces the expected unverified signature.
The regmap/lease bodies are fake RAM boundaries and are not hardware proof.

The diagnostic substitutes the first valid `99`+31-byte signature chunk for
the next same-length reply. The TX requests have different BE16 offsets and
unique new software request IDs, but the RX `99` frame contains no echoed
offset, nonce or request ID. Both actual C components accept the substituted
frame and finish an **incorrect UNVERIFIED signature**. Final cryptographic
verification is intentionally absent and no Authenticated/role/current/VBUS
permission is granted. Invalidation after a pending command safely stops
continuation and clears signature outputs; it does not physically flush RX.

This proves software indistinguishability **if** hardware supplies a late or
duplicate old payload. It does not demonstrate that the real THS produces
such a payload, identify an Apple firmware bug, or show that a physical fence
is impossible. The device protocol may supply an implicit flush/ordering
contract; that contract must be proved rather than replaced with an IDcounter.
IRQ timestamps and worker cookies alone do not distinguish those payloads.

## Original native abort/reset helper evidence

`emulate-ths-abort13.py` executes original13G36 methods, hash-pinned to official
kernelcache `8eae16f060a71d4f3790385951588f8198d0f21cdb74146f9750941a3d764125`.
Constructed THS vptr `809f12cc` maps slots364/368/36c to the actual entries.
Seven positive/error controls execute I2C writes and delay boundaries in fake
RAM, with explicit instruction budgets:

| Entry | Exact source operations | What remains unproved |
| --- | --- | --- |
| 809eec60 (+364) | reg41=1; return write result | RX FIFO drained/cancel acknowledged |
| 809eec80 (+368) | reg05=4; delay-helper argument20 after success | physical completion, time unit here |
| 809eecac (+36c) | reg05=8; delay-helper argument500; reg05=0 | no later old reply can arrive |

These helpers contain no status/idle acknowledgment reads and do not clear
native pending-RX field+84 in the bounded control. The fuller native status
handler `809eeabc` sees status0d bit04, clears fields79/7a and dispatches a
separate event. It also handles write/error events, and `(status&38)==10`
selects RX-read/reversal. An outer state machine exists; this group does not
claim that the entire native firmware lacks a fence. The causal meaning of
bit04 as a request-specific flush/reset acknowledgment, status read-to-clear
semantics, FIFO depth, completion ordering and late-response exclusion have
not been independently established. A delay plus successful I2C write alone
is not sufficient. No reset/abort sequence is added to Linux runtime code.

## Required Linux asynchronous boundary

Normal Linux IRQ/work/timer lifetime rules are necessary but insufficient to
prove response attribution. A real next integration must establish BOTH:

1. Physical THS/IRQ fence: retire old controller status and pending FIFO data;
   verify completion for the same live accessory; prove no old wire response
   remains admissible before publishing/submitting the next request. If the
   reset/cancel helper's semantics are not proved, keep UNKNOWN and do not
   restart or chain another transfer.
2. Software lifetime: one externally owned IRQ/lease, serialized worker and
   current request; I2C only in sleepable thread/work context; immutable
   resource lifetimes; bounded per-request deadline; irrevocable invalidation;
   no automatic resend. Quiesce IRQ producers and stop new work before
   synchronous timer/work cancellation and context destruction. Never hold
   the worker mutex while waiting for a worker/IRQ that needs the same lock.

The inspected Linux6.12 `devm_request_threaded_irq` implementation releases
managed IRQs on detach, but it does not itself drain unrelated queued work or
cancel timers. `synchronize_irq`, work cancellation or owner generation only
fence CPU callbacks. They do not establish that external chip RX belongs to
a request. A late frame could remain in the device after every CPU callback
has drained. Returning a fabricated successful fence callback in tests would
hide this missing physical condition, so no such backend is supplied here.

Actual kernel sources inspected: `kernel/irq/devres.c`, `kernel/irq/manage.c`,
`kernel/workqueue.c`, and `Documentation/core-api/workqueue.rst` from the
existing pinned Linux6.12 source. No internet/device/system/network operation
is needed for these controls.

## Reproduce

```
python3 test-pipeline.py --source-root REPOSITORY
python3 emulate-ths-abort13.py KERNELCACHE --source-root REPOSITORY
python3 arm-compile-pipeline.py --source-root REPOSITORY --linux-source LINUX_SOURCE --output PRIVATE_OUTPUT
```

Five entire-C composition controls pass with strict warnings, ASan and UBSan
on host and existing Ubuntu22 build image. Seven original helper controls
pass. Both existing production C components compile against real Linux6.12
ARM headers and link relocatably with strict warnings. This is not a new
kernel/bundle build or an installed asynchronous implementation.

All exported fixtures are synthetic, GPL source/text only. Apple firmware,
physical-device records, generated object files and private build outputs are
excluded. The frozen manifest contains exact file/source hashes. Next device
work is a separately approved, jointly observed fence/IRQ/response-retirement
validation; no hardware access is performed or pre-authorized by this group.
