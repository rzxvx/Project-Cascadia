# P105 VBUS presence sensing: no established 5 V provider

## Scope and result

The verified P105 **12H321** firmware contains a VBUS-presence callback with two
measurement paths: an optional AppleARMFunction and a THS7383 status fallback.
Neither audited path enables a supply. The fallback requires both low bits of
I2C status register `0x0c`, and returns false on a read error. Its public offline
harness executes the actual wrapper and fallback instructions, not a recreated
Python predicate.

This evidence does not identify a boost converter, a USB host power switch,
VBUS pin wiring or a safe host-power sequence. LDO5 accessory power, PMU ADC
channels, this presence predicate and EHCI PORT_POWER remain separate paths.
No runtime code or hardware behavior was changed by this research group.

## Pinned provenance and actual dispatch

Kernelcache: `build/firmware/kernelcache.12H321.macho`, SHA256
`db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c`.
The input was locally decrypted from the documented official P105 8.4.1 IPSW;
fixed-address execution refuses any other hash. Firmware bytes are not included
in the published harness.

| Link | Exact firmware evidence | Meaning established |
| --- | --- | --- |
| BuiltInUSBDevice instance | Constructor PIC pair `0x80885368/0x8088536a` plus instance `+8` → vtable `0x80888998` | Actual instance dispatch, not the metaclass |
| Presence callback | Instance slot `+0x3c4` → `0x808859c0` | The wrapper audited here |
| Optional function | Startup stores `function-vbus_voltage` in object `+0x130` | AppleARMFunction measurement interface |
| Function success | Call slot `+0x3c` at `0x808859dc`; output signed compare at `0x808859f6` | True if signed 32-bit raw reply is at least `0x40000` |
| Missing/function-error path | PIC pair `0x808859e2/0x808859e4` → global `0x808880d8`, containing base vtable `0x80888140` | Explicit base fallback dispatch |
| Base fallback | Base slot `+0x3cc` → `0x80884ea8` | Calls retained controller at object `+0xf4`, slot `+0x38c`, with argument 1 |
| THS7383 instance | Constructor PIC pair `0x808869fa/0x808869fc` plus `+8` → vtable `0x80889298` | Actual early-Tristar controller |
| THS predicate | THS instance slot `+0x38c` → `0x80886ce4` | Reads one byte at register `0x0c` through I2C-provider slot `+0x3d4` |

For argument 1, the THS function returns true only when `(status & 3) == 3`.
For argument 0 it instead tests whether either low bit is present; other
arguments return false. The BuiltIn fallback passes **1**, and the published
wrapper harness verifies that actual argument. A successful I2C reply with
status `0x03` is true even without bit `0x80`. Bit `0x80` is used by the
separate orientation/ID getters; it must not be added to this predicate from
those other functions.

The calibrated units of the optional function's raw value are not established
here. `0x40000` is reported as a raw threshold, not assigned a voltage in volts.
Successful raw replies `0x80000000` and `0xffffffff` are negative in the actual
signed comparison and return false. A failed function call invokes the THS
fallback rather than using the raw reply.

## P105 ADT and measured quantities

The verified complete P105 raw ADT, SHA256
`5c346586116de1bb32606acb27cad3158bcaa8a1318a10ee9631de09884cfd72`,
does not contain the literal property names `function-vbus_voltage`,
`downstream-vbus` or `ignore-vbus`. Thus no ADT-backed voltage provider is
declared through those names in this specific tree. This does not exclude
dynamic publication or another firmware/device-tree revision. The offline
experiment checks both optional-function presence and absence synthetically;
it does not claim a live instance was inspected.

The D1946 metadata identifies ADC channel 15 as `vbus`/`Vu0` and channel 27 as
`vbus_b`/`Vu1`. Those are independent measurement identities. The inspected
BuiltIn wrapper does not name either PMU ADC channel in its fallback; it reads
THS status instead. There is no proven link from either ADC name to a particular
external connector pin, outgoing bus, boost device or switch-enable callback.
See [the separate D1946 ADC evidence](p105-d1946-adc.md).

## An observed consumer and a separate Tristar write

Inside IOAccessoryManager's previously audited handshake routine
`0x80714028`, a branch for cached byte `object+0xd0 == 0x71` invokes actual slot
`+0x3c4` at `0x807142a8..0x807142ae`. If the first result is 1, it invokes slot
`+0x3a8` with argument 1, senses again at `0x807142c4..0x807142ca`, then invokes
slot `+0x3a8` with argument 0. The second result selects further branches.
Neither this byte value nor the sequence identifies a camera adapter or an
outgoing host source by itself.

BuiltIn slot `+0x3a8` resolves to wrapper `0x80884ed8`, forwarding to retained
THS slot `+0x398` → `0x8088704c`. That function writes the argument's low byte
to controller register **5** through the I2C provider. On successful argument
1 it calls import `0x808878ec` with `0x12` (18). That stub's MOVW/MOVT/ADD-PC
sequence resolves pointer `0x80888018` → `0x802e4659`, matching the actual
`_IOSleep` symbol at `0x802e4658` plus its Thumb bit. The API takes milliseconds,
so this is an 18 ms delay request; the public XNU implementation supplies the
unit cross-check, not a Tristar register definition.
[Primary XNU source, tag xnu-2782.40.9](https://raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-2782.40.9/iokit/Kernel/IOLib.cpp).
A neighboring method at
`0x80887080` writes byte 2 to the same register.

These writes are original firmware evidence, not implemented operations in the
public presence harness. Register 5's electrical purpose remains unproven; the
call must not be named a VBUS boost-enable solely because it occurs around a
VBUS-presence check. The incoming charging authorization path involving
`USBd` flags is documented separately in
[charging research](p105-charging.md).

## Offline regression

[`scripts/emulate-tristar-vbus-sense.py`](../../scripts/emulate-tristar-vbus-sense.py)
validates the cache hash, actual constructor-derived vtables, callback slots
and fallback global before mapping the original kext into Unicorn. BuiltIn,
THS and provider objects are synthetic RAM. Only the fake I2C read and optional
voltage-function reply are intercepted. No I2C write-provider slot exists;
unexpected accesses fail instead of reaching hardware. Output buffers must
remain inside the synthetic stack. Execution has a 10,000-instruction bound and
must return.

[`tests/test_tristar_vbus_sense.py`](../../tests/test_tristar_vbus_sense.py)
contains six unittest methods with all 256 status bytes, independent
representative fixtures, read failures, signed-threshold boundaries,
optional-function errors and refused inputs. Local verification ran all six
without skips. On systems without local verified firmware or Unicorn, these
firmware tests explicitly skip; a skip is not validation.

```sh
python3 tests/test_tristar_vbus_sense.py
python3 scripts/emulate-tristar-vbus-sense.py build/firmware/kernelcache.12H321.macho
```

No binary bytes, personal data, device-access credentials or external runtime
scripts are distributed. The harness reports `hardware_access: false` and
`power_source_verified: false`.

## Remaining 5 V source questions

The actual accessory voltage path dispatches `function-acc_pwr` to PMU LDO5,
whose audited range stops at 3300 in firmware units. The nearby SoC GPIO
`acc_sw_en` and accessory routing selector `apwr` do not establish a converter
or a connector supply. See [accessory power](p105-accessory-power.md) and
[GPIO 132 provider evidence](p105-accessory-gpio.md).

A runtime host-power implementation still requires a proven board/adapter
topology, a true 5 V source or adapter-side converter, its enable provider,
current limit, detach/fault handling and isolation against reverse feeding.
Sensing a status byte or setting a controller port-power request supplies none
of that missing electrical evidence. No regulated 5 V output or safe powered
hub/adapter workaround is claimed by these offline results.
