# D1946 system watchdog: separate from charger protection

The pinned p105 image contains an `AppleD1946PMUWatchDogTimer` class with
concrete control and `keepact` paths. This is evidence about a system watchdog;
it does not establish a charger safety timer, charge-termination timeout or
safe periodic charger write. No watchdog, charger or device operation is
enabled by this research.

Source: official iPad2,5 iOS **12H321** kernelcache SHA256
`db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c`;
raw p105 ADT SHA256
`5c346586116de1bb32606acb27cad3158bcaa8a1318a10ee9631de09884cfd72`.
See [firmware provenance](p105-charging.md). No Apple executable bytes are
distributed. Fixed addresses apply only to this image.

## Constructor and start gate

Constructor **0x80ed16cc** stores actual vptr **0x80ed4138**: the literal
0x2a4e at 0x80ed16f4, add-PC at 0x80ed16de and subsequent +8 adjustment
prove the address point. Its relevant slots are:

| Slot | Actual target | Observed role |
| --- | --- | --- |
| +0x168 | 0x80ed1724 | Start gate/provider/property lookup |
| +0x344 | 0x80ed17b0 | Interval arguments 500 and 1000000 |
| +0x34c | 0x80ed17f0 | Enable/disable control path |
| +0x350 | 0x80ed17c0 | Toggle cached keepact level and dispatch |

Start **0x80ed1724** requires a PMU provider cast, then obtains the PMU's
provider and tests for property **`pmu-watchdog`**. The PMU's own vptr
+0x1cc resolves to **0x802ed369**, whose nlist symbol is
`IOService::getProvider() const`. The property is a presence gate here;
the inspected instructions do not parse a numeric duration from its value.
Absent cast or property returns false before further setup.

After that gate it looks up **`function-keepact`**, stores the function at
object +0x4a4, and logs that hardware watchdog support is inactive if the
function is absent. It still delegates to the generic superclass start
method. The superclass import at 0x80ed31c0 points to raw table 0x804d6730,
whose +0x170 entry is **0x804b8acd**. That table is a separate superclass
reference; its offset must not be applied to the stored D1946 vptr above.
The surrounding generic service/timer machinery is explicitly mocked in
the bounded experiment, so this does not validate its full initialization.

The complete PMU node in the hash-pinned raw ADT has **34 properties** and
no `pmu-watchdog` property. It does contain `function-keepact`, raw bytes
`0000000f4f4950470402000001010000`, identifying an ADT GPIO dispatch.
This is a static board-property distinction, not evidence that the watchdog
is inactive under running iOS: runtime IORegistry properties or earlier
initialization could differ. Physical GPIO ownership and watchdog wiring
are not inferred from the presence of that function.

## Actual control and keepact behavior

With a cached function present and cached enabled byte +0x4a8 false,
the actual enable path writes **PMU register 0x80, byte 0x2f**, marks the
enabled byte true and invokes actual slot +0x350. The disable transition
writes **0x80, byte 0x0e**, then clears that enabled byte. Repeating the
same request performs no further write or kick. With no cached keepact
function, either request does nothing.

Kick **0x80ed17c0** toggles cached 32-bit field +0x4ac between 0 and 1,
then calls keepact's ARMFunction slot +0x3c with a pointer to that field
and remaining arguments zero. It does not itself test the enabled byte.
The test therefore confirms that direct kick can toggle the synthetic
level while disabled; real periodic invocation is controlled elsewhere.
All GPIO/provider callbacks are mocked, so no physical pin is toggled.

Interval helper **0x80ed17b0** supplies 500 and 1000000 to import stub
**0x80ed20bc**, through cell **0x80ed3104** to
`clock_interval_to_absolutetime_interval` **0x800b5adc**. The independently
verified [clock conversion](p105-clock-intervals.md) establishes a **500 ms
software interval**, not a measured watchdog expiration time. This harness
records those arguments and returns explicit fake ticks; it does not run a
timer or assert the hardware reset deadline.

## Communication failures do not roll back the original cache

Actual instruction execution against fake I/O demonstrates:

* A failed enable write still marks +0x4a8 enabled and dispatches keepact.
* A failed disable write still marks +0x4a8 disabled.
* A failed keepact callback leaves its toggled +0x4ac level and enabled
  cache in place; no rollback occurs.

Thus software cached state does not prove that the physical watchdog changed
state successfully. These are original-code limitations, not a Linux error
policy. The harness reports `r0_after_return` literally: it is not labeled
an I/O success code, since early exits and control paths leave different
residual values and a complete public return contract has not been proved.

## Reproduction and protection gaps

```sh
python3 tools/power/emulate-pmu-watchdog.py build/firmware/kernelcache.12H321.macho
python3 tests/test-pmu-watchdog-emulation.py
```

Ten tests pass without skips in the existing offline image: raw ADT presence
gate; missing provider/property; explicit superclass results; missing keepact;
enable/disable/repeat behavior; absent function; keepact toggling; write/GPIO
failures; interval arguments; and hash/fixture guards.

The original D1946 start gate, control method and keepact method execute.
PMU I/O, provider/type/property lookup, function lookup, name/logging,
interval conversion and generic superclass start/table are explicit fakes.
The existing bounded instruction runner applies. No USB, I2C, device node,
system service, host network or hardware API is opened.

The following charging questions remain distinct and open after this work:

| Question | Current evidence boundary |
| --- | --- |
| Charger watchdog/safety timeout | System watchdog paths above do not identify one |
| Charge target voltage and termination current | Not established by the traced current setters, source init or this class |
| Runtime over-temperature/battery-absent shutdown | Status reporting and generic policy exist; complete hardware shutdown remains unproved |
| Source removal | Source callbacks notify generic charger policy; full removal/stop/IRQ ordering remains unproved |
| PMU communication failure | Multiple original caches/reads ignore errors; no complete fail-safe transition is established |

The boot-fault logging path and its explicit log-clear side effects are
separately documented in [PMU lifecycle research](p105-pmu-lifecycle.md).
No protective defaults, guessed PMU voltage values, charger heartbeat or
active Linux callback may be derived from these system-watchdog observations.
