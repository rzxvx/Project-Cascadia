# Concrete implementation and integration order

The research branch supplies code for the requested features and fixes actual
runtime errors. Charging, writable NAND and the external host connector have
not been activated or hardware-tested. Every new experiment remains outside
normal patch/DT activation. This document identifies the implementation path
and the remaining evidence needed to join its components.

| Feature | Implemented software | Next integration evidence |
| --- | --- | --- |
| Charging from configured USB2 | Checked current core, owned completed-EP0/HDQ producer, drained invalidation notifications, opt-in bounded100/500mA plans, optional CDC declaration | Final analog/protection state after boot; calibrated source/battery readings; physical detach/suspend/fault cutoff |
| Charging from a wall charger | Original source/current policy and boot PMU helpers investigated | Actual owned DCP/Apple-divider source detector and removal/fault lifecycle; a USB configuration descriptor alone cannot detect a brick |
| Linux-owned NAND | PPN engine, CDMA/sequencer plans, completion/lease model, BBT transactions/codec, offline UBIFS/UBI files | Exclusive boot/IOP/CDMA handoff, real stop/drain witnesses, geometry/SLC qualification, writable backend and power-cut durability |
| External USB host | Native parent clock broker, port memberships, checked PHY power/tuning/reset, controller mux/reset, source-backed external Port1 route | Join all writers/owners, preserve WLAN Port3, authenticate/route the actual adapter and verify protected VBUS |

## Charging path

Stock CDC2 declares0mA; its completed configuration therefore still yields a
zero allowance in the owned source reader. The isolated gadget candidate accepts
only0/100/500mA, defaults to0 and validates before registering/binding. Its actual
libcomposite/EP0 tests establish encoded descriptor bytes0/50/250 and matching
completed USB2 budgets. Positive profiles reject non-USB2 UDC capability.
This patch is deliberately unsourced in the normal build.

The current candidate independently defaults off. With its further USB2 opt-in,
the current plan is bounded by the declared input and floors charge codes to
40mA: nominal100/500mA plans can request at most80/480mA. The real register
ceiling can reduce them further; the retained zero thermal slots remain zero.
These numbers are software current codes, not measurements or promised net
battery charge. Device consumption shares the available input power.

The software chain is descriptor → successful configuration/status cookie →
owned source/gauge tuple → checked current transaction. Pending/failed EP0,
disconnect, suspend, stale epochs and faulty status cannot grant a plan.
Source notifications invalidate and queue work; they do not establish an
electrical response deadline or autonomous battery protection.

The boot chain itself contains PMU initialization and unchecked helper errors.
An earlier iOS charging state cannot simply be assumed retained. Final CV,
termination, autonomous fault/temperature protection and safe calibration must
be independently established after all boot stages. A wall charger additionally
needs its own actual source detector; it may never complete a USB configuration.

Reproduction: `tests/test-experimental-gadget-maxpower.py`,
`test-experimental-source-provider.py`, `test-experimental-source-notify.py`,
`test-experimental-current-small.py` and the retained current tests. These are
host-only tests with explicit mocks, not activation commands.

## NAND path

The intended architecture gives Linux exclusive flash ownership and uses a
qualified SLC MTD aperture with UBI/UBIFS. It does not extend the read-only iOS
FTL reconstruction into a writable coexistence scheme. Erasing iOS would also
remove the existing kDFU entry; an independently demonstrated Pico/BootROM boot
and recovery path must precede that destructive option. This unit has no Pico
and no approved NAND operation.

The engine binds each data/OOB DMA lease to controller, epoch, transfer and
channel identity. Successful transport actions advance to a separate fence;
unknown errors retain ownership. BBT body/manifest publication additionally
requires external durable and quiesced receipts and identity-bound blank
witnesses. Its serializer never manufactures those receipts.

Actual IOP sleep replies precede callbacks/CPU sleep. The native stop helper
ignores provider errors, and its software running flag is not a hardware check.
IOP gate107c, CDMA gate1078 and reset1058 are separate resources. The actual
native graph can release IOP while CDMA remains active. The next backend must
independently halt/adopt prior owners and stop/drain each owned DMA generation,
including late interrupts, before leasing or freeing its buffers.

After that source-backed backend exists, geometry/SLC mode, address-bound final
PPN status and power-loss behavior still require approved hardware validation.
The existing local UBIFS/UBI images use explicit synthetic geometry; this
device's capacity and usable eraseblock map have not been qualified.

Reproduction: `tests/test-experimental-ppn*.py` and
`tests/test-ubifs-image.py`. See the [completion contract](../../experiments/p105-ppn/COMPLETION.md)
and [actual IOP handoff](p105-iop-handoff.md).

## External USB path

The source-backed external route is EHCI removable Port1, with OHCI0 companion
for direct low/full-speed devices. The existing Linux EHCI instance also owns
WLAN HSIC Port3. Forcing DWC2 host alone does not reproduce this route. A first
qualified high-speed device or high-speed hub with TT avoids requiring the
direct-FS companion, but has not been tested on this iPad.

The actual clock graph and true clkG bridge now have executable source proofs.
The broker retains protected foreign/global references and uses ordered
clock/gate transitions. All global owners and bypassing writers must join its
sleepable owner lock: the normal PHY's direct PMGR writes currently prevent
that claim. A permanently pinned-parent design must still exclude such writers;
reading an enabled bit cannot recover old counts or establish lifetime ownership.

The clock broker, port membership, PHY power/tune/reset and controller mux/reset
must be joined under one epoch/owner lifecycle, with UDC and every shared EHCI/
WLAN client quiesced at disruptive transitions. Partial errors retain their
pending disposition and prohibit blind retries. The normal WLAN setup has not
been replaced by these isolated components.

Accessory LDO5, Tristar VBUS sensing, mux selection and EHCI PORT_POWER each
have different source evidence. None alone identifies a protected5V source.
The actual adapter identity, rail/conversion path, current limit, reverse-feed
isolation and detach/fault handling remain required before supplying the bus.
An externally powered adapter is a topology to qualify, not a demonstrated
Linux workaround. Native5V output is not established by this research.

Reproduction: `tests/test-experimental-native-clock.py`,
`tests/test-pmgr-clkg.py`, the USB power/reset/controller tests and
`tests/test-experimental-usb-provider-loss.py`.

No hardware permission, normal feature activation or publication follows from
these source tests. The local PR package makes the changes reviewable before
any separate device-validation decision.

## Newly implemented transport boundaries

[Wall classification](../../experiments/p105-wall-source/EVIDENCE.md) and [checked ADC6 conversion](../../experiments/p105-adc6/EVIDENCE.md) now provide actual Linux-C source building blocks. Their callers must implement one exclusive Tristar/all-ADC owner with checked restoration and coherent attachment epoch; the wall measurement receives raw conversion results and never grants a current budget itself. UNKNOWN ADC state quarantines users even after a successful zero-stop readback. Charger CV/termination/protection qualification remains separate.

[CDMA cancellation](../../experiments/p105-ppn/CANCELLATION.md) now supplies bounded checked transport. Its successful result remains WAIT_FENCE; only a separately qualified backend can prove no future memory accesses and drained native/Linux IRQ callbacks before any memory release. Masking an IRQ does not erase a callback already captured by the original dispatcher.

[Checked bank0 transport](../../experiments/p105-bank0/EVIDENCE.md) now selects page0 only after an exclusive ALL-PMU lease, binds the cache to that owner generation, and quarantines uncertain selector/data transfers. Its owner must cover every PMU consumer and invalidate on ownership gaps before any child can use this primitive. The current normal overlay supplies GPIO, regulator, WLED and shutdown consumers; it does not supply a PMU RTC child. The cached primitive is not a drop-in regmap replacement and does not prove analog protection.

[Global PMU parent proposal](../../experiments/p105-parent-pmu/EVIDENCE.md)
implements a separate, uncached regmap bus for those normal consumers. Each
selector/data operation and a complete regmap read/modify/write hold the
adapter lock. Checked prepare/seal and the atomic final shutdown avoid the
existing direct-SMBus bypass in the opt-in proposal. STOPPED is checked before
and after each atomic transfer; it cannot cancel hardware already executing.
The adapter algorithm/configuration must remain unchanged and retries must
remain zero, including against unlocked I2C-dev configuration ioctls. Raw I2C
may run between separate regmap calls. An all-ADC conversion lease, IRQ/event
ownership and analog safety qualification remain separate work. The proposed
parent patch, header and Kconfig fragments are not installed by normal builds.

The later [shared parent/ADC alternative](../../experiments/p105-parent-adc-owner/EVIDENCE.md)
implements actual complete-conversion locking and a client-bound reservation
inside the A5 I2C driver. Generic transfers cannot address the reserved PMU,
including unsupported high/10-bit address aliases; other valid addresses remain
available. The typed A5 leaf invokes the real controller exactly once and reads
neither mutable adapter retries nor timeout policy. The generic-core EAGAIN
counterexample is synthetic; the current A5 controller does not itself return
EAGAIN, so this is not an observed A5 replay bug. This alternative removes the
earlier retries-zero integration contract but still requires valid provider and
consumer lifetimes. It is uninstalled and defaults off. A physical quiescence
witness is consumed for one conversion; success never manufactures the next
witness. First and subsequent unqualified conversion admission remain denied.

For authorized device diagnostics, use the [key-pinned RAM-session tools](../DEVICE_TESTING.md).
Raw PPN tools now reject a bound H2FMI kernel driver or ambiguous sysfs, while
the normal read-only MTD path remains available. That snapshot rejection does
not establish exclusive inherited IOP/DMA ownership or prevent a later bind.

[Remote-signature exchange](../../experiments/p105-authcp-aid/AUTHENTICATION.md) now produces bounded version2/3 nonce packets, checks actual response semantics, retains failed pending transactions and exposes only unverified bytes. Real IDbus transport, fresh nonce generation, matching certificate policy/public verification and accessory identity precede any role or electrical grant. No host-private signing operation is needed in this audited exchange/verification path; other iAP modes are outside that conclusion.

[Actual selected kDFU sleep](p105-kdfu-sleep-boundary.md) and [FMI client retirement](p105-fmi-client-retirement.md) close concrete software edges, including final-client release before power acknowledgment. They still do not establish inherited IOP/CDMA no-future-access or IRQ drain; no MTD writer is enabled.

[Checked THS transport](../../experiments/p105-ths-idbus/DESIGN.md) now supplies actual Linux regmap staged TX/control writes and bounded single-call status/bulk-RX processing. Every bus error retains UNKNOWN and pending identity; bulk data is checked before reversal/publication. A real shared IRQ/connection/request arbiter must supply physical exclusivity and late-response attribution before it can be joined to the signature parser. Pure numeric tokens and software revocation do not establish that hardware fence.

[Real mathematical verification](../../experiments/p105-authcp-verify/VERIFY.md)
now rejects corrupt reconstructed signatures using the observed native padding,
nonce and signature-size contracts. The source nonce bytes are prehash inputs
and must not be hashed again. Complete-C pipeline tests demonstrate rejection
of the synthetic old-chunk substitution for both supported profiles. This is
host-side verification against an explicitly supplied public key, with every
result retaining unverified certificate-chain/identity and no authorization.
It does not resolve hardware response attribution, authenticate a Camera
Adapter, establish trusted anchors or supply VBUS. Those remain separate steps.

The [read-only MTD lifetime proposal](../../experiments/p105-h2fmi-lifetime/EVIDENCE.md)
closes a separate software lifetime gap: open master/partition clients and
external device references retain context after provider removal/probe unwind.
Current requests drain and new reads fail ENODEV; automatic regular NVMEM
publication is disabled for H2FMI and descendants. The opt-in core contract is
built-in-only and forbids retired-master re-registration. It requires generic
MTD API review and a complete integration build before normal activation;
neither the normal overlay nor the prepared RAM baseline installs it. There
are no program/erase callbacks, NAND-write grant or inherited DMA/IRQ fence.

Further original-code evidence narrows the still-missing interfaces:
[ADC/event retirement](../../experiments/p105-adc-native-retirement/FINDINGS.md)
shows actual queued work surviving event-source disable, cached completion with
fresh zero event bytes, and ignored transport failure. Its13G constructor/getter
proof selects the legacy acquisition slot for the constructed D1946 class;
generic start/stop slots are zero-return stubs, not hardware stop witnesses.
[iAP policy facets](../../experiments/p105-iap-native-policy/IAP_TRUST_INTERFACE.md)
trace actual custom-anchor selection and native weak-key/parent/failure rules.
The source v3 anchor lacks BasicConstraints and carries a critical Apple OID;
an explicit anchor is exempt from the proven parent CA/KU checks. Complete
path/critical-extension/date behavior and a real accessory chain remain required
before a native-profile trust claim. Neither evidence group opens a hardware gate.

## Further qualified paths: direct signatures, ADC cancellation and IOP provider

The host-only [direct signature component](p105-direct-signature-closure.md)
verifies certificate and nonce mathematics with22 real crypto groups. Original
routing9/basic6/calendar2/critical-fallback9 cases have separate synthetic
providers; none supplies full native trust or electrical authority.

[ADC reset evidence](../../experiments/p105-adc-reset-evidence/README.md) adds16
original cases. Alternate logical15d/15e operations and the watchdog-bound
panic-reset callback do not establish normal43/44 cancellation. A native cancel
cleanup can ignore a failed write, and cached config success can hide transport
failure. The existing checked Linux cleanup remains unchanged; no new gate or
parent allowlist is added.

[IOP continuation evidence](../../experiments/p105-ppn/continuation/EVIDENCE.md)
qualifies24 original controls across12H321/13G36. The coherent-property branch
writes a software byte; otherwise actual IOService code obtains a provider and
delegates. The getter's memory barrier orders its provider cache, not DMA. The
dynamic hardware provider remains unqualified; this is a bounded local result,
not an absence claim for the entire platform graph. No writer or physical
stop/reset receipt is enabled.

The optional H2FMI [private-tree installer](../../experiments/p105-h2fmi-lifetime/PRIVATE_APPLICATION.md) binds the actual existing repository/proposal and ignores ambient Git redirection. It defaults to checking and applies only after an exact private-tree acknowledgement; interrupted states refuse. An automatic build wrapper is excluded; the manual recipe clears inherited build variables.24 existing installer controls and11 preserved-historical/fixed containment controls pass independently; portable real-Git boundary controls are also provided. A separate full kernel link passed normal flags with18 baseline warnings, without userspace initramfs or any boot/device test.


### Native key/link closure after checkpoint28

The optional [RSA source evidence](../../experiments/p105-iap-native-policy/rsa-closure/REAL_RSA_PROFILE.md)
replaces synthetic key-size results with seven independently repeated native
DER-import/getter/weak-root cases. Three further original link cases run real
RSA public exponentiation, with independent public signature checks. This closes
specific key/math gaps; it supplies no path construction, anchor legitimacy,
complete iAP trust, adapter-specific authorization or protected VBUS. Current
power and USB-role consumers still receive no physical permission from it.

For focused code review, use the [normal/optional correction map](p105-review-map.md). It links concrete triggers, changed components and actual-source regression runners without proposing feature activation.
