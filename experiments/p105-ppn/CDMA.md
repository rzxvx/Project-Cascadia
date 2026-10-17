# Isolated CDMA initialization and non-AES plan evidence

Exported clean-room candidate, 2026-10-12. No hardware backend, kernel hook,
register callback, IRQ installation, DMA submission or MTD write capability.
`ppn_cdma_build_plan()` returning zero means **a plan was prepared**; it says
nothing about transport completion or durable NAND storage.

Source is the verified official 12H321 Apple IOP image in kernelcache SHA256
`db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c`.
`inventory_12h321.inventory()` hash-gates every private original-code run.
The embedded image SHA256 is
`6dfa5dcc6f8cb38be41e48c0edc543b2014a026f7909c35c9ef050ffd23c22d4`.
Instruction inventory and raw traces remain private; no Apple bytecode is
included in C/H or intended for a public repository.

## Actual original source comparison

The IOP `cc08` start routine and `cf30` chain builder run as their original
Thumb instructions in Unicorn. `cee8` activation executes too. Fake physical
addresses translate only the mapped private chain RAM. Allocation, IRQ setup,
cache cleaning and critical-section calls are strict, recorded Fake APIs.
All MMIO reads/writes target only Unicorn's own virtual register memory;
there are no host-device descriptors, actual physical mappings or NAND calls.

119 original executions pass: channels 5/6 and 7/8 with their actual respective
FMI FIFO addresses, both directions, first-use initialization and reuse,
1/2/4/127/128 SG entries and invalid length/size/width cases. The independent
oracle checks descriptor addresses, flags, payload lengths, six register writes
and pending bytes. SG128 produces a 127-entry first batch with 64 bytes still
pending; the private C plan intentionally rejects incomplete/multiple batches.

56 exact instruction/literal facts verify chain allocation, limits,
interrupt registration, descriptor fields and start/cancellation paths.
The original first-use path allocates 4096 bytes aligned to32 and creates
128 next-address slots with32-byte stride. Channel state has0x58-byte stride.
Command flags are3 plus a4-bit opaque selector, with0x100 on the batch's final
descriptor and a following zero flag. Non-AES channel start is0x1c0009 plus
the separate channel-start selector in bits24..27.

The descriptor selector (direction-dependent channel-state offsets18/1c)
and start selector (state14) are deliberately distinct C input fields. The
tests exercise unequal selectors instead of assuming they always match.
Actual FIFO CONFIG selector fields are0/1 on FMI0 data/meta and2/3 on FMI1.

The original cache-clean request can cover4128 bytes for a still-pending full
127-descriptor batch despite a4096-byte allocation request. This is an exact
callback-length observation, not proof of a real out-of-bounds memory access:
the allocator's usable size and hardware cache-maintenance behavior remain
unproven. The clean-room builder requires owned storage for `count+2` entries,
validates32-bit address spans, emits only a complete batch and includes every
linked next address in that owned allocation. Its cache range is `count+1`
entries. It never emits a chain for invalid inputs.

638 actual C cases pass natively, under ASan/UBSan and in the existing Ubuntu22
image.96 complete original executions match descriptor and register plans
exactly;512 deterministic adversarial SG cases use a separately constructed
oracle. Invalid requests retain the entire output unchanged. GCC `-Wall
-Wextra -Werror` and Linux6.12 checkpatch have no errors or warnings.

## Backend contract before binding

The prepared numeric FIFO addresses come from this pinned IOP target's
peripheral mapping. A future Linux DMA backend must verify target mapping and
DMA mask32, obtain mappings with the Linux DMA API, and reject truncation or
unsupported address translation. A CPU virtual pointer is never a DMA address.
The plan's native words must be explicitly serialized little-endian. SG source
and output may not alias, and caller owns the output plus every data mapping.

Reserve the controller and exact channels before preparing/submitting. Allocate
`(count+2)*32` owned, DMA-addressable,32-byte-aligned chain bytes; clean/sync
the complete owned range and apply required barriers before start. Validate
mapped SG lengths against the exact payload size. Keep lease generation and
buffer/chain ownership until IRQ completion or proven quiescence. Hardware
IRQ W1C/refill/cancel semantics must still be implemented and checked.

Original `d3fc` cancellation polls channel-state bits16..17; state0x10000
triggers a cancel command2. At0x2710 original timer units, continued activity
calls REQUIRE. This is not an ownership-safe Linux cancellation implementation.
Our existing protocol engine's separate bounded cleanup deadline and retained
leases on failure remain mandatory; the plan builder does not replace them.

AES/IV descriptors, refill, scatterlist merging/coherency on actual ARM,
sequencer timing, IRQ ownership and controller reset are outside this candidate.
No successful offline trace establishes flash program durability, SLC/MLC alias
exclusivity, bad-block journal persistence or a safe UBI/UBIFS write callback.
Those conditions still block writable MTD binding. No device was tested.

Portable reproduction: `python3 tests/test-experimental-ppn-cdma.py`. The semantic fixture contains synthetic addresses/program words and no firmware instructions. ARMv7 object compilation against actual patched Linux6.12 headers passed with zero warnings.

The independently qualified [native binding](../../docs/research/p105-ppn-cdma-binding.md)
now establishes resource windows, channels5..8/IRQ55..58 and both version-specific
IOP completion layouts. It does not supply AP DMA mapping, IOP handoff or hardware
backend permission. Do not reuse the12H internal state ABI on13G.
