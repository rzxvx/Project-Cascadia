# P105 native CDMA resource and completion binding (offline evidence)

Verified offline source audit, 2026-10-12. No firmware bytes are included here.
No device, Linux DMA engine, interrupt, NAND operation or live firmware was run.
The existing default-off C planner is unchanged and has no hardware backend.

## Pinned inputs and reproducible result

| Input | SHA-256 | Relevant extraction |
| --- | --- | --- |
| 12H321 kernelcache Mach-O | db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c | IOP at file offset `0xa69000`, size `0x24000` |
| 12H321 embedded IOP | 6dfa5dcc6f8cb38be41e48c0edc543b2014a026f7909c35c9ef050ffd23c22d4 | original native ARM/Thumb instructions |
| 13G36 kernelcache Mach-O | 8eae16f060a71d4f3790385951588f8198d0f21cdb74146f9750941a3d764125 | IOP at file offset `0xc0f000`, size `0x23000` |
| 13G36 embedded IOP | 2d59d78ab2c60a924f0be39be631c4d4ed08b403119b8641e1bf115a0bba860d | original native ARM/Thumb instructions |
| Official 13G36 decrypted DeviceTree | e8277fd827ba250de3f46567cb1c910d4e1c80633d666510a7780e00f4ad4b59 | 79,748 bytes |

The official 13G36 IPSW provenance remains in
`references/p105-ios935-official/plaintext-provenance.json`. IOP extraction was
independently located through the Mach-O/prelink firmware kext, not by reusing
12H321 offsets. The full IOP image and the relevant function prefixes differ.

The private `verify-cdma-binding.py` checks 50 exact instruction, literal and peripheral-table
facts, plus the DeviceTree resource properties. the private `emulate-cdma-binding.py` executes
226 original helper paths: per version 8 IRQ-install, 8 IRQ-enable, 32 completion
and 65 physical-translation cases. Both PASS in the existing limited, offline
image. Critical-section APIs, callbacks and refill/require endpoints are explicit
Fakes; their real bodies, scheduling, cache operations and hardware were not run.
Single-step execution intercepts these boundaries between original instructions;
it does not rely on changing Thumb IT control flow inside a Unicorn code hook.
Strict Fake MMIO only accepts the specified AIC writes and CDMA accesses. An
independent arithmetic/state oracle checks the trace, callback and result.

## Physical resources, not a driver binding

13G36 ADT `arm-io/ranges` is `[0, 0x30000000, 0x10000000]`.

| ADT child resource | Physical resource | Evidence/scope |
| --- | --- | --- |
| cdma `0x07000000/0x2d000` | `0x37000000/0x2d000` | native channel control at base + `(channel << 12)` independently matches original IOP |
| cdma `0x07800000/0x9000` | `0x37800000/0x9000` | resource identity only; this group does not qualify an AES path |
| flash-controller0 six 4-KiB windows | `0x31200000`, `0x31240000`, `0x31280000`, `0x31300000`, `0x31340000`, `0x31380000` | controller registers/ECC resources; no new MMIO operation |
| AIC `0x0f200000/0x10000` | `0x3f200000/0x10000` | shared interrupt controller |
| IOP `0x0f300000/0x1000` | `0x3f300000/0x1000` | mailbox resource and IRQ2; no mailbox ownership/handoff proof |

The CDMA ADT IRQ array is exactly 51..92. The original channel-start code installs
IRQ `channel + 50`; channels 5..8 therefore use IRQ55..58. The old original PPN
program caller, already separately emulated, selects channels `5 + 2*bus` and
`6 + 2*bus`, with data/OOB FIFOs `0x31200014/18` or `0x31300014/18`.
The 13G36 native CDMA FIFO-selection table independently contains the same four
FIFO addresses and selectors 0..3. The other words are kept opaque, not assigned
new meanings. Full 13G36 PPN program-caller qualification remains outside this
scope. The flash-controller ADT has IRQ33/34, not the DMA completion IRQs.

**The ADT flash node has no `dma-channels` or `dma-parent` property.** The channel
relationship comes from original native instructions and callers, not from an
invented ADT DMA specifier. IRQ-parent phandles for CDMA/FMI/IOP bind to the same
AIC. Board-default disk properties do not identify the connected unit's NAND,
capacity, physical address layout, bad blocks or SLC geometry.

## AP versus IOP IRQ ownership

Both original IOPs write destination value 4 to AIC
`0x3f203000 + irq*4`, then clear the global mask through
`0x3f204180 + (irq/32)*4`. The global mask bank is **the same** register bank used
by Cascadia's Linux AIC driver, not an IOP-private mask bank. Linux's
`aic1_irq_unmask()` writes destination `BIT(0)` instead. The original IOP EVENT
literal is `0x3f202004`; Cascadia AP dispatch uses the per-CPU EVENT windows
`0x5004 + (cpu << 7)`. An AP adapter must use the Linux IRQ domain and the existing
AP dispatcher, not transplant the IOP's event-dispatch/target writes.

This is source identity, not evidence that the IOP currently owns or even runs
these channels after the Cascadia iBEC boot. A safe adapter still needs a boot
handoff proving IOP/channel/DMA quiescence, exclusive controller/IRQ ownership,
and that no late IOP callback or old descriptor can access reassigned memory.
Reassigning target=1 is not that proof. Linux resource reservation by itself
cannot fence an independently active IOP.

Linux AIC comparison source:
`patches/files/drivers/irqchip/irq-apple-aic1.c`, SHA-256
`5001ada7042ca42d884f71f191ffe2ee0ed9a8bd6811b4881933c2793d64edf6`;
constants at lines 55/64/68/71 and unmask implementation at lines 500..512.
Its existing comments distinguish the AP per-CPU event window from the classic
alias. This audit does not reinterpret the alias as a proven live CPU topology.

## Real DMA completion and version-specific state

Both original IRQ handlers first reject CONTROL bit `0x40000`, then `0x100000`,
and require `0x80000` before W1C-acknowledging `0x80000`. The source then checks
pending descriptors and hardware remaining length. Only complete progress
invokes the configured callback after disabling this channel through global
CDMA base+8. A pending complete-batch refill reaches a separately Faked refill
endpoint. A remaining-byte mismatch reaches original REQUIRE after the ACK;
it never becomes a successful callback in our trace oracle. Error/spurious/
no-event/combined-error paths also never publish the success callback.

| Native detail | 12H321 | 13G36 |
| --- | --- | --- |
| IRQ installer / enable | `0x231c` / `0x2220` | `0x236c` / `0x2270` |
| CDMA handler | `0xd280` | `0xd1d0` |
| CDMA start | `0xcc08` | `0xcba0` |
| channel state base / stride | `0x18a30` / `0x58` | `0x21414` / `0x58` |
| pending / callback / token offsets | `0x28 / 0x40 / 0x44` | `0x24 / 0x3c / 0x40` |
| finished-state word offset | `4` | `0` |

These changed layouts disprove treating 13G36 as the old C-struct ABI. The error
byte read at channel+`0x18` remains a raw diagnostic; no NAND/ECC/status semantics
are inferred from its synthetic value. DMA completion establishes memory/FIFO
progress, **not NAND program success, correct address-bound final status,
retirement, quiescence or power-loss durability**.

## Physical translation boundary

Original 12H321 `0xb5b8` and 13G36 `0xb574` call the respective map lookups
`0xb568` and `0xb524`, using a table at `0x13168`. Each 16-byte map contains cached
virtual base, uncached virtual base, physical base and length, with zero length
terminating the table. They accept either non-`0xffffffff` virtual alias, select
its relative offset and add that offset to the physical base. A missing mapping
returns `0xffffffff`. Synthetic boundary cases cover both aliases, absent aliases,
multiple mappings, before/after bounds, and physical spans reaching the largest
32-bit address. A genuinely mapped last address can also be `0xffffffff`;
therefore the raw return value alone cannot distinguish all mapping failures.

This is **not** proof of identity-mapped AP memory, coherent Linux DMA, IOMMU
absence or usable host physical addresses. A future adapter must allocate/map
through the Linux DMA API, validate its 32-bit mask, check mapping errors and the
entire owned span without wrap, and explicitly qualify peripheral-visible DMA
addresses. The old planner's extra two descriptors and cache-flush end must
remain inside that owned allocation. Firmware virtual pointers, the table itself,
AP kernel pointers and CPU physical addresses cannot be substituted as DMA
addresses. Source cache/IOP address translation doesn't validate Linux ownership.

## Remaining implementation gates

1. Explicit, bounded boot IOP/DMA/controller handoff and late-completion fencing.
2. Linux-owned resources and IRQ-domain mapping, serialized channel ownership,
   actual DMA allocation/mask/cache coherency, timeout and quiesce contract.
3. Complete native-controller clock/reset/CDMA state initialization; 13G36 caller
   and descriptor ABI qualification if using that profile.
4. Correct combined data/OOB completion and controller error propagation;
   address-bound final NAND status/error ownership remain independent.
5. Measured media/mode/alias ownership and durable/quiesced receipts before any
   writable MTD, SLC/UBI layout, retirement or Linux journal can be enabled.

This group closes concrete address/IRQ/helper questions. It intentionally creates
no hardware adapter, probes, allowlist changes, MTD type changes or write enable.
