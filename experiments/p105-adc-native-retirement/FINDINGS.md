# ADC completion, IRQ ownership and the remaining physical postcondition

Private frozen research candidate, 2026-10-12. No shared source changes, device
accesses, services, global configuration, builds, commits or network actions.
Seven new bounded actual-instruction groups pass. No physical quiescence witness
is produced. All Apple code remains in the user's authenticated local images;
this package contains authored harnesses, addresses and synthetic fixtures only.

## Exact native provider chain (12H321)

The D1946 actual vptr is `80ed3208`. Its start method obtains its provider using
slot `1cc -> 802ed368` (IOService::getProvider). It creates an interrupt event
source through imported cell `80ed30a8 -> 803051a5`, owner=PMU,
action=`80ecbca5`, provider=this provider, source index=0. PMU stores the event
source at `+80`, adds it to its WorkLoop and enables it. These steps provide an
owner/caller identity; they do not acknowledge or cancel an ADC conversion.

The authenticated ADTs carry PMU interrupt-parent raw `0000000f`, matched to
GPIO's AAPL,phandle; interrupt specifier `1300000001000000` has native LE cells
[19,1]. GPIO in turn has AIC interrupt119. Newly executed core
`IOService::getInterruptType 802f34b4` resolves the controller (explicit fake
resolver from the pinned ADT relationship), then calls actual GPIO vptr
`80df1938 +348 -> 80ded634`, returning native IOKit type1. Actual
`IOInterruptController::registerInterrupt 8031df88..8031dfba` consumes raw19
and computes `vectors +19*32`. The test stops before vector lock/share/init
side effects. Actual getProvider's coherent registry-cache path is executed;
the registry relationship/cache generation is a fixture. It does not prove
full registration, Linux trigger numbering, electrical polarity or a19->23
translation. No such translation was found.

Static native GPIO checks: `initVector 80ded764` uses index19*4, flags1 becomes
mode6, and the controller's eventsource interprets native type1 as auto-disable.
For mode6, handler `80ded67c` calls the registered action before ACK at
`880 +group*4`. Mask `80ded7ec` writes `800 +group*4`, re-enable `80ded814`
ACKs level state first, then writes `840 +group*4`. These are native logical
MMIO offsets, not a proof that all hardware activity has drained.

## Newly executed drain counterexamples (12H321)

The real kernel eventsource methods are mapped from the pinned kernel image:
`disable 803052f4`, `checkForWork 80305398`, `enable 803052cc`.
The PMU action is `80ecbca4`, event reader `80ecc460`, and ADC callback
`80ecbe2c`. Provider enable/disable, PMU reads and command-gate wakeups are
explicit fakes. Each result denies a physical witness.

1. Disable changes enabled metadata and calls provider disable, but does not
   consume producer/consumer counts. A queued work item still invokes the PMU
   action after disable and updates its consumer count.
2. PMU IRQ dispatcher ignores an event-read failure. A deliberately preexisting
   stack buffer containing ADC event02.5 results in real callback state1->2.
   The stale contents are explicitly supplied; real hardware behavior is not
   presumed. This proves failure cannot establish a valid completion record.
3. Hardware event bytes all zero still result in ADC state1->2 if resume cache
   flag `PMU+9a` and cached bitmap `PMU+9c` contain ADC event02.5. Hardware
   ACK/read success alone does not drain the software cache.
4. After its action, auto-disabled source checkForWork calls provider enable
   again when no explicit-disable flag is set.
5. Provider disable's synthetic I/O error is ignored by the actual void
   eventsource disable method; enabled metadata still becomes zero. That field
   cannot certify successful hardware masking.

ADC registration in startup uses packed event `120`, callback `80ecbe2d`,
table entry13 at `PMU+50 +13*32`. Mask/unmask slots36c/370 resolve to
`80ecc608/80ecc674`, with register13 bit20 and mask cache `PMU+b1`.
Unmask helper `80ecbe50` clears per-entry masked/pending bytes; it does not
clear the resume bitmap or drain WorkLoop queued eventsource actions. The
callback only tests software `PMU+94 ==1`, sets2 and wakes its gate. It contains
no sample epoch, conversion identity, IRQ generation, stop verification or
physical busy proof.

## Concrete 13G36 caller/vptr proof, not a transplanted offset

The 13G36 AppleD1946PMU constructor at `81144ecc` computes and stores actual
vptr `8114d214`. Allocation/base/context helper calls are explicit mocks.
Native getter `81147d58` loads this object's slot388 at `81147d68`, BLX at
`81147d6e`; slot388 is `811472ad`. The bounded original-instruction test
reaches `811472ac` with PMU owner/channel6 and stops before conversion. Other
static getter callsites are `81147c44`, `81147ca4`, `81147eba`, `81147ece`,
`81147f20`, `8114800e`. This proves the constructed D1946 class's method route;
there is no live iPad execution/IOKit-personality-selection measurement.

The new base-kext generic acquisition at `811411a4` appears to check its
start/stop callbacks, but the D1946 actual slots are:

| Slot | Native target | Executed result |
| --- | --- | --- |
|38c|81149b88|0, no hardware access|
|390|81149b8c|0, no hardware access|
|394|81149b90|E00002C7 unsupported|
|398|81149b98|1, no hardware access|

It is therefore incorrect to treat the base generic checked stop as a real
D1946 hardware-cancel primitive. The startup ADC registration imports callback
from cell `8114d14c ->81141181` in AppleDialogPMU, rather than local D1946 code.
That real callback uses software state `PMU+68`: states0/2/3 are unchanged;
1->2 calls gate wakeup(event=PMU+68, oneThread=0). The four actual callback
fixtures pass; they do not prove event origin/freshness.

Static legacy `811472ac` still writes43 for setup/start, reads44 on success,
and writes43 for cleanup; setup/start/cleanup transport errors are not checked,
while sample-read failure propagates. Main IRQ `81145568` calls event reader
slot39c (`81145d10`), still ignoring its result and merging software cached
flag+ a4/bitmap+ a6. No base-kext name or callback offset establishes a new
physical stop guarantee.

## Specific missing postcondition and implementable software boundary

Neither image proves that writing43 low channel bits or observing start bit20
clear permanently prevents a late ADC completion02.5, or guarantees that44
belongs to the current request. Earlier iBoot13 polling intends a bit20 busy
wait followed by44 read/43 cleanup, but successful I2C/status polling is not an
authenticated hardware cancel/drain contract. Native Linux's existing owner
can provide bus/conversion exclusion, checked transfers and epoch invalidation;
it cannot manufacture this missing analogue fact.

A real software-side drain would need the same exclusive central event owner
for01..09, verified mask13.5 with checked masked I/O, actual Linux IRQ disable
and synchronize, queued work drain, software resume bitmap/pending invalidation
under the same conversion owner, then checked completion-status acquisition
and generation association. PMU event bytes01..09 are consumed by multiple
native clients, so an ADC child cannot silently read them independently.
This group supplies concrete software counterexamples for those requirements;
it adds no alternate runtime consumer/positive witness. In particular an
IRQ synchronize alone does not prove the analogue engine stopped, and a
software cache flush cannot prevent a later physical IRQ.

A positive physical witness still needs an authoritative D1946 ADC cancel /
completion-latch postcondition, or a bounded experimentally verified electrical
and interrupt sequence on the device after explicit permission. None of the
above mock successes supply that assertion. No CV/termination/charger current
permission, bank switching authorization or manufacturer register map is
inferred here.

## Provenance, reproducibility and packaging

`source-provenance.json` pins both authenticated local kernelcache hashes and
all three existing authored library dependencies. `verify-group.py` verifies
those dependencies before emulation. Optional inputs/dependencies absent:
explicit SKIP. A mismatching existing input/dependency fails.

Local authoritative artifact provenance is the parent's official IPSW
plaintext manifests; offsets/symbols from the pinned images are primary
binary evidence. No third-party chip register claims are used. Exact image
hashes are in the provenance JSON. The primary open-source API names can be
crosschecked in Apple's XNU source, independently of the binary proof:
https://github.com/apple-oss-distributions/xnu/tree/xnu-2782.40.9/iokit/Kernel
This group does not assume uninspected source text matches these compiled
implementations.

Replay requires existing Unicorn and both locally supplied authenticated kernelcaches:

```sh
CASCADIA_SOURCE_ROOT=/PATH/TO/CASCADIA \
CASCADIA_TEST_KERNELCACHE=/PATH/TO/kernelcache.12H321.macho \
CASCADIA_TEST_KERNELCACHE_13G36=/PATH/TO/kernelcache.13G36.macho \
python3 experiments/p105-adc-native-retirement/verify-group.py -v
```

No script downloads or installs a dependency. Optional missing inputs/dependency
produce explicit skips; the independent full-source replay had7 PASS/0 SKIP.

Overrides: CASCADIA_SOURCE_ROOT, CASCADIA_TEST_KERNELCACHE,
CASCADIA_TEST_KERNELCACHE_13G36. The seven groups include four state fixtures
inside the13G dispatch group, not seven hardware tests. No ARM compile is
needed: this group adds no Linux-C runtime implementation. Earlier successful
parent/ADC/clock/IRQ suites were not rerun.

Proposed review subset: FINDINGS.md, source-provenance.json, verify-group.py,
emulate-drain-private.py, emulate-provider-private.py, gpio-fixture.py,
emulate-dispatch13-private.py, existing12 result JSONs. Raw private disassembly
and full image scans must remain outside any public export. GPL authored source
only; never export Apple firmware/code bytes.
