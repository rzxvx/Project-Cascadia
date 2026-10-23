# USB vendor current request: host offset to charger software budget

Research date: 2026-10-11. This is offline evidence, not a charging fix or a
host/device activation procedure. No USB control transfer, sysfs change,
PMU operation or iPad boot occurred.

## Inputs and reproducibility

Firmware is the official iPad2,5 8.4.1 build 12H321 kernelcache, SHA-256
`db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c`.
Board dispatch is the extracted p105 ADT, SHA-256
`5c346586116de1bb32606acb27cad3158bcaa8a1318a10ee9631de09884cfd72`.
The IPSW and extraction provenance are documented in `p105-charging.md`.
All addresses here refer exclusively to this firmware; they are not assumed
to describe the connected iPad's iOS 9.3.5 image.

`tools/power/emulate-usb-current-request.py` verifies the kernelcache hash,
then runs the actual request receiver, current calculation and deferred worker
instructions. Scheduling, USB acknowledgement, ARMFunction provider delivery
and object/task/thread cleanup are explicit mocks. The generic USB request
dispatcher is stopped at its entry when the packet does not match this
focused current-request path. Base current and cap are synthetic fixtures.
No firmware binary or disassembly is included in the repository.

```sh
python3 tools/power/emulate-usb-current-request.py \
  build/firmware/kernelcache.12H321.macho
python3 tests/test-usb-current-request-emulation.py
```

Eight tests passed in the existing resource-limited build image with
Unicorn 2.1.4. They cover both accepted request directions, signed 16-bit
boundaries, request-type dispatch, optional caps, worker delivery, offset
reset, rejected packets preserving prior offsets, and wrong-image refusal.
Tests skip explicitly if the local image or Unicorn is absent; a skip does
not constitute verification.

## What the Linux host driver actually sends

The Linux host driver accepts Apple USB product IDs in the 0x12nn range.
Its fast-charge setter sends a device vendor OUT request 0x40 with no data,
and puts the same 2500 mA **offset** into wValue and wIndex; its trickle
setter uses zero offsets. It exposes the requested charge type, without
measuring the delivered current or establishing the port's electrical
capacity. Source pinned to Linux v6.12 revision
`adc218676eef25575469234709c2d87185ca223a`:
[apple-mfi-fastcharge.c](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/usb/misc/apple-mfi-fastcharge.c).

Module presence or a successful request therefore cannot establish that
the Firebat supplies 2.5 A, that the iPad draws that amount, or that such a
load is supported by the port/cable. This research does not activate the
driver. The observed slow charging remains unmeasured electrically.

## Exact 12H321 receiver and current calculation

IOUSBDeviceFamily function `0x806eb678` reads the setup packet at the
request object's +0x0c. It requires bmRequestType 0x40 or 0xc0, bRequest
0x40 and wLength zero. It loads **signed** 16-bit wValue into controller
+0x8c (the log calls this extra current), and signed wIndex into +0x90
(suspend current). It invokes virtual slot +0x378 with request type zero,
then acknowledges through helper `0x806e7240`.

The request does not directly program the PMU. Acknowledgement alone also
does not certify that the deferred operation later completes successfully.

Actual calculation method `0x806e9d98` uses +0x94 as base current and
+0x98 as an optional positive cap:

| Numeric request type | Actual calculation |
| --- | --- |
| 0, 5 | max(0, base current + suspend offset), capped if +0x98 is positive |
| 1 | extra offset, capped if +0x98 is positive; no zero floor on this branch |
| 2, 3, 4 | 500 mA |
| Above 5 | 0 mA |

These numerical cases are proven by the inline Thumb TBB table and executed
instructions; complete semantic enum names remain unresolved. Startup
`0x806e8dcc`, at `0x806e8e1e..28`, initializes +0x98 through the
`usb_max_current` boot-argument lookup. That cap is separate from the ADT's
PMU input-limit maximum; its live value has not been measured.

Synthetic base 500 mA, offsets 2500/2500 and no cap produce 3000 mA for
request type zero; a synthetic cap 2100 produces 2100. With offsets -600/-600,
type zero produces zero, while type one preserves -600. These are software
arithmetic observations, not recommended currents, charger register settings
or proof that an actual source has been classified or approved for that load.
A new Linux policy must validate both bounds rather than copy every historical
branch as a safe input contract.

## Concrete controller and ARMPlatform dispatch

AppleSynopsysOTGDevice constructor `0x80bb51e8` stores actual vptr
`0x80bc0168` at `0x80bb51f6`. The literal at `0x80bb51fc`, the PC addition
at `0x80bb51f2` and explicit +8 ABI adjustment establish the address point.
From that address point, +0x378 is `0x80bb8370` and +0x37c is
`0x806e9d98`. This avoids shifting virtual offsets by mistakenly treating
the raw table header as the object vptr.

`0x80bb8370` schedules worker `0x80bb8464` with a twelve-byte task:
provider pointer, controller pointer and request type. The worker executes
the base current-setter bookkeeping, calls the actual virtual current getter,
then invokes the provider's ARMFunction +0x3c with pointers to the computed
current and the original request type.

Startup `0x80bb5e6c..78` resolves `function-usb_500_100` and caches it at
controller +0x240. In the exact p105 ADT this key targets
`/device-tree/charger`, provider phandle 0x5d000000, FourCC **CHRG**
(integer 0x43485247), argument zero. It is not the lower-case PMU **chrg**
backend. ARMPlatform handler `0x804c6e7c` compares the CHRG literal at
`0x804c6fb0`; its binding branch resolves callback `0x804c6fe0`
from the PIC literal at `0x804c6fd0` plus the PC addition at `0x804c6f18`.

That callback reads the delivered current and type pointers, checks existing
source/profile state, can reject incompatible updates, performs the inherited
reset with reason zero, caches the accepted report at +0x160 and type at
+0x168 for the active port, and calls whole-source recalculation
`0x804c75b8`. The request therefore joins source policy; it does not override
the physical source, battery-temperature policy or all current ceilings by
itself. See `p105-charging.md` for the independently traced source classes,
handshake inhibition and input-current versus battery-current distinction.

## Consequences for the Linux charging port

The missing Linux integration has at least two distinct pieces: the gadget's
USB configuration/disconnect/suspend budget path documented in
`p105-usb-charge-budget.md`, and any explicitly supported vendor-offset
receiver. A vendor request may advertise a host's willingness to allow more
current, but requires source validation, clamping, detach invalidation,
role exclusion and battery/fault protection before it can become a PMU
transaction. Adding an unconditional request-0x40 handler or changing a USB
descriptor alone would not complete charging safely.

Remaining gaps are the live host-port capacity, the connected iOS build's
receiver, actual `usb_max_current`, source-classification transitions and the
complete D1946 fault/voltage/termination behavior. Active charger callbacks
remain disabled. This artifact is suitable as reviewer-visible reproducible
evidence; it is deliberately separate from production kernel fixes.
