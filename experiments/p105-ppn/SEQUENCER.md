# Isolated per-controller Sequencer plan foundation

Offline, unbound clean-room C candidate. No MMIO/upload callbacks, kernel hook,
microcode table, device, DMA operation or writable MTD binding. Returning0 from
`ppn_seq_prepare()` means only a transformed44-word **plan** was prepared.
It does not authenticate the caller's program, initialize hardware or guarantee
that a program/erase operation is durable.

The hash-pinned official12H321 IOP original function88a4 is executed in Unicorn
with synthetic44-word program and two synthetic timing weights. All writes target
private RAM or44 virtual FMI register slots; no host physical address is mapped.
There are98 actual original executions over both buses, one-time flag on/off,
zero/small/large clocks and overflowing original products. The instruction-only
56-facts verifier in the frozen sibling CDMA group binds44 count, the global flag,
program/table literals and upload loop directly to the pinned original bytes.

Original semantics: first initialization mutates only exact words0x0e000000 or
0x0e000001. It multiplies controller field+8 by the corresponding timing weight
with32-bit wrap, then computes `(product * 0x431bde83) >> 50`. All44 words are
uploaded to FMI offsets0xc1000+4*i. Other0x0e-prefixed words are preserved.
The global byte at18318 suppresses transformation for all subsequent controllers.
A two-controller test proves that a later differing clock reuses the first plan.
This is a software observation, not proof that Apple's actual controller clocks
differ or that the original firmware's hardware timing is wrong.

The C candidate creates a fresh plan from caller-owned immutable original words
for each controller. It rejects clock0 and original32-bit multiplication overflow
instead of reproducing unproven wrapped timing. Descriptor input/output may not
alias. Exactly44 readable original words must be supplied. Unknown word meanings,
the clock field's physical units, required timing weights, instruction semantics
and controller/firmware applicability remain separate validation obligations.
No guessed opcode validation or Apple44-word sequence is included.

1089 real C cases pass natively, in the existing Ubuntu22 image and ASan/UBSan.
25 compatible fresh original executions match exactly; all incompatible zero
clock/overflow cases are rejected.1024 deterministic bounded-product fixtures
use an independent integer-division oracle, plus exact million/product boundaries
includingUINT32_MAX. Invalid requests leave output untouched. GCC-Werror and
Linux6.12 checkpatch have0 errors/warnings.

A future backend must authenticate an exact target program source and its
timings, verify the measured controller clock and clock-change exclusion, own
the controller exclusively, hold quiescent FIFO/DMA/CE state, serialize the plan
with correct endian and MMIO barriers and validate upload/readback semantics.
None of those responsibilities is satisfied by this pure preparer. The current
read-only NAND path and original frozen protocol engine remain unchanged.

Input source hashes:

- Kernelcache:db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c
- Embedded IOP:6dfa5dcc6f8cb38be41e48c0edc543b2014a026f7909c35c9ef050ffd23c22d4

No iPad access, actual NAND write/erase, kernel attachment or hardware test occurred.

Updated unit evidence: AP80c519c0 reads hclk and divides by1000 before
Command+C0xb8; IOP26ec transfers this field to FMI+8, which88a4 consumes.
Thus the backend must supply kHz, not Linux clk_get_rate() Hz. Fixture clocks
are synthetic arithmetic stress values, not measured P105 controller timings.

Portable reproduction: `python3 tests/test-experimental-ppn-sequencer.py`. The semantic fixture contains synthetic addresses/program words and no firmware instructions. ARMv7 object compilation against actual patched Linux6.12 headers passed with zero warnings.
