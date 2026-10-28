> Isolated source experiment. Nothing selects this code in the normal kernel. Hardware behavior has not been tested. Local staging paths below identify the original evidence, not build hooks.

# Private EP0 role transport candidate — 13G36 evidence

This is review staging. It is not installed in Cascadia, built into either
kernel, registered as a USB function or connected to hardware. The shared
checkout remains unchanged. The component contains no USB transfer,
GPIO/PMU/MMIO access, PHY operations or power-provider implementation.

## Scope correction from the actual userspace producer

The independently extracted official 13G36 `iaptransportd` imports
`_IOAccessoryPortSetUSBRoleSwitchMask`. Its actual strings identify
`CIapPortIOKit::SetRoleSwapMaskForUSBPort(UInt16, UInt16)`, a "Stark accessory
role switch", and disabling the mask when CarPlay is absent. Root traced
callers using a CarPlay boolean as the first mask and zero as the second.
This is positive evidence for a CarPlay/Stark role-swap consumer. It does
not establish that a Camera Adapter uses EP0 requests51/53.

Consequently this transport component is an independently useful USB-role
protocol candidate, not a Camera Adapter handshake implementation. The
prioritized Camera Adapter path may instead use direct ID-bus accessory
classification and manager role detection; that chain still needs proof.
HID descriptor presence also does not identify a Camera Adapter consumer.

## Proven transport boundary

Official iPad2,5 13G36 kernelcache SHA256:
`8eae16f060a71d4f3790385951588f8198d0f21cdb74146f9750941a3d764125`.
Addresses below are derived anew from this cache, not reused from 12H321.

- `IOAccessoryPortUSB` constructor at `0x807fca2c` installs actual instance
  vptr `0x8080a230`. Slot `+0x168` calls start `0x807fcb38`.
- Start constructs callback pointer `0x807fcd2d` at `0x807fcc88` and registers
  it through import `0x808056ec`, pointer `0x808080a0`, actual
  `IOUSBDeviceFamily` method `0x807c77b4`. That method stores owner/callback
  at `+0x78/+0x7c`; dispatch `0x807c77c0` invokes it.
- Handler `0x807fcd2c` accepts exactly `c0/53`, wValue=0, wIndex=0,
  wLength=4 and emits two LE16 values: object `+0x70`, then `+0x6e`.
- `40/51` requires wLength=0. wValue must be a subset of `+0x70`, wIndex a
  subset of `+0x6e`. Invalid mask bits return firmware IOReturn
  `0xe00002bc`; wrong sizes are left unhandled. No camera-adapter ID has
  been identified.
- Success obtains its manager through `+0x340`; calls gated API
  `0x807f7af4` with1 (callback `0x807f7b30`, stores override `+0x84=1`),
  then gated API `0x807f7000` with3 (callback `0x807f703c`). Actual mode
  table `0x80807fb0` is `[2,3,0x25,5,1,4]`; input3 therefore requests
  USBd control5 through `0x807f70fc`.
- Success sends IOService message `0xe3ff8022`, exact original setup8B
  via `+0x22c`. Failure at the first gate prevents the second call/message.
- Actual AppleTriStarBuiltIn vptr `0x809f09b4` slot `+0x384` targets
  `0x809ecd2c`. That method passes the control word through its retained
  `function-usb_dock_control` AppleARMFunction at object `+0x850`.
  The mock experiment deliberately stops before this hardware transition.

## Who permits the request

This is not a kernelcache-only accessory identity policy. The actual
`IOAccessoryPortUserClient` dispatch table `0x8080dcc8` index2 has handler
`0x807fc581`, scalar input count2, no structure input/output. The callback
reads the low16 bits of two 64-bit scalars and invokes port slot `+0x35c`,
actual `setUSBRoleSwitchMask` `0x807fd5e4`, storing `+0x70/+0x6e`.
The userspace producer, its authentication prerequisites and A1619 identity
have not been resolved. Zero masks must not be interpreted as permission to
perform a physical switch with a zero-valued request.

## Candidate C semantics

`cascadia_accessory_role_setup()` returns4 for the exact IN response,
0 for accepted OUT, negative Linux errno for failure, and `-EOPNOTSUPP`
for requests delegated to the enclosing gadget. Firmware IOReturn values
are intentionally translated into Linux errno semantics, not copied as
32-bit negative integers.

The masks and callbacks are supplied by an explicit policy snapshot;
initialization to zero reports zero capability masks and denies all role
changes, including a zero-valued OUT request. `allow_switch`, an
`authorize()` callback and a `request_switch()` callback are all necessary.
No callback exists by default. Positive unexpected callback return values
fail closed. This is a deliberate extra Linux policy guard, not behavior
claimed from original firmware.

The caller must snapshot policy and its connection epoch under its own lock.
Both callbacks receive that epoch; the backend must revalidate it before
queueing and again in its worker before any role effect. No wire epoch is
invented, and the transport core does not own the attach lifetime. The caller
must pass exactly the eight
setup bytes, copy the four IN response bytes to its EP0 request, and map
`-EOPNOTSUPP` to delegation appropriately. The switch callback must validate
and copy the setup; it may not retain the input pointer. A real gadget setup
callback may run in atomic context. It must not call regulators, PHYs or
PMU/I2C transactions synchronously here. Scheduling and acknowledgement of
the final EP0 status stage versus disconnect/role transition still needs a
real adapter transcript and a concrete gadget completion/workqueue design.
The source contains no fabricated asynchronous success mechanism.

The exact component also compiles as an ARMv7 object against the generated
headers and actual Linux6.12 source from the current normal build, with the
source mounted read-only. No kernel tree or config was modified.

Private Kconfig/Makefile fragments are default-n, no DT changes or driver
registration. They show intended isolation, not an integrated feature.

## Validation

- Actual C component compiled with `-std=c11 -Wall -Wextra -Werror`.
- 24 independent bounded C cases: LE ordering, exact lengths, masks,
  errors, missing callbacks, stale attach epochs before and after admission,
  default policy, malformed buffers and wrong
  recipient. Only fake policy callbacks exist.
- Seven C cases compared with independently executed actual13G36 handler
  instruction paths (Unicorn, maximum20000 instructions, fake RAM).
  Oracle intercepts debug/logging, manager getter, policy admission,
  command-gate provider, response descriptor allocation, message sink and
  memcpy. Actual mask branches, gated wrappers, mode table and setup copy
  execute; the hardware transition function is intercepted.
- `compare-oracle.py` checks handled/delegated classification, exact
  four reply bytes, request copies, and prevention of switching side effects.
  Linux errno is compared semantically rather than equated with IOReturn.

These tests establish the transport component. They do not establish a
Lightning adapter, authentication, electrical routing, voltage, current,
real USB enumeration or successful Linux host operation.

## Prioritized functional hardware path

Apple documents that its Lightning-to-USB3 Camera Adapter's additional
Lightning power input supplies both the iOS device and connected USB device,
and requires iOS9.3 or later. Its mini1 accessory compatibility filter lists
that adapter. This supplies a manufacturer-supported external-power
architecture, while the current build firmware remains12H321/8.4.1.

Sources verified2026-10-12:

- [Apple power and hub support](https://support.apple.com/en-us/111811)
- [Apple mini1 compatibility filter](https://www.apple.com/shop/ipad/accessories/chargers-adapters/ipad-mini-1st-generation-usb-3)

For Linux, the target route should follow the evidenced Apple external
EHCIport1/OHCI0 companion path, separate from HSICWLAN and DWC2PHY0.
Needed implementation boundaries are:

1. Accessory ID/orientation capture and verified adapter/power policy.
2. Accessory transport appropriate to the proved adapter. The EP0 candidate
   applies only if that adapter is actually observed/proved to use this
   role-swap protocol; CarPlay evidence is not a Camera Adapter substitute.
3. Connector role owner arbitrating DWC2 peripheral disconnect and external
   EHCI/OHCI publication/port ownership without stopping HSICWLAN.
4. Shared PHY clock, reset and client membership accounting using actual
   firmware ownership evidence; a generic PHY_HOST.set_mode at WiFi's probe
   is insufficient to represent connector transitions.
5. Board-specific connector mux and externally supplied VBUS sense/protection;
   neither PMULDO5 nor accessory3.3V is silently treated as USB5V.
6. A verified completion/rollback path and explicit hardware test permission.

Adding the exact firmware vendor protocol to Linux without these policy and
owner boundaries would not be a completed host solution. External power
avoids requiring an unproven native iPad5V boost; it does not bypass accessory
recognition or prove backfeed protection on a particular hub/adapter.
