> Isolated source experiment. Nothing selects this code in the normal kernel. Hardware behavior has not been tested. Local staging paths below identify the original evidence, not build hooks.

# Private checked P105 removable-port client lifetime

This component is an isolated source review candidate, not registered or activated. It has no PHY, USB, PMU, GPIO, register or VBUS backend. It does not
change a Linux generic-PHY count. The default policy denies operations; no
callback table exists by default. ARMv7 object compilation against actual
Linux6.12 generated headers succeeds with -Wall -Wextra -Werror. The Linux tree
was mounted read-only and no full kernel/config change or build occurred.

## Actual caller, not a generic forced-DWC2 assumption

All following addresses use exact official P105 12H321 kernelcache SHA256
`db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c`.

The actual AppleUSBEHCIRemovablePort constructor PIC pair
`0x80c90a8c/0x80c90a8e` +8 installs vptr `0x80c92608`. Start
`0x80c90b30` reads `clock-mask` via PIC `0x80c90b58/0x80c90b5e` into+0x110,
and `phy-id` via `0x80c90bfa/0x80c90c00` with fallback0. At
`0x80c90c58..0x80c90c5e` it calls factory with kind0, phy-id and the raw clock
mask. Stub `0x80c91acc` resolves pointer `0x80c92060`→`0x808622ad`.
This is an actual kind0 host client caller. P105 external EHCIport1 declares
clock mask0x30002; the PHYfactory filter0x60070|0x10000 reduces it to0x30000.
The parent-controller gate bit1 is held separately by the parent-nub clock
membership. It must not be reconstructed from the PHYclient mask alone.

Actual client factory executes constructor/init `0x80cac7d0`→`0x808622e4`,
stores membership mask4 for kind0, and dispatches state via actual client vptr
`0x80866568` +0x3c→`0x8086240d`.

Actual removable-port callback `0x80c90fb0`:

1. Guard: whatDevice must equal self.
2. Parent USBnub +0x350(self, rawclockmask), even for target powerstate0/1.
3. For target powerstate>=2, PHYclient +0x3c(5).
4. Dispatch inherited +0x31c.

Actual complementary callback `0x80c91008`:

1. Dispatch inherited +0x320 first.
2. Guard: whatDevice must equal self.
3. Target0: PHYclient +0x3c(10), then parent clock membership0.
4. Target1: PHYclient +0x3c(25), then parent rawclockmask&0xfff9ff8f.
   For EHCIport1 this is0x10002.
5. Other targets: no client/clock release here.

The imported inherited dispatch table at `0x80c920c8`→`0x8084e3f0`
resolves +0x31c→`0x8084846d`, +0x320→`0x80848509`. These actual instruction
pointers are intercepted by the fixture; their full controller implementation
is not claimed to execute.

The actual caller does not check parent-clock/PHYclient errors. Its Did
callback performs releases even after an inherited error, returning that
inherited error. Those error paths are evidence to improve, not behavior to
copy blindly into the new kernel candidate.

## PHYclient state, captured with bounded fake providers

- State5: arbitrator+0x344(filteredclockmask), PHYselector+0x350(mask4),
  PHY+0x358(kind0) when not already added. Repeated5 produces no callbacks.
- State25: host membership remains mask4; clock retention moves to a separate
  base-client collection through actual static base+0x34c, while the ordinary
  client clock request becomes0. This is not an off state.
- State10: removes the reset/client membership, selects mask0 and clears clocks.
  Clock collection transfer differs when coming from25 rather than5.

The fixture executes actual factory/client instructions, but supplies fake
locks, allocator, OSSet and provider callbacks. It does not execute regulator,
clockgate, mode-tuning or MMIO operations. The synthetic OHCIclockmask0x30006
comparison has the same filter result; the exact OHCI removable-port factory
caller has not yet been executed by this fixture.

## Checked two-phase interface

The C component deliberately covers only this **owned port-client boundary**.
Its storage must be freshly zero-initialized before the first init call.
`init()` returns -EALREADY for an initialized object, including an owned/pending
object, and preserves all metadata; resetting its storage externally would
violate the owner lifetime contract. This is not a reset API.

Its callbacks are idempotent absolute membership updates for one owned client.
They may not be implemented as blind generic phy_power_on/off reference
increments/puts. Parent-clock masks are supplied from verified ADT data;
they are not regulator settings or VBUS voltage/current values.

- `prepare(target, epoch)` obtains the raw parent-clock membership and, for
  ACTIVE, requests PHYclient state5. It leaves a pending transition.
- The caller performs its actual controlled port transition and reports the
  **completed result**, not merely successful workqueue scheduling, to
  `complete(epoch, controller_error)`.
- On successful completion, STANDBY requests client25 then the standby parent
  mask; OFF requests client10 then releases the parent mask. ACTIVE retains
  prepared memberships.
- A provider error leaves its disposition unknown and latches FAULT. A
  controller error retains acquired memberships and latches FAULT. Subsequent
  prepare requests refuse success; no automatic compensating power/role write
  is issued.
- Explicit `cleanup(epoch, controller_quiesced=true)` is permitted only after
  the caller has actually quiesced the owned external port. It clears the
  PHYmembership before releasing its parent clock, stops on error, and clears
  FAULT only when all updates succeed. It never resets the shared block.
- The caller must serialize the object under its connector/shared-owner mutex.
  The component is not an IRQ setup callback. Backend callbacks may sleep only
  in the actual workqueue/owner context.
- Epochs identify the specific attach/client lifetime. The owner validates the
  epoch before and after each backend operation, between phases and immediately
  before final success/cleanup commits. Each backend must also revalidate it
  before affecting hardware. A post-I/O stale epoch marks the corresponding
  provider disposition unknown and retains FAULT/pending metadata. An old
  epoch fails without pretending the held resources have gone away. Resolving
  ownership after physical detach/reprobe remains an integration obligation;
  this prototype does not silently substitute a new device's epoch.

There is no complete controller implementation behind these callbacks. The
owner must still implement the actual EHCIport1 quiesce/enable path, keep
HSICWLAN clients alive, aggregate shared PHY ownership, control the proved mux,
and establish adapter identity/external power/protection. It may not treat a
successful PHY callback as successful VBUS or USB enumeration. The independent
CarPlay EP0candidate is not evidence for a Camera Adapter handshake.

## Validation and limits

`emulate-port-owner12.py` executes the two actual port callbacks with fake
parent-clock, client-state and inherited callbacks (maximum20000 instructions).
Twelve success/foreign-device/error fixture cases pass, including the actual
firmware's ignored provider errors and release-after-inherited-error behavior.
`emulate-client-host12.py` captures factory/client state transitions for the
actual EHCImask and separately labeled synthetic comparison masks.

`test-client.py` executes the actual compiled C source through ctypes. Normal
ACTIVE/STANDBY/OFF callback order and masks are compared with the independent
original-instruction oracle. Error/lifetime cases cover each prepare stage,
controller failure, cleanup failure, standby clock failure, stale epochs,
pending transitions, reinitialization rejection, missing policy/providers,
positive callback errors and epoch changes after successful fake I/O/before
final commit. These bounded checks do not prevent a physical detach after a
successful return; the actual owner still needs its event/lifetime locking.
It verifies that a failed inherited completion does **not** perform the unsafe
release observed in original firmware.

No claim is made about physical tuning, Lightning routing, charging, adapter
identity, authentication, 5V output, backfeed safety, real HID/storage devices,
or successful Linux host boot. This remains a ready reviewable piece of the
future external EHCI/OHCI role owner, not a complete USBhost driver.
