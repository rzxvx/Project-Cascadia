# Isolated checked P105 staged-power primitive

Source-only and **default n**, no DT/driver/gadget/HCD registration, native
clock lookup, MMIO mapping, connector operation or device access. Normal PHY0,
peripheral gadget and WLAN remain unchanged. It does not return full host
client-state5 or connector-role success. Native VBUS, charging and storage are
outside its scope. This is the next concrete register backend primitive after
`register-backend/`, not a functional first-boot claim.

Actual C calls **clk_get_rate** on a supplied real `struct clk *`, and checked
Regmap APIs on supplied actual mappings. No fixed guessed rate/DT clock and
no fake native clock provider exists in the source. The tests replace those
APIs with explicit fake providers; their successful returns do not establish
a native provider exists. Linux P105's current PHY node supplies no such clock
binding, so no current code can legitimately instantiate this primitive.

## Original instruction evidence

See `power-backend-next/EVIDENCE.md` and its three pinned12H321 instruction
oracles. That cache SHA256 is
`db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c`.
The native power helper`80cabea8` and exact register accessors run in the
power oracle; Arbitrator`+34c` runs the actual `80b3a78c` control-bit helper.
Clock/power-gate membership callbacks are fake. Frequency discrimination is
bounded original startup code, with only the actual named-rate API simulated.
Both full-hash P105 ADTs have the same required static board parameters.

The C up/down stages reproduce those actual register and delay operations,
including different reset bits for0→1 versus1→2, original complete static
writes for PWR/CLK/TUNE/hostset, and original preserving RMW operations on
reset/power/control fields. Level3↔4 only changes metadata; repeated same-level
requests do not repeat register operations. The board-specific `+5c` tuning
write is omitted because P105's actual phy-tuning-val is zero.

The original power-gate-family callback consumes a `power-gates` property,
which both verified P105 ADTs lack. A required boolean binding assertion
covers that explicit board capability; no required regulator or native
provider is mocked as success. The normal clock-membership callback remains
**mandatory**, must update one reserved owner's actual idempotent membership,
and must propagate incomplete native parent/last-release support. It cannot
be a generic phy_power_on/power_off count, static success or raw unowned put.
The primitive checks its callback errors before further PHY operations.

## Required ownership and state adoption

All flags deny use by default. Before any named-rate API or I/O, the future
role owner must attest the exact P105 binding, absent power-gates property,
complete clock/PHY/controller memberships, every affected owner's quiescence,
an exclusive transition lease and verified actual initial state adoption.
Those attestations are **caller contracts, not physical facts established by
this module**. No native implementation sets them yet. Cold level0 is not
inferred from freshly zeroed C storage while iBoot/USB/WLAN remains active.
Current generic-PHY0/1 independent counts do not meet complete ownership.

An active adopted state additionally needs its explicitly verified
`adopted_clock_hz`. Real clk_get_rate must match it; otherwise a fault is
latched before I/O. Exactly12/24/48MHz selects0/1/2, proven by original start
instructions; other rates fail before I/O. The clock rate is rechecked after
each register stage. The transition lease must prevent foreign parent-clock
changes; a detected rate change latches ESTALE and never becomes success.
The P105 ADT ref-clock-sel3 is separately used in the PWR static write and
must not substitute for the frequency code.

One caller-supplied **shared owner mutex must already be held**, covering all
clock, PHY, HCD/UDC, connector and power operations across modules. Lockdep
asserts that requirement; tests assert it with a fake mutex. This is not an
independent per-PHY mutex or a global core count manipulation. Without all
native clients using that same owner protocol, the binding must stay denied.
The role-owner worker context may sleep; no IRQ/spinlock callback is used.

## Epoch/error disposition

Every Regmap read/write and clock callback is checked; writes and membership
callbacks recheck connector epoch immediately after return. Delay boundaries
and stage/final metadata commits also check epoch. Failed writes/callbacks may
have applied effects, so a touched error latches its original negative error,
retains pending target and last fully completed intermediate stage, and
refuses all subsequent transitions. No invented rollback, forced reset,
false success, count repair or cleanup using another connector epoch exists.

Recognized stale/unsupported clock or epoch for an already adopted active
state also latches fault even if this module has not written yet. Reinit
returns EALREADY without erasing active or fault state. Invalid/null/error
Regmap/clock resources are refused. Initial cold invalid clock/epoch before
any touch leaves no phantom target, allowing only owner-controlled retry.

Successful intermediate stages may update the last completed level before a
later stage fails; the fault makes that partial disposition explicit. That
metadata is not a voltage measurement. A final check may fail after the last
stage record advances; the API still returns the error and holds fault.
No recovery operation or native membership adoption is implemented here.

## Validation and remaining integration

16 offline unittest groups PASS/no skips. They compile **actual candidate C**
with explicit fake regmap/clock/epoch/clock-membership implementations and
compare all25 start/target level combinations plus a repeated multicycle
sequence with actual pinned firmware instructions. They cover all I/O and
membership errors in both directions, every epoch-check location, final
commit, all observed rate-change boundaries (including metadata-only stages),
exact rates, default flags, missing actual callbacks, active-rate adoption,
resource errors, preserved32-bit RMW fields and reinit retention.

The fixtures' arbitrary initiallevel/register images are synthetic. Equality
with original instructions does not certify those images represent a valid
native adopted state or that parent-clock/rail providers exist. Original
power-gate callbacks are deliberately filtered only after the independent
missing-property/code proof; this is not a hidden successful required power
provider mock. Full cold PHY power-up/host timing on physical hardware is
unverified.

The source also compiles as an ARM object against real Linux6.12 generated
headers, ARMv7 kernel defines, with build-equivalent header warning exclusions
and -Wall -Wextra -Werror, zero warnings. No full kernel/config/DT build change.
Native named clock provider, complete owner migration/adoption, reset-client
membership add/remove, controller/MUX orchestration, accessory ID/authentication,
external VBUS/protection and approved device validation still block binding.

Portable test: `python3 tests/test-experimental-usb-power.py`. Original semantic
oracles are normalized fixtures with synthetic providers and no firmware bytes.
This directory has no normal build hook, DT binding or installation step.
