# Further-opt-in owned USB2 current selection

This source-only experiment addresses a concrete policy mismatch: the actual
owned USB2 observer publishes at most500mA, while the previous frontend selected
nonzero charge only for nominal input>=1000. Even a successfully completed500mA
configuration could never select a charge-current plan. This is separate from
physical current calibration, analog CV/termination/protection and thermal-slot
identity, which remain unverified. No hardware operation or normal configuration
change is performed; all drivers/DT remain disabled and all existing analog
retention/source/gauge assertions still apply.

New read-only module parameter allow_usb2_current is false by default. Only the
owned source route may use this further opt-in. Its snapshot budget must be
<=500000uA, and nominal input must be a known positive profile<=500mA. The charge
request is min(requested charge, existing720 experiment maximum, nominal input),
then floored to40mA codes. Hence nominal100 selects at most80; nominal500 selects
at most480. No guessed reserve current is subtracted. The original zero maxima
for4b/4d remain; no source-class/thermal-bucket association is invented.
Inherited charge ceiling still clamps the code through the unchanged core.
Legacy supplier route retains its existing>=1000 restriction.

The owned source's real constructor/validator still requires the reviewed
fixed-peripheral DWC2/composite binding, successful SET_CONFIGURATION and actual
EP0 status completion, standard USB2 speed, source generation and250ms lease;
combined reader still requires the actual managed HDQ reader and coherent fresh
sample. No positive grant is manufactured by this policy. Stock cdc2 currently
advertises0mA, so its real observed budget remains0 and this opt-in alone cannot
make that stock configuration charge. Descriptor/current-hook and analog handoff
validation remain separate requirements; the protocol budget is not measured
USB power or a proof that the cell is physically charging.

Existing worker status bits/experimental voltage-temperature stop window,
source freshness, error latch, worker retry bound, UDC invalidation notifier,
queue fence and before/after-enable epoch checks are unchanged. No active new
register, analog configuration, thermal cap, firmware table or recovery recipe
is introduced. The retained_analog_confirmed parameter remains an unproved
external assertion; no instruction here recommends setting it or activating the
candidate. The C code can be reviewed/compiled offline without physical testing.

## Evidence / tests

The independently executed original13G36 iBoot policy+setter chain in the private low-input-owner13 comparison proves software100/500 profiles coexist with nonzero charge
requests and preserves00003f3f3f slot maxima. Its source/raw flags, ADC/gauge and
transport values are explicitly synthetic, not actual USB permission. Cached
ceiling2520 and temperature-error fallback are firmware counterexamples and are
not adopted. Existing pinned12H321 original D1946 setter fixtures independently
prove the100/500 fallback codes,40mA floor and thermal-slot maxima used by the
unchanged Linux current core. Neither source proves autonomous analog safety or
physical charging at a particular USB budget.

Seven actual-worker ASan/UBSan tests PASS, including the prior three fault/retry
checks. New cases execute defaultoff versus opt-in budget0/100/500, each40mA
request0..720, non-multiple100→80, inherited ceiling1→40, oversized owned budget
rejection, unchanged legacy behavior, stale source before/after I/O, expired
sample, epoch change during final enable, absent/offline/thermal/statusfault,
every transport failure in a successful plan and bounded repeated supplier
EAGAIN/EIO/ENODEV. Positive source/PMU/gauge samples in these worker tests are
explicit test fakes, not the real production permission producer. Its separate
actual-composite/EP0/HDQ tests remain unchanged in the notifier export.
Actual ARMv7 frontend/core objects compile against real Linux6.12 W=1,0warnings.

This group requires the frozen p105-notify-stage replacements first. The
portable export changes only frontend C, adapts the prior fault-worker fixture
to include the actual new pure selection helper, adds the small-worker test and
this doc. No other core/headers change. This policy still cannot enable stock
cdc2 charging because its completed advertised budget is zero.

The separate [CDC2 declared-demand candidate](../p105-gadget/EVIDENCE.md)
provides source-only default0/opt-in100/500mA descriptors. Actual composite and
completed EP0/source-reader tests close the former all-zero protocol-budget
gap for that candidate. The normal gadget remains unchanged; a USB declaration
does not replace analog protection, calibration or a wall-charger detector.
