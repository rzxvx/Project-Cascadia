# Isolated P105 power, storage and USB components

These are concrete Linux-C source components for review and offline testing.
The normal Cascadia build does not descend into this directory, source these
Kconfig fragments, create their DT nodes or install their callbacks. They have
no enabled configuration or qualified device integration. Several components
implement actual I2C/regmap operations; their required physical ownership and
hardware validation are still incomplete. No working physical
charging, USB host or writable NAND result is claimed.

| Component | Implemented | Deliberately still required |
|---|---|---|
| `p105-current` | Actual regmap/platform/IRQ-domain/power_supply driver; checked current masks and readback; epoch-bound producer contract; fault latch; inhibit on removal/sleep | Exclusive MFD event ownership, analog retention/CV/termination/protection evidence, controlled immediate source shutdown and physical tests |
| `p105-source` | Completed-configuration/current observer; managed DWC2/HDQ opaque readers and combined epoch/freshness-bound callback provider | Controlled immediate source invalidation/shutdown, external accessory source support and physical analog validation; hook patches remain unapplied |
| `p105-ppn` | Single SLC-page program/block erase orchestration; checked complete non-AES CDMA descriptor plans; per-controller sequencer preparation; owned DMA leases and cancellation; Linux BBT transactions and explicit-endian16KiB body/manifest codec; no blind replay | Actual MMIO/CDMA/sequencer binding, exclusive SLC blocks/geometry/BBT, completion and power-cut guarantees; no MTD writer exists |
| `p105-usb-client` | Checked two-phase removable-port clock/PHY membership lifecycle; active/standby/off; detach epochs and retained fault ownership | Real shared-PHY/clock provider, completed EHCI-port transition/quiesce, accessory identity/mux and verified external power |
| `p105-usb-registers` | Checked shared clock-mask aggregation and real regmap tuning/reset primitives; bounded polls; retained errors and per-owner epoch-validation callbacks | Complete actual platform clock graph, real shared power lease/controller quiesce, accessory mux/VBUS and physical validation |
| `p105-usb-power` | Real clk_get_rate/regmap staged PHY power transitions, preserving RMW and fault ownership; exact supported frequency codes | Authenticated clock provider/binding, adopted initial state and exclusive shared-owner/quiesce lease; no host/VBUS registration |
| `p105-usb-reset` | Identity-bound standard reset groups0/1/5, real regmap RMW and temporary-power lease contract; preserves other PHY reset bits | Actual shared power membership provider, controller quiesce/adopted ownership and physical validation; no HSIC reset writes |
| `p105-usb-controller` | Exact selector RMW and owned EHCI reset pulse; checked clock acknowledgement, epoch and all-port quiesce; retained partial errors | Actual role/admission/publication owner, native clock/PHY/VBUS contracts and physical continuity; no complete host transition |
| `p105-usb-role` | Actual USB 51/53 setup decoder, exact lengths/endian/masks, default-denied policy callbacks | Real gadget function/completion and authorized accessory policy; the known original userspace producer is CarPlay, not a Camera Adapter proof |

The transport-only PPN return explicitly keeps `durability_unknown=true` even
when the original successful register/DMA sequence matches. Do not translate it
to successful MTD write/fsync/UBI commit. Its failed DMA cancellation retains
the relevant leases/buffers; no reset, erase-all or EF3380 acknowledgement is
supplied. Synthetic geometry and owned ranges are test inputs, not discovered
target hardware facts.

The USB-client callback contract is idempotent membership of **one owned
client**, not blind generic PHY power references. External Port1 uses the
original EHCI/OHCI route; its standby state retains host membership. A scheduled
transition is not a completed controller result. An old attach epoch cannot be
silently replaced to release another device's resources.

The current-only driver preserves unknown analog bytes. It supplies no CV,
termination, OTP or watchdog initialization, and refuses probe without its
explicit experiment assertions and a real producer. The epoch/freshness checks
are software invariants, not proof of physical battery safety.

## Verification

`python3 scripts/test-offline.py` includes actual-C component tests. Hosted C
builds use fake callbacks only, with UBSan on native component binaries/shared
libraries and ASan/UBSan on the combined source/gauge fixture. Golden fixtures
record semantic outputs of separately executed, hash-pinned original ARM code
with synthetic provider/MMIO responses. They contain no firmware instructions
or device dumps. Normal kernel source/config is not modified by these tests.

ARMv7 object compilation against the actual patched Linux 6.12 generated
headers passed for the current driver/core, PPN engine, CDMA/sequencer planners all six USB components and the BBT transaction/codec components.
An object compile checks APIs and target code generation; it does not execute
the Linux driver framework or establish a hardware binding.

The PPN successful trace can be regenerated from the user's own exact official
12H321 extraction using `tools/nand/emulate-ppn-program.py --kernelcache FILE
--output NEW_JSON`. This runs only in Unicorn, with bounded original code and
strict synthetic MMIO/DMA/helper hooks. Both source input and output are regular
files. It creates no device transport and performs no real NAND operation.

See each component's `EVIDENCE.md` for the source addresses, checked boundaries,
and original staging provenance. Legacy staging paths identify local research
inputs; the portable tests at `tests/test-experimental-*.py` are the exported
reproduction entry points. Do not copy a staged component into the runtime
kernel merely because its offline tests pass.

Additional isolated components: [bank0](p105-bank0/EVIDENCE.md) implements Linux I2C/mutex/atomic transport with a globally owned page-selection cache; [AuthCPAID](p105-authcp-aid/AUTHENTICATION.md) implements a pure version2/3 signature-exchange state machine. Both reject unprovided ownership, preserve ambiguous faults and remain outside normal activation.

[THS IDbus](p105-ths-idbus/DESIGN.md) adds the actual regmap submission/response primitive for the P10532-byte windows. It refuses unsupplied physical ownership, retains ambiguous transactions and remains unregistered/default-off.

[Global PMU parent](p105-parent-pmu/EVIDENCE.md) proposes a shared regmap bus for
the existing GPIO, regulator, WLED and shutdown consumers. It selects bank0
under the adapter lock for every transfer, holds that lock across regmap RMW,
and checks the separate atomic shutdown path. The replacement parent source,
header and patch are review material only; the normal overlay is unchanged.
This does not supply an all-ADC conversion lease or authorize charging.

[Shared PMU/ADC owner](p105-parent-adc-owner/EVIDENCE.md) is a later alternative
proposal: it reserves the actual A5 PMU address against generic/raw transfers,
rejects unsupported address aliases, and holds the shared parent and adapter
locks over an entire ADC conversion. Its A5-specific single-transfer leaf avoids
mutable generic retry policy. A private lockless regmap cannot escape through
the parent's regmap lookup. Probe rollback, close, suspend and shutdown are
checked. Physical admission remains closed: each conversion needs a new real
quiescence witness, and no runtime producer for that witness is supplied.

[Mathematical signature verifier](p105-authcp-verify/VERIFY.md) checks the
source-backed v2 RSA1024/prehashed20 and explicitly supported v3 P-256/raw-rs/
prehashed32 profiles. Real-crypto tests reject the substituted late signature
chunks from the complete THS/parser pipeline. Supplied public keys remain
untrusted: certificate-chain, accessory identity, freshness and role/current/
VBUS authorization are not provided. This host-only component requires
Python3.10+ and cryptography41.0.7 and is not installed in the RAM image.

[Read-only MTD lifetime proposal](p105-h2fmi-lifetime/EVIDENCE.md) retains H2FMI
context for open clients and deferred final device releases, drains synchronous
reads on removal, and opts out of the unretained regular-NVMEM path. Its generic
MTD core/header patch supports built-in core/providers only. This proposal is
not installed by normal builds; it neither enables NAND writes nor supplies an
inherited IOP/CDMA hardware fence.

[Native ADC retirement evidence](p105-adc-native-retirement/FINDINGS.md) traces
the actual event-source/provider chain and selected12H/13G ADC callbacks.
Disabling the event source does not drain queued work; cached events and ignored
read errors can complete a software ADC state. Source getter/constructor dispatch
does not prove the live device's IOKit selection or physical converter retirement.

[Native iAP policy facets](p105-iap-native-policy/IAP_TRUST_INTERFACE.md) expose
the actual anchor-selection, weak-key and securityd parent/policy boundaries.
Real source-anchor metadata/self-signature inspection is separate from the
synthetic certificate/CF providers in native emulation. No complete peer-chain
trust validator or accessory/current/VBUS grant is supplied. These source-only
replays require the caller's SHA-pinned official input files and are optional;
their independent native controls are not added to the ordinary offline count.

## Further qualified source and host-only certificate evidence

`p105-iap-direct-evidence` verifies real direct leaf/issuer signatures and nonce signatures under a deliberately narrow metadata subset.22 real crypto groups include malformed and unsupported lazy extension decoding; it never supplies complete chain trust, accessory identity or power authorization. Its9 original routing cases use explicitly synthetic crypto providers. `p105-iap-native-policy/path-closure` adds6 original basic-path and2 calendar-construction cases with synthetic metadata/CF arithmetic.

`p105-adc-reset-evidence` records16 additional original controls: alternate15d/15e operations do not establish normal43/44 cancellation; a failed cancel write can return the same error, and a cached config write can report success after transport failure. It adds no runtime gate, allowlist or reset sequence. All original-input replays are optional and separate from ordinary test-entrypoint counts.

`p105-h2fmi-lifetime/prepare-lifetime.py` optionally applies the pinned3-source proposal only to an explicitly acknowledged private prepared tree; repository/proposal binding rejects routing mistakes. [Application and manual qualification](p105-h2fmi-lifetime/PRIVATE_APPLICATION.md) exclude automatic builds and explain environment clearing. No normal-image activation follows.

[Review map: normal corrections, optional faults and their tests](../docs/research/p105-review-map.md)
