# Native IOP IRQ retirement boundary — original source evidence

This group executes original 12H321 and 13G36 IRQ install/mask/dispatch leaves in
strict Unicorn Fake RAM only. It creates no Linux IRQ adapter, route change,
callback, device operation or firmware probe. Both firmware hashes and embedded
IOP hashes are checked before execution (same pinned official inputs as the
frozen CDMA binding/cancel groups). No firmware bytes are exported here.

`emulate-irq-retirement.py` passes **14 exact instruction assertions, 64 cases
and 208 actual original leaf calls**, covering both versions and NAND channels
5..8 / IRQ55..58. The independent trace oracle requires exact owned AIC target,
mask-set and mask-clear addresses/bits, callback identity/token and suppression.

| Original detail | 12H321 | 13G36 |
| --- | --- | --- |
| Disable leaf | `0x21dc` | `0x222c` |
| Install leaf | `0x231c` | `0x236c` |
| Event dispatcher | `0x2360` | `0x23a8` |
| Post-callback re-enable leaf | `0x2238` | `0x2288` |
| IRQ table base / entry stride | `0x15190 / 12` | `0x1dc1c / 12` |
| Handler / token / software-masked offsets | `12 / 16 / 21` | `0 / 4 / 9` |

For these normal IRQs, disable sets the software-masked byte to 1 and writes only
`BIT(irq % 32)` to AIC MASK_SET `0x3f204100 + 4*(irq/32)`. It does not erase the
registered handler/token, read pending EVENT, inspect CDMA CONTROL, stop DMA or
wait for any callback. Install writes the handler/token under explicit Faked
critical-section calls and assigns destination 4 at `0x3f203000 + 4*irq` even
when handler is NULL. **This is source evidence, not a recommended unregister
or AP retarget operation.** The shared AIC and IOP ownership remain unqualified.

Dispatcher reads IOP EVENT `0x3f202004`. A normal raw EVENT `0x10000 | irq` selects
the current table handler/token and calls it when non-NULL. The software-masked
byte is not consulted before invocation; it is checked only in post-callback
re-enable, where nonzero suppresses MASK_CLR `0x3f204180 + 4*(irq/32)`.

The controls demonstrate these precise software properties:

- Enabled and software-masked registrations both invoke the retained handler
  when the Fake EVENT has already been supplied. Only enabled re-enables it.
- A NULL handler installed before table load suppresses the callback, including
  after native disable. It does not establish physical IRQ or DMA drain.
- Replacing registration before dispatch makes the same raw EVENT invoke the
  replacement handler/token; the event contains no modeled transfer generation.
- Explicit synthetic interventions after handler load or after token capture
  cannot revoke already captured old registers. The old callback still runs.
  These interventions are deliberate input changes in owned Fake RAM, not proof
  that the single IOP CPU's real critical-section scheduling permits this race.

The fixture **does not claim real hardware delivers an event after mask-set**.
It supplies a previously delivered event independently of the synthetic mask;
AIC pending/latching, in-flight dispatch, cross-CPU synchronization and memory
ordering are not emulated. Likewise, old/new callback registrations are test
inputs, not a reconstructed PPN generation ABI. Critical-section bodies, CPU
masking and original callbacks are Fakes, not qualified physical barriers.

## Harness qualification

Executed instruction ranges are allowlisted, each leaf has a 1,200 executed-
instruction and 600 single-step-call cap, and all AIC accesses are exact-address
checked. No CDMA/FMI/host MMIO windows are mapped. Writes are restricted to the
owned IRQ table entry and process-local stack. Critical sections and callbacks
are intercepted between execution calls; their real bodies never execute.

Thumb IT execution can defer Unicorn `emu_stop` through a Fake endpoint NOP.
The code observer records the full CPU boundary context without changing PC/IT
inside the hook, then restores it between execution calls before servicing the
Fake API. A following process-local BKPT bounds that endpoint; only that
`UC_ERR_EXCEPTION` with a captured endpoint context is accepted. Unexpected
native exceptions or instruction/address escapes remain fatal. Explicit
interventions change only owned synthetic table bytes, never native code/PC.

## Backend consequence

AIC mask-set or NULL registration is not a generation-bound drained-callback
receipt. Linux `synchronize_irq` can drain a Linux-owned AP IRQ path only after a
qualified handoff; it does not drain the independently routed native IOP
registration. IRQ target assignment alone also cannot stop old DMA descriptors.
A future backend must prove both physical no-future-memory-access and callback/
IRQ lifetime serialization before releasing buffers or reusing any transfer ID.
The frozen checked-cancel helper correctly remains WAIT_FENCE/-EINPROGRESS.
This group introduces no new C module and changes none of the frozen experiments.

Primary comparison (different A4 hardware, no A5 compatibility claim):
[OpeniBoot A4 CDMA](https://github.com/iDroid-Project/openiBoot/blob/866562fdb1cfd019bcd77885c80fbf0af65d5c15/plat-a4/cdma.c)
`dma_cancel` likewise retains handler and provides no explicit IRQ drain. Its
controller base and descriptor allocation differ; those details must not be
transplanted into the native A5 backend. No new hardware specification validating
A5 physical abort/drain semantics was found by the narrow current search.

The root independently repeated the frozen14 exact assertions,64 cases and208 original leaf calls. The resulting normalized oracle matches exactly; private log `logs/night/irq-retirement-root-repeat-v1.log`. This public document contains no firmware bytes.
