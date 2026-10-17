# p105 charger ownership evidence and blocked-output contract

This group provides three separate offline artifacts: actual firmware voltage
owner/cutoff emulation, bounded actual startup floor/cap emulation, and a new
synthetic Linux design contract. The first two describe Apple's instructions;
the third is deliberately stricter software input validation. None is an
active charger driver, hardware test, or instruction to enable charging.

## Primary evidence and executed scope

The user-provided Apple iPad2,5 iOS 8.4.1 / 12H321 kernelcache is hash pinned:
`db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c`.
Provenance is recorded in [charging research](p105-charging.md). Programs and
tests ship no Apple instruction bytes. CLI/test loaders verify the original
hash before fixed addresses are used. Constructors require that already
verified image. No USB, SSH, sysfs, PMU bus or network access occurs.

### Actual voltage property owner

`emulate-voltage-owner.py` executes callback **0x804c3224** and current-ceiling
selector **0x804c3ef4**. The callback's owner is r0, backend/source r1, and
property dictionary r2, as shown by its prologue and caller **0x804c3174..3178**.
Key `ChargeLimitVoltage` resolves from literal **0x804c338c** plus PC
**0x804c32e8** to string **0x804cef47**. The actual callback casts the value to
OSNumber, uses its +0x50 accessor and stores to **owner +0x24c** at
**0x804c32fc**, then forwards the dictionary through **backend +0x9c**.

Dictionary lookups, numeric cast/access, owner name, logging and backend
forwarding are explicit mocks. The forwarded dictionary/owner and actual
OSNumber metaclass import are guarded. Source +0x9c is intercepted before any
backend operation. The inherited generic notification stub is not reached.
The fake owner has no current table, override or current cap, and both cached
inhibit flags are zero. Therefore a non-cutoff return of `0xffffffff` is the
actual **no-table −1 sentinel**, not a charger-current grant.

The selector compares reported field **+0x22c** against nonzero **+0x24c** at
**0x804c3f1e..3f2a**; equality or a larger unsigned sample chooses current
ceiling zero. Zero limit disables this comparison. It does not configure
constant-voltage regulation in the PMU.

Ten tests execute these boundaries:

| Fixture | Actual behavior |
| --- | --- |
| Numeric limit, sample equal/above | Stored limit; zero ceiling |
| Numeric limit, sample below | No-table −1 sentinel in this fixture |
| Missing or wrong-type key | Previous limit retained; backend still called |
| Numeric zero limit | Previous limit replaced with zero; cutoff disabled |
| Backend error | New owner limit remains stored; error returned, no rollback |

The sample and limit are **firmware numeric units**. Field +0x22c has a
`Voltage` OSNumber producer at **0x804c2f10..2f2e** (key at **0x804cedde**) and
another producer at **0x804c5302** in the pmu-measured path. The follow-up
[ADC-to-owner unit chain](p105-voltage-unit-chain.md) now proves the bounded
software conversion to the mV convention, including signed policy compensation.
The HDQ/HID dictionary producer, physical calibration and safe CV/termination
configuration remain unverified. The limit backend is not a proven analog CV
actuator. Fixtures 4000/4200 are chosen integers, not measured battery values
or recommended targets. Numeric
boundary tests deliberately include 1 and UINT_MAX; those test arithmetic,
not permissible physical voltages.

### Actual startup floor/cap slices

`emulate-input-clamp-owner.py` executes these original blocks with synthetic
r4 owner, r8 provider, stack, and explicit imported OSData metaclass context:

- **0x804c622e..6288**: floor **+0x138**, stop before **0x804c628c**;
- **0x804c62cc..62d0**: cap **+0x14c** default1000, stop before **0x804c62d4**;
- **0x804c6326..6346**: provider cap override, stop before **0x804c634a**.

The full startup resource checks, initialization, provider lifetime and
failure cleanup are **not** executed. PE boot-argument parsing, provider
getProperty, OSData cast/length/byte access are mocks with checked owners,
keys and expected metaclass. Stops are emulator hooks; Apple code bytes are
not patched or copied into the repository.

Eight tests confirm:

- Boot `force-usb-power`: 0→0, 1→500, unsigned values ≥2 stored directly. A
  present boot argument takes precedence over the provider property.
- Without it, absent/wrong-type `force-usb-power` property selects floor0.
  OSData length4 supplies its first u32; other reported lengths select500.
- Cap defaults1000; a valid `usb-input-limit-max` OSData overrides it.
  The local cap block does not call its length accessor.

The deliberately malformed length fixtures still provide four backed bytes.
Their cap result proves an omitted local length check; it does **not** emulate
an actual out-of-bounds read, provider memory lifetime or safe malformed-data
handling. No host/iPad boot arguments or properties are changed.

The boot-argument trampoline **0x804c9494 → import0x804d006c → 0x8033732d**
is identified by Mach-O nlist as `_PE_parse_boot_argn` at **0x8033732c**.
Floor key literals **0x804c6494/6498** with PC **0x804c6238/6256** both resolve
`force-usb-power` at **0x804cf5c4**. Cap literal **0x804c64b0** with PC
**0x804c6336** resolves `usb-input-limit-max` at **0x804cf5f7**.

The pinned p105 raw ADT (SHA256
`5c346586116de1bb32606acb27cad3158bcaa8a1318a10ee9631de09884cfd72`)
has charger property `usb-input-limit-max=2100` (`34080000`) and no
`force-usb-power` among its complete19 properties. Runtime boot arguments and
IORegistry properties remain unknown. A board cap is not USB-host current
capability. The [actual dispatch tests](p105-input-dispatch.md) already prove
that this floor can raise an initial inhibited zero before the cap is applied.

## Stricter future Linux input contract

`charger-evidence-gate.py` is a **new synthetic design prototype**, with no
firmware execution and no claim to reproduce Apple's full state machine.
It keeps an epoch and invalidates queued old snapshots on attach, detach,
role change, reset, fault or communication error. Each complete snapshot
requires all of the following:

- Matching epoch; known source class and explicit permission/source validity.
- Battery-present, temperature-valid, fault-snapshot-valid and backend-ack
  booleans supplied explicitly, not inferred from a numeric cache.
- Requested input budget and separately validated board cap; nonzero
  `force-usb-power` floor is rejected for additional review.
- Voltage sample **and** software cutoff limit, each with explicit value,
  consistent `12H321-policy-units`, validity and freshness. Unknown physical
  calibration is never silently converted to mV.

Missing/revoked/stale/unknown input yields review candidate0. A zero software
limit is rejected even though Apple code treats it as disabling that check.
USB2 request above500 is rejected without adapter classification; board cap
above2100 is rejected. These prototype bounds use existing documented USB2
budget/p105 board constraints, without performing adapter negotiation.
Malformed inputs raise an error before constructing any actuator plan.

A complete valid synthetic snapshot may produce
`candidate_input_ma_for_review_only`, bounded by the cap and the software
cutoff. **It always produces input_current_ma0, charge_current_ma0 and an empty
register_operations list**, independently of that review candidate. Three
unproven prerequisites remain hardcoded:

1. Hardware constant-voltage target configuration.
2. Hardware termination configuration.
3. Complete fault shutdown and initialization.

Input JSON cannot mark these proofs complete, supply guessed registers or
unlock outputs. Software validity/freshness booleans themselves are fixtures;
the prototype implements no Linux notifier, ADC/calibration or hardware ack.
Its ten tests cover every missing/negative input, stale epochs, invalid units,
equality cutoff, floor/inhibit ordering, malformed input and the permanent
output block. It complements, without replacing, the narrower
[USB budget lifecycle model](p105-usb-charge-budget.md).

## Reproduce and test prerequisites

```sh
python3 tools/power/emulate-voltage-owner.py build/firmware/kernelcache.12H321.macho
python3 tools/power/emulate-input-clamp-owner.py build/firmware/kernelcache.12H321.macho
python3 tests/test-voltage-owner-emulation.py
python3 tests/test-input-clamp-owner-emulation.py
python3 tests/test-charger-evidence-gate.py
```

Actual-code tests require Unicorn2.1.4 and the local hash-pinned image. They
explicitly skip when either prerequisite is missing; an existing wrong-hash
image fails. `CASCADIA_TEST_KERNELCACHE` selects the test image. The pure
contract tests run with standard Python only. `CASCADIA_RESEARCH_REPO` is an
optional source-root override for private staging; ordinary repository use
needs no override.

All28 tests passed in private staging before publication into the local
repository. Missing-image runs produced explicit10+8 skips; the ten pure
contract tests remained runnable. No connected-device result is included.

## Next implementation conditions

Establish actual CV/termination/fault configuration and its initialization,
resume, interrupt and detach owners; prove sample units and freshness; then
implement serialized Linux events with cancellation on source loss/error.
Acknowledged PMU state must remain distinct from cache updates and transfer
errors. Source removal→input/charge zero is still a partly traced software
chain, and electrical shutdown is untested. This group is a reviewable contract
and reproducible source evidence, not completion of safe Linux charging.
