# P105 source-only Lightning digital-ID semantic decoder

This unregistered C component decodes data in RAM. It performs **no I2C, USB,
GPIO, PMU, PHY, clock or device operation**. No driver, device-tree node, worker,
adapter allowlist, authentication implementation or active host profile is
provided. A valid record describes native parser validity; it grants no role,
current, voltage or power permission.

## Source and scope

The official P105 iOS 9.3.5 **13G36** kernelcache is the pinned research source:
SHA-256 `8eae16f060a71d4f3790385951588f8198d0f21cdb74146f9750941a3d764125`.
The verified 13G36 ADT is
`e8277fd827ba250de3f46567cb1c910d4e1c80633d666510a7780e00f4ad4b59`.
Its P105 Tristar node explicitly contains `tristar,ths7383`; this does not
identify an attached accessory. Build firmware remains the separate 12H321
image; no boot handoff or live adapter was inspected.

The named AppleTHS7383 constructor PIC pair `0x809ee15c/0x809ee160` resolves
the literal `AppleTHS7383`. The actual constructor vtable PIC pair
`0x809ee18c/0x809ee18e`, plus eight, establishes vptr `0x809f12cc`.
Its `+0x380` slot points to `0x809ee42d`:

1. Read register `0x39`, length seven, through the retained provider's `+0x410`.
2. Only after success, read register `0x0c`, length one, through that provider.
3. Require the status byte's bit seven and the response's seventh byte `0x75`.
4. Copy response bytes five through zero into the six-byte digital-ID output.

The provider and all registers in this description are **original firmware
semantics**, not a live Linux transport implementation or proposed register
writes. No CRC is checked in this particular THS method; do not import the
different CBTL1610 response/CRC format.

Native failure returns before copying and preserves the caller's old output.
The C frame decoder deliberately clears its six-byte output on every failure.
Provider transfer errors must be handled by a future transport owner before
calling this pure decoder; the decoder cannot establish successful transfer
or freshness from a byte buffer alone.

## Actual pair and role-request producer

Dock9Pin `handleAccessoryDetect` at `0x808033b8` invokes its `+0x344` slot at
`0x8080341e`. For the actual BuiltIn vptr `0x809f09b4`, that slot is the
inherited `0x807ff551`: it copies exactly six bytes from the IOAccessoryID
OSData property. This is a separate cached property stage. The asynchronous
Tristar-to-property producer is not executed by this component or fixture.

The source then executes the six-byte data-pair decoder `0x80802f24` at
`0x808034c8`, followed by the low USBd request block
`0x80803576..0x808035ec`. These are distinct operations:

| Synthetic six-byte ID | Native pair selectors | Native parser valid | Low USBd request, with explicit baseline context |
|---|---|---|---|
| `08 00 00 00 00 00` | `usb0`, none | yes | 5 |
| `10 00 00 00 00 00` | `usb0`, none | yes | 2 |
| `18 00 00 00 00 00` | `usb0`, none | yes | 1 |

These values are synthetic test inputs, **not Camera Adapter product IDs**.
Identical pair selection does not imply identical controller direction.
The values are request flags, not an unconditional CableType result; attach,
stack/source status, manager feature policy, authentication and final
publication remain separate conditions. Low flag `0x20` is not inferred from
these examples or equated with charging-handshake bit 19/20.

The full native manager also calls pair-routing slot `+0x3d4` at
`0x8080377a` and applies additional checks and feature masks before dispatch
to USBd. This decoder deliberately does not call that slot or bypass those
checks. Non-USB selectors (`urt0/1/2`, `mbus`, `jtag`, `usbC`) are descriptive
records only.

## Source context and conservative error behavior

| C context input | Audited original producer/consumer |
|---|---|
| `debugger_known/allowed` | Decoder calls stub `0x8080552c`; IAT `0x80808030` resolves `0x80388eed`, exactly named `_PE_i_can_has_debugger` in the pinned kernel nlist. |
| `source_ignore_vbus_known/present` | Actual BuiltIn `+0x3e8` is `0x809ecf1c`; its PIC pair `0x809ecf26/0x809ecf2c` resolves dock property `ignore-vbus` at `0x809efd0e`. It returns property presence as a boolean. |
| `source_md_root_known/source_md_root` | Dock9Pin init `0x808027e0` parses `rd`, then `rootdev`. The original `0x80802832..0x8080284c` checks bytes zero/one equal `m`/`d` and byte three equal zero, then stores one at `+0x7ed`. It does not test byte two. This is source boot-context metadata, not authentication success. |

All context starts unknown/false. A branch requiring unknown context returns
`-EACCES` and clears the record. Supplying context values permits semantic
decoding only; it does not grant authority to ignore real VBUS, enable a
debug mode, publish a controller or power an accessory.

The C decoder returns an empty invalid record on malformed input, invalid
native combinations, unknown required context or any other error. Native
invalid combinations may leave nonzero selectors or request flags; those
are intentionally not retained by the C error path. This prevents a future
consumer from accidentally publishing stale/partially decoded state. A
future transport/role owner still needs device epochs, disconnect invalidation,
serialized ownership and complete transfer/error handling.

## Reproduction and validation

Optional original-instruction script `emulate-id-route13.py` verifies the
13G36 SHA and the constructor-derived THS slot, then executes the original
THS method, pair decoder and bounded flag block in Unicorn RAM. I2C provider,
logging/class-name diagnostics, debugger result, `ignore-vbus` result and
`+0x7ed` context are explicit fakes. No binary firmware is distributed.
Each function is bounded to 3000 instructions and its exact stop address.

The normalized text oracle contains 4360 semantic cases: all 256 first-byte
values across four byte-two masks and four byte-three classes (4096 cases),
256 debugger-enabled cases, and eight independent context controls. Five
additional original THS method cases cover success, absent ready status,
wrong marker, first-read error and second-read error. The immutable oracle
SHA is `069bcc457b479eba20ae1ce45ebd4060b9aa67ae159a038ea1405698c0e40aa6`;
the complete original report SHA is
`5662dda9322f12d3e484d58da1b485d0d00ebef6c97773e4552c067f023adf2c`.

`fixture.c` compiles the actual C source under `-Werror` plus ASan/UBSan.
`test-id.py` compares every native-valid selector/flag result and requires
empty error output for every native-invalid case. Independent C checks cover
null pointers, lengths, ready status/marker errors, unknown required context,
stale output clearing and safely overlapping frame/ID output. Root review additionally snapshots an overlapping policy/result input before clearing the output; the actual-C fixture covers that alias. The self-contained Python runner builds only into a temporary directory.
Host and the existing Ubuntu 22 build image passed; a separate real Linux 6.12 ARM-header
object compile passed with zero warnings.

These are software/source tests. There is no observed Camera Adapter identity,
live ID-bus exchange, authentication transcript, PMU rail enable, 5 V supply,
connector routing, host enumeration, charging or USB keyboard/stick result.

## Remaining useful integration

The evidence supports a future read-only THS transport and checked digital-ID
record stage. It does not yet close the cached IOAccessoryID publication,
Camera model/authentication producer, accessory current/voltage owner,
connector isolation or adapter-side 3.3-to-5 V conversion. The separately
audited PMUL5 accessory rail must not be labelled native 5 V VBUS.

The externally powered Apple Lightning to USB 3 Camera Adapter remains a
physical solution candidate documented by Apple for iOS 9.3 or later, not a
Linux authorization or authentication workaround:
https://support.apple.com/en-us/111811

For an initial powered-host target, an actual high-speed device or high-speed
hub with a transaction translator could avoid a direct full-speed OHCI
companion dependency. The Linux EHCI documentation supports that controller
distinction; it does not establish P105 routing, adapter wiring or power:
https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/Documentation/usb/ehci.rst

Reproduce the source oracle with the existing resource-limited offline image:

```sh
python3 emulate-id-route13.py /path/to/verified/kernelcache.13G36.macho --source-root /path/to/cascadia
python3 test-id.py
```

The first command needs the pinned user-provided firmware plus existing
Unicorn and Capstone. The ordinary C regression needs neither firmware nor
an emulator: its oracle consists only of generated synthetic inputs and
normalized source results. No Apple executable bytes are in that JSON.
