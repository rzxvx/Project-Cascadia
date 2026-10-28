# Linux DMA provider boundary for a future P105 backend

Bounded private source audit and actual GPL C fixture, no new adapter/receipt
module. No A5/Tegra equivalence, live IRQ/MMIO/DMA, device/boot/NAND operation,
normal driver patch or new hardware probe. Current unbound P105 C helpers remain
unchanged and retain DMA memory until their externally qualified fence.

## Exact original GPL functions

Pinned Linux6.12 inputs:

* include/linux/dmaengine.h SHA256 a1041fddb0ec26c888320ddca5f55da17e58b4c559e534f5b24e59fafe273c1e
* drivers/dma/tegra186-gpc-dma.c SHA256 b4f39876aec6d57324d3ff3f79976daae6b9bc19b2872007ef6621bcc85bc65e

run.py extracts six real functions without changing their bodies: generic
terminate_async, synchronize and terminate_sync; Tegra terminate_all,
chan_synchronize and its chip-specific pause_noerr. Fixture structs and
remaining API endpoints are explicit field-compatible Fakes, not real kernel
struct ABI, hardware behavior or a fake replacement implementation of those
functions. Source attribution for Intel/NVIDIA and GPL is preserved.

Native and existing Ubuntu22 bounded image ASan+UBSan/-Werror PASS22 controls.
There is no reason to claim an ARM hardware validation or export a new runtime
helper from this test-only group.

## Closed Linux software obligations

The actual Tegra terminate_all holds the channel lock, calls its hardware-
specific terminate method, and immediately returns any nonzero failure BEFORE
retiring/freeing the active descriptor or clearing the running descriptor.
After successful hardware termination it retires the descriptor, invokes channel
disable, clears running state, removes SID, detaches descriptor lists, releases
the lock and frees descriptors. Its synchronize method waits for the Linux IRQ
and vchan callbacks separately. The exact control trace and retained-state/error
return are verified for active/idle channels, success, negative timeout/EIO,
invalid positive callback error and active/inactive simulated callback lifetimes.

A deliberately INVALID provider which returns terminate0 without halting its
modeled hardware and has no synchronize callback makes the actual generic
terminate_sync return0. This is not a Linux/Tegra defect and not evidence of
real running hardware. It demonstrates that the wrapper trusts the provider
contract and cannot turn an inadequate A5 provider into a correct fence.
Missing terminate method returns EINVAL. The actual chip-specific pause_noerr
also returns0 after a failed pause callback; its source documents recovery from
Tegra flush states by subsequent disable. That chip-specific rationale cannot
be copied as an A5 timeout-to-success fallback.

Primary Linux API documentation:
https://docs.kernel.org/driver-api/dmaengine/client.html
https://docs.kernel.org/driver-api/dmaengine/provider.html
Pinned implementation:
https://github.com/torvalds/linux/blob/v6.12/include/linux/dmaengine.h
https://github.com/torvalds/linux/blob/v6.12/drivers/dma/tegra186-gpc-dma.c

## Concrete P105 backend decision

Do NOT install dmaengine_terminate_sync as a facade over our current cancel
CONTROL2/poll/CLR helper and call the result a no-future-access fence. A correct
future P105 device_terminate_all must establish actual hardware stop/drain first,
return negative error and retain all coherent allocations/mappings on unknown
stop, then fence Linux IRQ and deferred callbacks before freeing/reassigning.
Generation/immutable opaque transfer identity and descriptor lifetime must
remain continuously owned under a channel/controller lock. Device detach,
probe unwind and managed allocation teardown must not free memory retained by
an unquiesced operation merely because a helper returned its timeout.

No concrete normal-driver hardware stop predicate can currently be imported:
Tegra uses its own BUSY and TX/RX status bits, request selector and pause/disable
protocol; R-Car uses its own CHCR DE readback and device-specific flush contract.
Those bits/semantics are not Apple CDMA. R-Car's void halt/readback-error logging
is likewise not an acceptable template for our error-propagating cleanup.

Our exact source-backed Apple candidate proves per-channel SET, CONTROL2,
control30000 polling, CLR and enabled-bit readback. Existing original-code
controls separately prove late software IRQ callbacks remain possible after
cancel; raw AIC events have no embedded Linux transfer generation. None proves
that zero/other CONTROL state or enablebit clear prevents queued/outstanding bus
accesses. IOP SLEP ACK precedes its CPU sleep; AP stop ignores provider failures.
AIC synchronize_irq drains current Linux handler/thread activity but has no
current driver ACTIVE/PENDING chip-state hook and cannot drain independent IOP
work. An old IOP target4→APtarget1 rewrite is not exclusive ownership.

Thus the strongest IMPLEMENTABLE C integration edge now is the separately
frozen raw-PPN guard against a bound/in-probe kernel H2FMI owner, plus explicit
MTD/sysfs-only firstboot. A live CDMA/IOP backend remains gated by a physically
qualified stop/drain predicate and old-owner handoff. No third receipt placeholder
or guessed field is added to pretend that gate is closed.

## Reproduce

python3 run.py --linux /home/marco/projects/ipad-cascadia/upstream/build/linux

Limited image: /code this group readonly, /linux pinned Linux tree readonly,
independent private writable /out, no device mounts, existing constrained wrapper
with CASCADIA_BUILD_OFFLINE=1, --read-only and user1000:1000. Run python3 run.py
--linux /linux --output-dir /out. Compiler/executable operate only on private
regular-memory fixtures; Fakes don't call any kernel/API or real device.
