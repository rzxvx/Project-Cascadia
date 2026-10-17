# Concrete P105 solution paths after the deeper firmware comparison

2026-10-12. Official 8.4.1/12H321 remains the build firmware. Official
9.3.5/13G36 is an additional offline research input, not a replacement iBEC or
boot recipe. All findings here are source/emulator/build evidence. No iPad
access, NAND operation or host service/network modification occurred.

## Charging

There is now an actual isolated Linux-C current controller in
[`experiments/p105-current`](../../experiments/p105-current/). It uses regmap,
a central 72-event IRQ domain and power_supply, checks masked writes/readback,
preserves inherited current ceilings, and latches failures. Suspend and removal
inhibit without silently restoring the old permission on resume. No DT node or
default build hook enables it.

The physical CV mapping was narrowed rather than guessed: actual 12H321
source+0x9c→+0xa0→PMU+0x380 ends in AppleDialogPMU's `debug` dictionary handler.
Absent `debug`, that forwarding performs no PMU bus operation. Thus the owner's
`ChargeLimitVoltage` property is a software cutoff, not a demonstrated physical
CV setter. The freshly derived 13G36 backend adds an optional
`AppleForceResetEndOfCharge` path using the known input-control masks; it still
does not establish a voltage-programming API.

Both official ADTs bind PMU IRQ to GPIO pin19, Apple flags1. Original GPIO-IC
initialization translates it to mode3, matching Linux level-low; the GPIO
controller's parent is AIC119. The current normal MFD has no central PMU IRQ
owner. Reading all event banks outside an exclusive owner can consume events,
so the experimental charger cannot coexist with another independent event reader.

The normal HDQ word reader is corrected to high–low–high, with bounded retry,
transport errors and valid signed `0xffff` handling. The actual Apple reader
and [TI-authored Linux HDQ driver](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/power/supply/bq27xxx_battery_hdq.c)
support this coherence mechanism. A new consumer snapshot/lifetime API remains
separate research. A power_supply reference alone does not pin its driver's
devm context through removal; managed links must target the actual producer.

A charger still needs completed USB/accessory configuration combined with
fresh bounded gauge measurements. A successfully completed standard USB
configuration grants the advertised descriptor budget; an unsupported current
control callback is a separate missing hardware hook. The current cdc2 gadget
advertises0mA, and no such protocol observation replaces analog validation. `vbus_draw` or gadget state alone is insufficient
to establish a completed configuration. The generic S5L PHY does not supply the
legacy DWC2 current-grant path. No positive permission is synthesized from an uncompleted
request, stale reading or failed request. Analog retention across the exact
boot handoff and physical protection remain unverified.

The prototype's1000mA enable threshold is a conservative experiment restriction,
not a proven physical charging minimum. The original consumer compares an
accumulated owner value; it does not establish that500mA cannot trickle-charge.

An actual [combined source provider](../../experiments/p105-source/) now observes
completed standard USB2 configuration and combines it with the bounded HDQ-v3
reader under managed supplier/consumer lifetimes. It does not replace a failed
current-control hook with a grant: actual libcomposite configuration and matching
EP0 status completion establish the protocol budget independently. Five actual-C
ASan/UBSan groups execute the real observer, source/combined adapter, HDQ functions
and unmodified libcomposite set_config against fake controller/gauge transport.
The present cdc2 descriptor0mA remains0; USB2 budgets are bounded to500mA.
The current frontend now has a default-false owned USB2 route with actual platform
and battery references, constructing the combined provider before any PMU read.
No dummy input supply is used. Queue/stop fencing, parent IRQ synchronization,
foreign callback removal and a fault self-requeue loop are repaired and tested
with actual functions, including a deterministic pthread interleaving. An owned atomic notifier chain now propagates actual UDC invalidations directly
to the frontend epoch and urgent worker, with RCU callback drains before teardown.
Five actual-C notifier tests include deterministic stop/close races and the
historical stop failure. Physical cutoff timing and analog safety remain open. A further default-false owned-USB2 policy now permits bounded nominal100/500mA
plans (at most80/480mA requested charge codes), retaining all source/gauge/analog
assertions and both zero slot caps. Seven actual-worker ASan/UBSan tests pass.
Stock cdc2 remains0mA, so this still does not enable charging in that configuration.
Physical calibration, slot selection and protection remain unverified. No DT,
DWC2 patch, boot change or runtime activation is supplied.

## Linux-owned NAND

The concrete path is exclusive PPN SLC blocks → writable MTD → UBI → UBIFS,
not a writable reconstruction of Apple's FTL. The new
[`p105-ppn` component](../../experiments/p105-ppn/) executes the reconstructed
single-page/block protocol through unbound callbacks, bounds physical addresses,
holds unique DMA leases and propagates completion/cancel errors. A frozen clock
cannot leave its polling loop unbounded. Original firmware's conditional DMA
timeout success is intentionally not copied.

Single-page success matches the independently emulated original IOP register
and DMA sequence on both buses. EF3380's transport and original conditional
callers are traced, but its device acknowledgement/idempotency effect remains
unestablished. The engine snapshots error lists after DMA quiescence and does
not send that unknown acknowledgement or replay a partially completed program.
It reports transport completion with unknown durability and exposes no MTD writer.

Two additional C components now prepare real non-AES CDMA chains and per-controller
sequencer upload plans. Complete single batches match96 original DMA traces;
25 fresh sequencer cases match original arithmetic. No Apple microcode is
included. The original clock field is hclk/1000 in kHz; synthetic fixture
clocks are arithmetic stress inputs. These preparers do not submit DMA or
claim durable flash completion.

A separate [Linux-owned BBT transaction state machine](../../experiments/p105-ppn/BBT.md)
now requires a persistent intent before an operation and distinct confirmed body
and commit-manifest stages. Unknown durability, bad confirmations or a corrupted
committed intent keep recovery read-only; an uncertain block is not silently
reclassified bad. Reinitialization cannot discard retained state, and even a new
workspace requires two identity-bound blank-slot witnesses before Genesis.4075
actual C calls pass an independent CRC/persisted-copy/powercut model. This is an
independent Linux journal design without a hardware backend;
fixture receipts are not physical durability evidence. The new explicit-endian
[body/manifest codec](../../experiments/p105-ppn/CODEC.md) prepares two complete16KiB
byte pages with strict schema, identity, CRC and padding checks.33,408 C calls,
including every single-byte corruption across both pages, pass an independent
byte oracle. Decode emits no read/blank/durability proof flags and is not a stable
hardware storage format.

The filesystem layer is already concrete: an Alpine UBIFS/UBI pair has been
generated and checked locally. See [the offline SLC rootfs builder](p105-linux-owned-rootfs.md).
The profile's 16KiB I/O, 128-page/2MiB SLC block and 64-LEB reservation are
explicit synthetic assumptions. Actual chip capacity/mode/exclusivity, controller
binding/BBT and crash guarantees must precede writable installation. Keeping
iOS requires its global FTL; giving it up requires an independent Pico/BootROM
boot path, which this user's present setup lacks.

## External USB host and power

The README's old claim of absent EHCI external ports was directly contradicted
by **both** official ADTs. Each declares external EHCIport1/type2/mask0x30002 and
OHCI0port1/type2/mask0x30006, separately from WLAN's HSIC Port3. The exact trees
can be checked with `scripts/check-external-usb-ports.py`. A DWC2 forced-host
build remains an isolated experiment and does not identify the Lightning route.

[`p105-usb-client`](../../experiments/p105-usb-client/) implements the actual
two-phase port-client lifetime in C, including standby retention, owned-error
state and detach epochs. It requires the completed controller transition and
an idempotent shared PHY/clock backend; it does not reset the WLAN block or
pretend a generic power-off proves resource release.

A third isolated [register backend](../../experiments/p105-usb-registers/) now
implements shared owner clock-mask aggregation and the original host tuning/reset
sequence using Linux regmap. It defaults denied, checks each I/O and connection
epoch, and keeps failed ownership latched. Native final gate releases additionally
require separately verified parent-graph policy. It does not claim a complete
PHY power transition: real power-level lease and controller quiesce are still
required. Five owner scenarios and four tuning combinations match original
instructions with synthetic providers.

Apple's [powered Camera Adapter support](https://support.apple.com/en-us/111811)
documents an external Lightning power input supplying the iOS device and USB
peripheral. Its [mini1 compatibility filter](https://www.apple.com/shop/ipad/accessories/chargers-adapters/ipad-mini-1st-generation-usb-3)
lists this adapter, with iOS9.3+ required. This provides a manufacturer-backed
power topology for eventual testing without inventing an internal iPad5V boost.
It does not prove Linux enumeration, PMU charging or a particular hub's isolation.

The 13G36 vendor51/53 transport is now implemented as a separate default-denied
[`p105-usb-role` parser](../../experiments/p105-usb-role/). However, the actual
`iaptransportd` caller uses `isCarPlayAllowed`, setting value-mask0/1 and
index-mask0. It is a positive CarPlay/Stark finding, not a Camera Adapter
handshake. Actual iOS gadget registration also includes vendor HID descriptors.
The camera path must not be reduced to this one vendor hook.

The direct exported `IOAccessoryManagerEnableUSBHostMode` API chooses mode2,
which maps through the actual 13G36 mode table to USBd control0x25; vendor51
chooses mode3/control5. Original USBd flag0x20 changes the software cable's
`USBDeviceNeedsAuthentication` classification, retaining `USBDevice` instead.
That is not proof of physical authentication or power permission. Mode, ID,
data mux, clock/PHY and electrical supply are separate verified boundaries.

The complete actual userspace forwarding path is now also emulated: official
13G36 `iaptransportd` method `setUSBMode:forMode:` at0x166d8 uses the actual
`iapPortManagerForDevPort:`/`service` selectors, opens that service, and forwards
the requested mode unchanged to library entry0x2a16eb40. Its actual import is
`IOConnectCallScalarMethod`, selector1 with one64-bit scalar/highword0. Six mode
cases plus an absent-manager case pass bounded original instructions with fake
IOKit/Objective-C services. This is a generic mode producer, not Camera Adapter
identity or physical power permission. Source daemon SHA256 is
`0eabef460b814484590b1337e7349ddeb37385d1fd80c83332dd562c3410ad1e`;
shared-cache SHA256 is
`4f5b5d73244c53fd0bdc5b8012106b10d0dd7607cc850bf7c8f9e9ef3a3f733b`.

The separate [staged power primitive](../../experiments/p105-usb-power/) adds
actual Linux clock-rate and regmap operations for original PHY power stages.
Its frequency code is derived from named `usbphyclk`12/24/48MHz, separately from
ADT ref-clock-sel3. Every clock/epoch/I/O transition is checked. Both official
ADTs lack the specific `power-gates` property, which is not the inverse of
normal clock membership and does not account for accessory/VBUS power paths.
All25 level transitions match the original semantic fixture; complete owners,
a real clock provider and verified adopted state remain required.

A separate [standard reset client](../../experiments/p105-usb-reset/) preserves
identity membership for reset kinds0/1/5 and shared PHY+8 bit0x10. It requires a
real temporary Power4 owner lease and refuses HSIC kinds2/3/4. The actual WLAN
HSIC producer uses kind3, clock-mask0x30010 after filtering, membership2/Level1,
and reset bit0x4. External EHCI kind0 uses membership4/Level2 and bit0x10. This
source separation does not establish physical WLAN continuity through host tuning.

The [native host clock broker](../../experiments/p105-native-clock/DESIGN.md)
now supplies the actual44-byte-table parent graph and ordered physical clock/
gate transitions, with protected foreign/global parent references, first/last
own-client accounting and bounded acknowledgement checks. Its16 actual-C groups
compare normalized native request traces and every I/O/epoch failure boundary.
An additional historical-source control proves that losing an adopted epoch
validator must latch ownership rather than silently resume after restoration.
This closes the register-plan implementation gap; it deliberately retains the
unsatisfied dynamic provider, all-global-owner and inherited-state adoption
contracts. Neither a compile nor a ready-bit read manufactures those contracts.

## Review and physical limits

The components are compiled against actual Linux6.12 ARM headers and tested
against original-code semantic fixtures with synthetic providers. They are
reviewable source foundations, not enabled features or hardware-safe boot
artifacts. The normal kernel keeps NAND read-only, gadget peripheral mode and
the existing WLAN setup. Any device validation still requires explicit user
approval; these local changes grant none.

## Checked wall-source and cancellation transports

The unregistered [wall-source candidate](../../experiments/p105-wall-source/EVIDENCE.md) implements actual Tristar routing, checked status/readback and original ADC6 classifier arithmetic. Seven complete-C sanitizer groups include72 independent original classifier comparisons, retained failed restore ownership and one-shot initialization. It returns observations only; original source profiles do not grant Linux current. A real exclusive Tristar/ADC owner and qualified restoration remain required.

The unbound [CDMA cancellation candidate](../../experiments/p105-ppn/CANCELLATION.md) implements the original enable/abort/poll/disable order with fresh bounded cleanup timing and immutable ownership checks. Its9,118 C assertions include288 independent original successful write traces. Native cancellation retains callback/token and does not drain IRQs. Therefore successful transport remains WAIT_FENCE/-EINPROGRESS; no buffer release, zero-success adapter, NAND write or durability assertion is implemented.

The unregistered [ADC6 polling candidate](../../experiments/p105-adc6/EVIDENCE.md) now implements the complete PMU43/44 normal conversion sequence, a50ms/64-poll limit, checked zero-stop/readback and retained UNKNOWN ownership after any ambiguous conversion. Seven complete-C sanitizer groups cover each error/epoch boundary, source lifetime and generation wrap. Independent original instructions qualify the24MHz/ticks÷24 software timer contract and reproduce unchecked original errors/timeout without cleanup. No real all-ADC reservation/late-completion fence or analog safety grant is supplied.

The [Lightning digital-ID decoder](../../experiments/p105-accessory-id/DESIGN.md) implements THS frame parsing and six-byte native selector/request semantics in pure Linux C.4,360 original valid/invalid cases and five transport controls qualify this stage. Source context names now distinguish debugger, ignore-vbus property and md-shaped boot root; none is accessory authentication. The actual current/rail/authentication producer remains separate.
