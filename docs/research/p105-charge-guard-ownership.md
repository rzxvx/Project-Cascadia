# Charger guard fields: concrete owners and remaining protection gaps

This is manually checked static evidence from the official p105 **12H321**
kernelcache SHA256
`db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c`.
The raw ADT hash is
`5c346586116de1bb32606acb27cad3158bcaa8a1318a10ee9631de09884cfd72`.
No code, property or register was changed on the iPad. The observations
resolve several software-field owners; they do not establish a complete
charger fault-shutdown mechanism. See [firmware provenance](p105-charging.md).

## +0x1cd comes from `pmu-measured`, not a fault bit

During generic charger start, **0x804c2328..0x804c233e** reads property
`pmu-measured` from object +0xa8 and stores its presence boolean at charger
**+0x1cd**. PIC literal **0x804c24dc**, used at 0x804c232e / add-PC
0x804c2334, resolves to the exact string **0x804cebfe**. A present property
becomes 1; absent becomes 0. The property value is not parsed in this path.

The property reader **0x804c2d44** obtains `CurrentCapacity` (string
0x804ced68) as an OSNumber. On its first report, when +0x1cc is zero,
**0x804c2de4..0x804c2dfa** checks the same `pmu-measured` property. If absent,
it clears +0x1cd, marks +0x1cc true at 0x804c2e00, publishes a
`BootBBCapacity` property, then caches reported capacity at **+0x1e0**
at 0x804c2e18. Literal 0x804c3168 and add-PC 0x804c2df0 resolve to the same
`pmu-measured` string. Thus the field's policy provenance is established;
it must not be renamed battery-overtemperature or hardware-fault status.

The complete charger node in the pinned raw p105 ADT has **19 properties**
and no `pmu-measured` property. This does not establish the running iOS
IORegistry state: the provider/property object and earlier runtime updates
must still be verified. The field's downstream behavior remains more complex
than its presence gate, especially across periodic measurement policy.

## +0x1d0 is a minimum-current policy parameter

Start **0x804c2320..0x804c2324** sets +0x1d0 to **300**. The next property
lookup at 0x804c2348 / add-PC 0x804c234e resolves literal 0x804c24e0 to
string **0x804cec0b**, `min-charge-capable-current`. A valid data object
replaces the field at 0x804c236c. The p105 ADT supplies **1000**, already
recorded in the [charging policy research](p105-charging.md).
This is a current policy parameter, not a watchdog timeout or 300-second
delay. The adjacent `charger-sleep-interval` property is distinct.

Additional original-instruction evidence narrows this field: actual consumer
`0x804c40ac` calls accumulator `0x804c4168` over port metadata, then conditionally
updates two flags, using separate1000/300 comparisons. A total500 yields flags1/0
in the pinned synthetic fixture, with no PMU transaction. This is not a direct
hardware USB-budget enable/disable rule. Complete flag interpretation remains
outside that bounded trace; it does not exclude physical500mA trickle charging.

## +0x1c1 is used as a transient inhibit

Method **0x804c4c44** clears publication/phase fields +0x221/+0x224 and
capacity-related +0x1e8. At **0x804c4c5a..0x804c4c7c**, it sets +0x1c1
when current capacity +0x1e0 is **INT_MIN**, or when +0x1cd is nonzero and
byte +0x25c is zero. Otherwise it clears +0x1c1.

The base table reference at 0x804d040c points to raw table 0x804d9580;
its +0x368 entry resolves to **0x804c838d**, a trampoline to source-budget
replan **0x804c75b8**. Thus the temporary inhibit is set before that replan.
If inhibited, later calls refresh helper 0x804c4d40 and policy/property
helpers, then clear +0x1c1 at 0x804c4cdc. The unknown-capacity branch also
consults `boot-voltage`, string **0x804cf3f0**.

Separately, periodic method **0x804c5370** computes another +0x1c1 value at
0x804c548e using source/phase/capacity conditions. It toggles phase byte
+0x224 at 0x804c54d0 and schedules further policy work. The full lifecycle
and +0x25c owner remain open. These call sites show that +0x1c1 is not
simply a directly latched thermal-fault bit.

The already tested current-ceiling function **0x804c3ef4** returns zero
if +0x1c0 or +0x1c1 is nonzero. +0x1c0 has a separate property owner:
**0x804c3038..0x804c305c** caches `IsCharging`, string 0x804cee0b, from the
property object. A property name does not make this a measured current or
an independent hardware protection signal.

## Timestamp +0x1c8 uses `adapterInfo()`

Method 0x804c4c44 calls **0x804c96a4**, whose imported pointer at
**0x804d00f0** is **0x80302e35**. The image's nlist identifies the even
address 0x80302e34 as `IOPMPowerSource::adapterInfo()`.

If that report is nonzero and +0x1c8 is zero, the method requests
`kern.monotonicclock` and stores its result at 0x804c4d10; sysctl failure
stores zero. If adapterInfo is zero, it clears the timestamp at 0x804c4d18.
Calling it an attachment timestamp describes its inferred role in recovery;
its concrete producer is adapter information, not a direct VBUS measurement.
The separate recovery consumer tests `externalConnected()` explicitly,
as documented in [charging status](p105-charge-status.md).

## What this still does not establish

No traced owner above establishes charge target voltage, termination
current, battery-presence interlock or hardware overtemperature shutdown.
Source removal still requires verification of source classification,
notification/debounce ordering and the eventual zero-current transaction.
An unknown capacity or failed status read must not be presented as a healthy
battery in a future Linux driver. Likewise these policy gates cannot replace
the unproved hardware fault and voltage configuration.

The guards are static firmware observations, not new actual-code tests or
a Linux charger implementation. Existing offline policy/status tests retain
their documented boundaries. The distinct [system watchdog](p105-pmu-watchdog.md)
also supplies no proof of a charging safety timer. Active PMU charger and
IIO callbacks remain disabled.
