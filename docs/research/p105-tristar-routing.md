# P105 THS7383 accessory routing is separate from VBUS supply

Offline analysis of the verified P105 iOS 8.4.1 **12H321** cache, SHA-256
`db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c`.
No hardware connection, runtime write or new host profile. This extends
[USB host evidence](p105-usb-host.md) and [EHCI controller power requests](p105-usb-ehci.md).

## Actual dispatch and accepted routing codes

BuiltInUSBDevice's verified instance vtable `0x80888998`, slot `+0x3a4`,
points to `0x80884ecc`. That wrapper invokes its retained Tristar controller
at `object+0xf4`, slot `+0x394`. The actual THS7383 instance vtable
`0x80889298` resolves that slot to **`0x80886f6c`**.

This function accepts two internal FourCC routing selectors, encodes them
into a byte, and writes controller register **2** through its I2C provider
slot `+0x3d0` at `0x80887016..0x80887024`. The arguments are selectors,
not voltage or current numbers. Symbolic names do not establish physical
pin wiring. Other Tristar variants can have different dispatch/encoding.

The publishable offline Unicorn harness
[`scripts/emulate-tristar-routing.py`](../../scripts/emulate-tristar-routing.py)
executes these actual instructions with fake provider/RAM and captures the
one-byte synthetic I2C requests. It validates the firmware hash, actual
instance vtable/dispatch and imported orientation getter. Its only external
semantic fixture is the orientation getter described below.
[`tests/test_tristar_routing.py`](../../tests/test_tristar_routing.py) asserts
30 selector/orientation combinations plus rejection of unsupported inputs.
The firmware is a separately supplied, verified local input; no firmware bytes
are distributed with the harness. Reproduce inside the existing build image
with `python3 tests/test_tristar_routing.py`, or run
`python3 scripts/emulate-tristar-routing.py build/firmware/kernelcache.12H321.macho`.
An ignored local report is
`build/usb-host-research/ths-accessory-routing-writes-v3.json`.

| First selector | Second selector | Synthetic orientation getter | Actual result |
|---|---|---|---|
| None | None | 0, 1 or 2 | Register 2 ← `00` |
| `idbs` | None | 0, 1 or 2 | Register 2 ← `00` |
| `apwr` | None | 0, 1 or 2 | Register 2 ← `03` |
| `urt1` | None | 1 | Register 2 ← `01` |
| `jtag` | None | 1 | Register 2 ← `02` |
| `urt1` or `jtag` | None | 0 or 2 | Return `0xe3ff82e6`, no write |
| `urt0`, `apwq`, or `usb0` | None | 0, 1 or 2 | Return `0xe00002e6`, no write |
| None | `idbs` | 0, 1 or 2 | Register 2 ← `00` |
| `idbs` | `urt0` | 0, 1 or 2 | Return `0xe00002e6`, no write |

These are fixtures, not observed camera-adapter IDs. A preliminary manual
literal-to-branch association was corrected by running the actual firmware;
the table above is the validated result. No electrical meaning or safety of
programming these selectors on hardware follows from it.

## The extra predicate is orientation, not power permission

Import stub `0x80887acc` resolves pointer `0x80888090` to
IOAccessoryManager method **`0x80713944`**. That method fetches a property
through symbol global `0x80724134`, dynamically verifies an OSNumber-like
object, then returns its numeric value.

The symbol initializer at `0x80715754` proves the property's identity:
PIC pair `0x807157a2/0x807157a6` resolves string `0x8071da2a`,
**`IOAccessoryOrientation`**; creation call `0x807157ac` returns the symbol;
the following iteration stores it into global `0x80724134` at
`0x807157b8` (global PIC pair `0x807157b0/0x807157b4`). The *next* creation
string, `IOAccessoryPowerHandshake`, belongs to the following global and
must not be confused with the preceding store.

Thus the THS7383 routing setter's equality check against 1 is an orientation
condition, not an authentication or VBUS permission gate. The producer is
now traced: BuiltInUSBDevice's actual instance slot `+0x3c0` points to
`0x80884e9c`, which calls its retained THS7383's slot `+0x388`, resolving to
`0x80886cb0`. That getter reads I2C status register `0x0c`, requires bit `0x80`,
and returns `((status >> 6) & 1) + 1`, or 0 on error/not-ready.

IOAccessoryManager's ID-event handler `0x807139b8` invokes this slot at
`0x807139f6..0x807139fc`. It compares the returned orientation against the
existing property and, when changed, passes the same value to setter
`0x8071338c` at `0x80713a66`. The setter creates a 32-bit OSNumber or removes
the property for zero. There is no 1/2-to-0/1 normalization in this observed
path. Thus valid status orientations are 1 and 2, and the routing setter
accepts `urt1`/`jtag` only for orientation 1. The offline experiment has
30 fixtures, including all three synthetic getter values 0/1/2. These still
do not identify physical Lightning pin orientation or an adapter ID.

## Misleading power-adjacent names do not prove a boost circuit

BuiltInUSBDevice startup at `0x808853dc` stores the result of
`function-brick_id_voltage` lookup in **`object+0x128`**. The exact property
string is `0x80887d4d`, resolved by PIC pair `0x808854a6/0x808854ae`; the
store is at `0x808854b6`. Therefore methods `0x80885a60/0x80885a7c`, which
access that field, must not be labeled as `function-acc_pwr` or an external
5 V source switch.

Similarly `downstream-vbus` getter `0x80885a08` and `ignore-vbus` getter
`0x80885a40` perform property lookups. Neither inspected getter contains a
boost-controller implementation. Accessory LDO5, incoming charger permission,
EHCI PORT_POWER and external VBUS voltage remain separate evidence paths.
A real camera-adapter identification fixture, its complete power-provider
path, current protection and physical routing are still required before any
active implementation or hardware experiment.
