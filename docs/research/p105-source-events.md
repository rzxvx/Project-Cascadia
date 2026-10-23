# D1946 source events and notification boundaries

Research date: 2026-10-11. All instructions come from the official iPad2,5
12H321 kernelcache, SHA-256
`db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c`.
Addresses are specific to that image. No hardware was accessed. This is
offline evidence for a future charging implementation, not an active driver.

## Exact callback and software state

Power-source initialization `0x80ed0558` registers callback `0x80ed06a4`
with two nine-byte masks: `08 00 00 00 00 00 00 00 00` at `0x80ed2fdc`,
and `10 00 00 00 00 00 10 00 00` at `0x80ed2fe5`.
The callback's incoming value packs byte index into bits above bit 7 and
the event bits into the low byte. It indexes the second mask and checks
whether an incoming low bit matches that mask.

The source object field +0x2a has a concrete producer: input-current setter
`0x80ed086c` computes the register 0x3d code, caches that byte at
`0x80ed08a8`, then writes the code to fake or real PMU through its provider.
Thus this field records the software-requested input code; it is not a
silicon revision, battery temperature or measured current. With no calibration
the actual setter produces cached codes 2, 10, 34 and 72 for requests
100, 200, 500 and 1000 mA respectively. The established decoder maps
code 2 to 100 mA. The tested 1000 mA request decodes to 975 mA.

## Notification masks, not invented electrical event names

If the incoming event matches mask `0x80ed2fe5`, the callback invokes
import stub `0x80ed1f6c`, bound at `0x80ed30b0` to ARMPlatform method
`0x804c5924`. That method forwards notification mask **1** to
`0x804c2cfc` if the generic charger owner is present.
No additional PMU status read occurs on this branch.

Otherwise the callback executes status reader `0x80ecc450`, which reads
six bytes 0x0a..0x0f. It forwards through import `0x80ed1f5c`, bound at
`0x80ed30ac` to method `0x804c5908`. That adapter emits mask **6** only if
all three conditions hold:

1. Cached software input code +0x2a is at least 10.
2. Status register 0x0b bit 0 is set.
3. A further read finds live input register 0x3d equal to 2.

If any condition is false, the adapter emits mask **2**. The whole generic
notification consumer is asynchronous/workloop-aware and remains outside
this experiment. Neither mask has been renamed to a hardware fault, battery
absence or attach/detach condition without further consumer evidence.

The cached-versus-live input comparison is useful progress: this callback
can distinguish an input code that has dropped to the 100 mA setting from a
larger requested setting under a particular status condition. It does not
establish why the hardware changed, prove automatic fault shutdown, or
justify an automatic charger restart.

## Actual-instruction tests and error limitation

`tools/power/emulate-source-events.py` runs the actual callback, actual
status reader, import stubs and notification adapters with a synthetic PMU.
Only PMU I/O and the downstream generic charger notification consumer are
mocked. Its fixture uses the actual input setter to populate +0x2a; its
synthetic PMU setup is never a device command. The event callback itself
performs only the recorded reads in these fixtures.

```sh
python3 tools/power/emulate-source-events.py \
  build/firmware/kernelcache.12H321.macho
python3 tests/test-source-events-emulation.py
```

Seven tests passed inside the existing resource-limited image with Unicorn
2.1.4: code provenance, both registered mask positions, the three-condition
notification case, independent condition exclusion, absent owner, both read
failure sites and invalid image/event rejection. A missing image or Unicorn
causes an explicit skip, not a pass.

The callback does not check the return code of either status or input-code
reads. With an explicitly zeroed synthetic stack, failure at either site
still emits mask 2. This is a firmware limitation, not a valid Linux failure
policy: an unavailable PMU status must not be treated as confirmed healthy
status. It also does not prove that a live failed read always yields zero;
the uninitialized stack can contain a prior value.

Event consumption/acknowledgement happens elsewhere. This experiment neither
reads physical 0x01..0x09 event registers nor validates read-to-clear timing.
See `p105-pmu-lifecycle.md` for the Wake/Quiesce, boot-fault clear and
watchdog boundaries. Analog charge-voltage limits, PMU thermal-slot ranges,
runtime over-temperature shutdown, battery-presence shutdown and complete
termination/restart requirements remain unproven. They still block enabling
new Linux PMU charger writes.
