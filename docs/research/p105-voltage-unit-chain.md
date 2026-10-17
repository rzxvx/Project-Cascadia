# p105 voltage software units: ADC/source/owner chain

Bounded offline evidence, 2026-10-12. Firmware SHA256:
`db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c`.
No device transfers, active ADC conversion, PMU mutation, CV-regulator change,
commits or changes to frozen research groups.

## Separate primary software-unit contract

Apple's [IOPMPowerSource.h at XNU2782.40.9, revision c2b85efb3cd2a727d605af388855308d90d0bd70](https://github.com/apple-oss-distributions/xnu/blob/c2b85efb3cd2a727d605af388855308d90d0bd70/iokit/IOKit/pwr_mgt/IOPMPowerSource.h)
lines108..111 explicitly specify unsigned `Voltage` in mV. This is a primary
software API contract. The firmware identifies itself as XNU2784.40.6~18;
the header is contemporaneous source, not a claim of byte-for-byte source
identity with that kernel. It does not certify analog calibration or PMU
constant-voltage target/termination settings.

## Dictionary path: already an integer, no rescaling

Actual updater **0x804c2f10..2f2e** retrieves key `Voltage` (literal
0x804c31e4 plus PC0x804c2f1e → string0x804cedde), casts to OSNumber and calls
its unsigned accessor at virtual+0x50. The resulting integer is directly
stored at charger-owner **+0x22c**. This consumer does not divide or multiply
that OSNumber, and it does not decode a raw HDQ packet. The primary API
contract establishes mV for the supplied Voltage property; it does not prove
that every external producer obeys that contract.

The complete producer connection back to AppleHDQGasGaugeControl is **not**
closed in this checkpoint. Its pinned prelink personality matches `gas-gauge,hdq`
under IOService and declares an AppleHDQGasGaugeHID plugin. The kext is loaded
at0x80cef000 with size0x9000. In the inspected kext string/import inventory no
literal `Voltage` publisher or direct import of kernel
`IOPMPowerSource::setVoltage` was located. This bounded negative lookup is not
proof of absence: a HID report/userland/plugin or indirect callback may supply
the property. That producer/report layout still needs tracing. No HDQ silicon
identity or physical unit is inferred from the current Linux gauge comments.

## PMU-measured path: exact callback and import identities

The ADC path uses the same owner field but starts with a fixed-point sample:

1. **0x804c4df0** invokes generic backend iterator **0x804c5520** with callback
   **0x804c55c9**; literal0x804c5024 + PC0x804c4dee supplies that callback.
2. Actual callback **0x804c55c8** calls backend virtual **+0x90** at55da..55e2.
   The actual D1946 source address point **0x80ed3c68** has this entry resolving
   to **0x80ed0bf5**.
3. **0x80ed0bf4** loads PMU owner from source+0x18, uses ADC channel **4**,
   passes the output pointer and multiplier **0x10000**, then jumps through
   stub **0x80ed1e3c** → import cell **0x80ed3064** → **0x80ec8b11**.
4. Actual wrapper **0x80ec8b10** takes the PMU's command gate from **+0x60** and
   invokes its +0x70 method with callback **PMU virtual+0x394**. Actual PMU
   address point **0x80ed3208** resolves +0x394 to **0x80ece491**.
5. Actual voltage scaler **0x80ece490** invokes raw conversion via PMU+0x3ac.
   For channel4, the previously verified scaler produces
   **encoded = 163840 + 32 × raw12**. The metadata getter identifies channel4
   as `vbat`/`VB0`. The wrapper's multiplier0x10000 and rounding arithmetic
   preserve this integer encoding in the tested positive range.

The offline fixture executes callback55c8, source0bf4, import/wrapper8b10 and
scalerE490. The command gate is explicitly mocked to dispatch its *actual*
E491 callback with PMU/channel/output arguments. Raw ADC conversion is mocked;
it never writes control register43 or reads a physical sample. The generic
collection iterator5520, actual command-gate locking and timing are not executed.

## Signed scale, compensation, then integer store

Actual current-estimation routine **0x804c4d40** prepares sample output with
INT_MIN. It treats returned INT_MIN as missing data. On a valid sample, original
instructions **0x804c4dfe..4e06** multiply the signed32 encoded value by **1000**
and accumulate it in the signed64 pair fp/r8 using SMLAL.

When owner+0x258 is nonzero, the full routine requests ten samples and divides
the accumulated quantity by10 at4e10..4e22. The bounded fixture uses one sample;
that averaging branch and its error scheduling are static observations, not
executed claims in this fixture.

At **0x804c5060..5072**, a separately selected signed compensation quantity is
added as **compensation <<16** to this accumulated fixed-point value. Its high
word handling uses the sign of the compensation's bit15. The experiment bounds
compensation to−1000..1000, where this sign-extension is consistent with a
signed16 value; it does not assume arbitrary signed32 offsets are valid.

The adjusted low word is stored on the caller stack at **0x804c50ae**. Later,
**0x804c52f6..5302** shifts that low word right16, combines it with the adjusted
high word shifted16, and stores the integer at **owner+0x22c**.

Thus for the one-sample bounded positive case:

```
encoded             = 163840 + 32 * raw12
fixed_millivolt      = signed64(encoded * 1000)
adjusted_fixed       = fixed_millivolt + compensation * 65536
owner_22c           = floor(adjusted_fixed / 65536)
```

The scale matches the primary mV software contract. The stored value can be
**policy-compensated**, not simply the instantaneous physical battery voltage.
At4e48..4e50 the routine separately publishes the unadjusted shifted value as
`BootVoltage` (string0x804cf3fd), before the later compensation. At5316..531e
the property is `UpdateTime` (0x804cf4c3), **not** a second Voltage publication.

Compensation is selected by the existing source/capacity policy around4e54..505c,
including source input-budget bands, owner+0x258 and owner+0x278. Its physical
justification, every configuration range and complete lifecycle are outside
this bounded arithmetic proof. It must not be interpreted as PMU calibration.

## Eight actual-code fixture tests

All eight tests passed with Unicorn2.1.4 and the pinned original image:

| Fake raw / compensation | Encoded ADC result | Stored integer |
| --- | ---: | ---: |
| 0 /0 |163840|2500|
| 2048 /0 |229376|3500|
| 3072 /0 |262144|4000|
| 4095 /0 |294880|4499|
| 3072 /+20 |262144|4020|
| 3072 /−15 |262144|3985|

Tests additionally cover 1/2047/4094, signed offsets−1000..1000, original
callback identities, no PMU transfers, distinct encoded/raw/stored values,
unchanged owner cutoff+0x24c, input guards and single-use fixtures. An injected
raw error makes55c8 restore INT_MIN and the probe omits the conversion/store.
The probe does **not** certify that the whole charger routine invalidates a
previous owner voltage after error, or that all raw ADC errors are detected
correctly by physical hardware.

The arithmetic stages run bounded original basic blocks with explicit fake
register/stack context and stop hooks:4dfe→4e0a,5060→5076,52f6→5306. The
capacity-interpolation block, intermediate50ae stack store and compensation
selection are not executed; the probe transfers their already computed values
through explicit fake context. These boundaries are not described as a full
execution of4d40 or as an exact live sample/clock sequence.

Reproduce from the repository with Unicorn 2.1.4 and the local original image:

```sh
python3 tools/power/check-voltage-units.py build/firmware/kernelcache.12H321.macho
python3 tests/test-voltage-units.py
```

The constructor, CLI loader and test loader verify the original firmware hash.
Tests explicitly skip when Unicorn or the optional image is missing; a present
wrong-hash image fails. `CASCADIA_TEST_KERNELCACHE` selects an alternate test
image. `CASCADIA_RESEARCH_REPO` optionally selects the source root for private
staging; ordinary repository invocation needs no source-root override. No
Apple firmware bytes, personal device dataset or active driver are added.

## Result and limits

Software values at owner+0x22c and `Voltage` use the mV convention, supported by
both actual *1000/shift arithmetic and Apple's contemporaneous primary header.
The raw ADC code, fixed-point encoding, unadjusted published BootVoltage,
compensated stored integer, and externally supplied OSNumber are distinct.
The ADC→D1946 backend→owner arithmetic connection is now concrete. The HDQ/HID
publisher→Voltage dictionary producer remains open.

Nothing here measures physical calibration, sensor accuracy, battery chemistry,
CV target, termination, temperature/fault protection, or safe charging. The existing
Voltage/gate artifacts remain unchanged; any later unit-contract refinement
must keep those separate hardware prerequisites blocked.
