# P105 charging checkpoint: 17 October 2026

Linux charging remains unqualified. The completed USB500 observation showed
net battery discharge, and the additional firmware analysis has not established
the missing analog protection or a physical charge-cutoff contract. This update
records the completed observation, improved analysis coverage and one newly
tested measurement interface. A later correction below narrows the experimental
worker's temperature window; charging opt-ins remain disabled by default.

## Experimental temperature-window correction

The worker previously accepted temperatures through 45°C, exceeding the agreed
40°C experimental maximum. It now permits 10..40°C. A fresh out-of-range sample
selects zero input/current and requests charge inhibition even when a positive
plan was already applied. This is an experimental software bound, distinct from
the original P105 thermal table and from hardware thermal protection.

The actual-C small-current suite passes 30 tests under ASan/UBSan and Werror.
New tests cover initial admission and withdrawal of an active plan, including
the 10/40°C endpoints and rejection at 9.9,40.1, 45 and 45.1°C. Both old-worker
controls reproduce the 40.1°C defect. A scoped ARM frontend object build with
W=1 passes without warnings against the existing read-only prepared kernel.
No full kernel rebuild or device deployment was performed. This fix neither
explains the inactive USB500 observation nor establishes physical cutoff.

## Completed Linux observation

The separately checked RAM configuration ran the existing bounded recorder and
one collector snapshot. It was not a boot of the complete research branch.

| Observation | Result |
| --- | --- |
| Valid source and gauge samples | 40 over 598.965 seconds |
| USB protocol allowance | 500000 microamps in every sampled source lease |
| Reported stored charge | Decreased by 49 mAh |
| Reported battery current | -294 to -292 mA |
| Reported battery voltage | 3.837 to 3.852 V |
| Reported battery temperature | 26.7 to 28.5 degrees C |
| One PMU input-code snapshot | Register 0x3d = 0x02, the existing uncalibrated nominal 100 mA profile |

The PMU snapshot occurred between source samples, outside their respective
250 ms leases. It was neither an atomic joint snapshot nor a continuous record
of PMU configuration. The 500 mA allowance is a USB protocol grant, not measured
input current. The deliberately inactive current/combined path was not shown
to be defective; its source-budget handoff already exists. No positive charging
opt-in was activated. Repeating this baseline is not the next experiment.

The bounded recording ended normally. A subsequent shutdown request produced
console EOF and disappearance from host USB. Those observations do not prove
physical charge inhibition or a safe fault cutoff.

## REA discovery coverage

REA 6.5.0 with Ghidra 12.1.4 was evaluated on an exact extracted
AppleD1946PMU Mach-O module from the pinned 12H321 kernelcache. Module bytes,
headers and virtual addresses were retained. External dependency code was not
included in that module analysis.

* Original kernelcache SHA256:
  `db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c`.
* Module SHA256:
  `6d884a7b4f7596b0a02a0921f4bbc658142a184cebb6337d7d090748ea0b7e13`.
* Unseeded discovery found 54 regular local functions. Seven original-pointer
  qualified Thumb seeds raised this to 81, recovering 14 of 15 missing sites
  in a historical list of 70 literal write-slot loads.
* Sixteen further missing local entries in the constructor-qualified source
  vtable `0x80ed3c68`, offsets 0..0x88, are now recovered: 15 regular functions
  and one thunk. Including three automatically discovered routines, the final
  inventory contains 99 regular functions and 6855 instruction entries.

These are coverage counts, not complete call-graph or write coverage. The
Thumb seeds are analyst-supplied facts backed by original function pointers;
they are not independent Ghidra discoveries. A scoped analysis-only Java
overlay verified those pointers and set/read back TMode before disassembly.
The installed REA script and original module remained unchanged.

95 of 99 complete instruction-address sequences agree with the frozen listing.
Three ARM helpers require the separately verified original BLX mode transitions,
because that old listing used linear Thumb decoding. The fourth difference is
a shared import tail outside the old listing. Ghidra also places a shared event
helper in one caller's body; inline-looking pseudocode does not prove exclusive
ownership. The final import used two CPUs, a 1 GiB limit and no swap, peaked at
772067328 bytes and recorded no OOM. No whole-kernel reimport was needed.

## Event controls and source VBUS getter

Source slots +0x44/+0x48 (`0x80ed0720`/`0x80ed0730`) forward their respective
known nine-byte event masks to `0x80ecd8f4`, which visits set bits and calls
`0x80ecbe50`. Qualifier 1 clears the software masked/pending fields and invokes
PMU +0x370 to unmask. Other qualifiers set the software masked field. This
is event handling, not a direct charge-off operation. See the existing
[event](p105-source-events.md) and [IRQ lifecycle](p105-d1946-adc-lifecycle.md)
boundaries.

Source slot **+0x84** resolves to **0x80ed0b44**. It forwards channel **15**,
the output pointer and multiplier **0x10000** through stub `0x80ed1e3c`,
original import cell `0x80ed3064` -> `0x80ec8b11`, to the command-gate wrapper
at `0x80ec8b10`. PMU +0x394 resolves to the existing scaler `0x80ece490`.
The [ADC metadata](p105-d1946-adc.md) calls channel 15 `vbus`/`Vu0`.
This connects the source interface to that measurement path; it does not
qualify a connector pin, physical calibration, CV regulation or a cutoff.

The new [getter experiment](../../tools/power/check-source-vbus-getter.py)
executes the original source method, import stub, wrapper and scaler. The
command gate is mocked to dispatch the actual callback, and raw conversion
is mocked. Both PMU I/O entrypoints and the real conversion entry are refused.
The exact original source slot, import cell and PMU slot are checked first.

Six [tests](../../tests/test-source-vbus-getter.py) pass without skips:

* Successful raw fixtures produce `88 * raw`, with channel 15 verified at
  both mocked boundaries. This is the existing software encoding, not a
  physical calibration result.
* Raw-conversion errors propagate unchanged and preserve the caller's prior
  output, including a plausible old sample, zero and a missing-value sentinel.
* A command-gate error propagates without invoking raw conversion and also
  preserves the prior output.
* Unexpected I/O/conversion entries, invalid inputs, a wrong image and fixture
  reuse are rejected.

Therefore this getter's caller must check the return code. An unchanged output
alone is not evidence of a fresh sample. This is an interface requirement,
not proof that Apple's actual caller ignores errors or that Linux has that bug.

```sh
python3 tests/test-source-vbus-getter.py -v
python3 tools/power/check-source-vbus-getter.py \
  build/firmware/kernelcache.12H321.macho
```

Missing local firmware or Unicorn produces an explicit test skip, not a pass.
No Apple executable bytes, disassembly, decompiler output, device identity or
private RAM image is included in this update.

## Unresolved consumer and acceptance boundary

A bounded linear Thumb search across the pinned AppleARMPlatform text did not
establish the charging-policy consumer of source +0x84. Equal slot offsets
also occur on unrelated objects. Computed dispatch, other modules and analysis
coverage gaps remain possible; this search is not proof that the getter is
unused. Its error result cannot yet be connected to source revocation or a
charge-off decision. The independently tested battery-voltage +0x90 path must
not be substituted for this input-voltage interface.

Still required are P105-specific CV, termination, thermal and fault semantics,
their retention through the exact RAM boot chain, and effective physical
source-loss/fault cutoff. A software read error, status bit, USB allowance,
timer name, EOF or isolated positive current value cannot qualify those.
Acceptance requires controlled stored-charge increase under Linux with
plausible voltage/temperature and verified source-loss and fault behavior.
