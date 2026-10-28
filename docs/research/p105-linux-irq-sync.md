# Actual Linux6.12 IRQ synchronization boundary (offline fixture)

This fixture compiles **the actual GPL C bodies**, extracted verbatim from the
pinned current Linux6.12.0 source, for `__irq_get_irqchip_state`,
`__synchronize_hardirq` and `__synchronize_irq`. It supplies process-local Fake
locks, progress counters, state-query callbacks and thread wait predicates.
It never requests/disables/synchronizes a real IRQ, signals a process, accesses
a device or creates a hardware adapter. No normal kernel/AIC source is changed.

Primary source: [Linuxv6.12 kernel/irq/manage.c](https://github.com/torvalds/linux/blob/v6.12/kernel/irq/manage.c).
The exact inspected local source is authoritative for this group:

| Actual input | SHA256 |
| --- | --- |
| `build/linux/kernel/irq/manage.c` | `764415dc7e1732033e3b6dce8227190287546055a75c4fd5122293e0c710e96a` |
| Current Cascadia `irq-apple-aic1.c` | `5001ada7042ca42d884f71f191ffe2ee0ed9a8bd6811b4881933c2793d64edf6` |

The test pins both files, checks kernel VERSION6/PATCHLEVEL12/SUBLEVEL0, and
records individual extracted-body hashes. Current AIC1 uses a linear IRQ domain
and its actual irq_chip initializer has no `irq_get_irqchip_state` callback.
It is not replaced by a synthetic capable chip in the ordinary missing-hook
fixture. Hierarchy/capable-chip fixtures test generic optional kernel behavior,
not capabilities of this board's AIC1 implementation.

## Source-backed behavior

The actual synchronization body first waits for software IRQ-in-progress, then
checks that predicate again under the descriptor lock. When `sync_chip` is true,
it optionally queries `IRQCHIP_STATE_ACTIVE`. It explicitly ignores the query's
return code; progress can be updated only through the state output. Without a
state callback, the actual query function returns `-EINVAL` and leaves that
output unchanged. The current AIC has no such state callback or hierarchy parent.
After hardirq synchronization, the actual full function waits for the thread-
active counter to reach zero. These are Linux handler-lifetime predicates.

The meaningful compiled controls confirm:

- With software idle, missing hook and a separate synthetic physical-pending
  observation, synchronization returns without inspecting/clearing that pending
  observation. Direct failed queries leave the caller's state output unchanged.
- A state callback returning `-EIO` without changing its output is also ignored
  by synchronization; it does not become a confirmed hardware-idle sample.
- A supported successful callback polls ACTIVE until false when `sync_chip=true`;
  `sync_chip=false` intentionally performs no chip query.
- Software hardirq progress and the under-lock race recheck cause actual waiting.
  Thread counters0..7 are actually awaited by the extracted full function.
- Optional hierarchy can find a parent chip callback; absent hierarchy cannot.
- A permanently active software handler never produces a timeout return. The
  Fake observer stops it after101 relax calls via process-local longjmp; this is
  an explicitly bounded test observation, not an error manufactured by Linux.

The physical-pending variable is an independent synthetic counterexample input,
not a measured AIC register or proof a real pending interrupt survives a given
mask write. Fake scheduling/counters establish algorithm scope, not concurrency,
ordering, controller active-state semantics or real interrupt delivery.

## Consequence for a future NAND backend

A qualified Linux-owned AP IRQ can use generic APIs to drain its running AP
handlers/threads. The existing PPN contracts' AP handler-drain scope remains
valid; this test does not weaken it or claim the existing API is broken.
That receipt cannot be expanded to physical AIC-pending clearance, native IOP
callbacks, DMA no-future-memory-access or NAND durability. Disabling the owned
IRQ and lifetime serialization remain separate prerequisites; interrupt status
and native IOP ownership cannot be inferred from a void synchronization return.

Generic IRQ synchronization has no timeout argument and can wait indefinitely.
It cannot by itself satisfy the experiment's bounded synchronous cleanup-callback
contract. A future backend must qualify the bounds/liveness of its own handler,
thread/work callbacks and lock order; on missing physical proof or timeout it
must keep its DMA buffers/lease retained. No arbitrary grace period, blind retry,
IRQ retarget, invented ACTIVE hook or fake fence is introduced here.

## Offline result and reproduction

Native and existing bounded Ubuntu22 image both pass ASan/UBSan with `-Werror`:
**189 actual-C assertions without hierarchy,192 with hierarchy**. No skips and
no hardware calls. Generated C includes the original GPL copyright attribution;
source bodies are inserted unchanged, never rewritten into an invented model.

```
python3 test-linux-irq-sync.py --kernel /path/to/linux6.12 --aic /path/to/irq-apple-aic1.c --sanitize --output-dir /new/private/output
```

In the offline image, mount only readonly `/code`, readonly `/kernel`, readonly
AIC source `/input/aic.c`, and private writable `/out`. Supply those explicit
paths; no device mounts, privilege or networking. Both hierarchy runs have a
10second subprocess bound; Fake software/thread loops are separately capped.
The output hash inventory preserves fixture, actual bodies and test results.

The public `tests/test-linux-irq-sync.py` pins and compiles the exact generated GPL bodies/fake fixture. It checks each extracted function hash and the current tracked AIC source. When the full kernel tree is available it additionally verifies the original manage.c hash and exact body equality; a fresh checkout uses the same pinned self-contained GPL fixture without downloading Linux. No skips or device operations occur.
