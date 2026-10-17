# Sequencer clock factor correction — separate evidence, frozen C unchanged

The field consumed by original IOP88a4 is **the platform `hclk` quotient1000**,
not the raw platform clock value. It is consistently named kHz by AP timing
diagnostics. A future Linux caller must not pass `clk_get_rate()` Hz directly as
`ppn_seq_request.clock_factor`.

Exact pinned12H321 chain:

1. AP80c519c0 reads its platform object, invokes vtable34c with string`hclk`
   at519ca/519cc. Return is64-bit inr0/r1.
2. AP519ce/519d2 sets divisor1000 as64-bit r2/r3. Actual helper57a90/57aa8
   divides unsigned64-bit input; 519d8 stores only quotient low32 to command+b8.
3. IOP26ec constructor reads command+b8 at2742 and writes FMI+8 at2746.
4. IOP88a4 reads FMI+8 at88e0, multiplies the timing weight modulo32 at88e8,
   then applies the fixed multiply/high shift at88ea/88ee.
5. AP `_fmiInitTimings` independently divides hclk by1000 at52c02/52c08 and
   diagnoses `clockSpeedKHz > 0` at52cd8.

19 exact static instruction/string facts pass.11 actual AP prefix/divider
executions use only a fake platform-vtable return and synthetic RAM, including
24,000,000→24,000 and100,000,000→100,000, zero/<1000 boundaries and64→32-bit
quotient truncation. Both Thumb64-bit and ARM32-bit division helpers execute as
their original code; there are no platform hardware calls or host MMIO maps.

The frozen Sequencer tests' large numeric clock factors were deliberate synthetic
overflow/edge inputs, **not target timing recommendations**. C arithmetic and
original comparisons remain valid. Documentation previously leaving clock units
open is now narrowed by this additional source evidence; firmware microcode
semantics, timing-weight provenance and actual controller clock remain unmeasured.

Linux caller contract before binding: obtain an authenticated target HCLK source
and confirmed platform units; validate Hz≥1000 and quotient≤UINT32_MAX; convert
once to the exact kHz factor; exclude concurrent clock changes. Validate timing
weights and reject32-bit factor×weight overflow. Recompute from original immutable
program for each controller; do not reuse a global already-patched table or infer
that a copied bootloader value remains valid. No microcode upload is enabled here.

Kernelcache SHA256db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c.
No Apple firmware bytes are included in this note and no target operation occurred.
