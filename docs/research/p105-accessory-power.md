# P105 accessory rail callbacks and ID-selected voltage

Offline evidence from the official P105 iOS 8.4.1 **12H321** cache, SHA-256
`db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c`.
No device access, runtime power writes, host profile or firmware bytes are
included. This extends [Tristar routing](p105-tristar-routing.md) and the
[ADT / PMU rail evidence](p105-usb-host.md).

## Actual provider dispatch

BuiltInUSBDevice startup `0x808853dc` resolves the following properties from
its retained dock provider. PIC literal pairs identify the strings; these
are distinct from incoming USB charging and voltage sensing.

| Property | String PIC load/add | Store | Retained field | Actual P105 ADT provider |
|---|---|---|---|---|
| `function-acc_pwr` | `0x8088545a/0x8088545c` | `0x80885462` | `+0x118` | PMU `pmuL`, argument 5 |
| `function-acc_sw_en` | `0x8088546a/0x80885470` | `0x80885478` | `+0x11c` | SoC GPIO `GPIO`, arguments `0x1004`, `0x101` |
| `function-acc_sleep_pwr` | `0x80885476/0x80885480` | `0x80885488` | `+0x120` | PMU `pmuL`, argument `0x216` |

The actual instance vtable `0x80888998` is established from its constructor
PIC pair `0x80885368/0x8088536a` plus 8. Slot `+0x368` points to
**`0x808857c4`**. That method accepts three independent values and invokes
the three retained ARMFunctions in the order above, through each provider's
slot `+0x3c`. The first is required; the latter two are optional.

It writes the enable value into `object+0x138` **before** the first callback.
Any nonzero callback result exits immediately, with no rollback of preceding
calls or of that cached value. This is observed Apple firmware behavior,
not a recommended Linux error policy, and a cached value does not prove the
physical rail's state.

IOAccessoryManager `0x807122e8` calls slot `+0x368` at `0x80712348` for its
internal states 2/3/4 with `(1, state == 4, object+0x75)`. State 1 calls it at
`0x80712396` with `(0, 0, 0)`. These states are internal accessory policy
states; they must not be equated with USB controller power states or charger
source-description indices. Their larger authentication and routing policy
is not fully modeled by the harness below.

## The voltage producer is a separate ID-derived request

Instance slot `+0x39c` points to **`0x80885828`**. It invokes only the
`function-acc_pwr` ARMFunction, supplying the cached enable byte `+0x138` as
its first argument and its new input as a second argument. Thus a voltage
request and a power-enable request use the same provider with different
argument shapes; there is no fixed 5 V request at this bridge.

The actual producer is the ID-policy block `0x807150e6..0x80715194`.
It extracts the upper nibble of the ID response's byte at stack offset
`+0x25`, compares it against cached class `object+0xb1`, and acts only when
the class changes:

| ID byte | Requested second argument | Cached class |
|---|---|---|
| `0x00..0x3f` | No voltage callback in this block | `byte >> 4` |
| `0x40..0xbf` | `2100 + 100 * (byte >> 4)`, i.e. 2500..3200 | `byte >> 4` |
| `0xc0..0xff` | 3300 | `byte >> 4` |
| Upper nibble equals prior class | No callback or replan in this block | Preserved |

A changed class is cached and leads to `0x807122e8` power-policy reevaluation.
The larger handler's earlier ID parsing, orientation and authentication
conditions are not bypassed by any supplied Linux code: this harness simply
executes the audited block using synthetic inputs.

The numbers are firmware voltage units, not measured output. The P105 ADT
resolves `acc_pwr` to D1946 LDO5; the independently audited D1946 table caps
that rail at 3300 firmware units. This fits an accessory rail. It does **not**
identify a 5 V boost circuit, establish the external adapter's conversion,
prove that `acc_sw_en` gates VBUS, decode its GPIO argument to a physical pin,
or establish current protection.

## Reproduce without hardware

[`scripts/emulate-accessory-power.py`](../../scripts/emulate-accessory-power.py)
checks the pinned hash, constructor-derived instance vtable and the actual
`+0x368`/`+0x39c` slots. Unicorn executes the original callback methods and
ID-derived arithmetic in ordinary RAM. The ARMFunction providers and the
subsequent power-policy replan are fake boundaries; real PMU/GPIO operations
and the surrounding policy are excluded.

[`tests/test_accessory_power.py`](../../tests/test_accessory_power.py) asserts
callback order, independent values, all three short-circuit failures, absent
optional providers, ID boundary values, unchanged class, preserved cached
enable, and rejection of invalid inputs or unverified firmware. Six tests
passed in the existing resource-limited, network-disabled build image.

```bash
python3 tests/test_accessory_power.py
python3 scripts/emulate-accessory-power.py build/firmware/kernelcache.12H321.macho
```

The verified firmware is a local user-supplied input. The harness distributes
no Apple binary bytes and has no device or network backend. An ignored local
JSON report is `build/usb-host-research/accessory-power-emulation-v1.json`.

## Remaining VBUS blockers

EHCI PORT_POWER MMIO, incoming charger permission, accessory LDO5,
SoC GPIO `acc_sw_en`, Tristar routing and an external 5 V bus are separate
pieces of evidence. The GPIO dispatch implementation, full accessory power
state transitions, boost/provider schematic, adapter pin mapping and current
protection still need proof before any active host power implementation.
