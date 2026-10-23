# Review map for the P105 development draft

Start with the normal corrections, then review the optional components as
separate proposals. The exploratory branch deliberately keeps the latter outside
normal Kconfig, Device Tree, patch application and boot-bundle activation.
This index does not propose enabling them or merging the entire branch at once.

## Normal corrections

| Area | Source or explanation | Relevant offline tests |
| --- | --- | --- |
| Battery reads and cache | [HDQ driver](../../patches/files/drivers/power/supply/apple-hdq-battery.c), [capacity rejection](p105-battery-validation.md) | [Battery](../../tests/test-hdq-battery.py), [word coherence](../../tests/test-hdq-word-coherence.py) |
| Raw controller exclusion | [nandctl](../../tools/nand/nandctl.c), [iosnand](../../tools/nand/iosnand.c) | [Raw read gate](../../tests/test-nandctl-read-gate.py), [MTD open](../../tests/test-iosnand-mtd-open.py) |
| Read-only MTD error/progress | [H2FMI driver](../../patches/files/drivers/mtd/devices/mtd-apple-h2fmi.c) | [H2FMI reads](../../tests/test-h2fmi-read.py), [MEMREAD](../../tests/test-iosnand-mtd.py) |
| Shared USB error/lifetime paths | [Normal PHY](../../patches/files/drivers/phy/phy-apple-s5l-usb.c), [error analysis](p105-usb-phy-errors.md) | [Core lifecycle](../../tests/test-usb-phy-core-lifecycle.py), [failure policy](../../tests/test-usb-phy-fail-closed.py) |
| Loader stack and build | [Staging loader](../../pongo/staging-loader.S), [stack analysis](p105-staging-stack.md) | [Stack](../../tests/test-staging-stack.py) |
| Explicit SSH diagnostics | [Host helper](../../tools/host/ipad-session.py), [device testing](../DEVICE_TESTING.md) | Process-local SSH tests in the ordinary offline suite |

Raw-tool refusal prevents a specific conflicting access path; it is not a
positive exclusive controller/DMA lease. MTD geometry is still hardcoded.
Battery reads and USB error handling do not prove charging or external host mode.

For the live desktop, [touch registration and gesture discovery](p105-desktop-touch-observation.md)
separates Xorg's input path, Touchégg's initial event queue and incorrect physical
size metadata. Those source observations are not a physical touch qualification.

## Optional faults corrected during the follow-up review

These components remain uninstalled. Each linked test executes actual source
functions with explicit host providers; hardware behavior is outside its scope.

| Trigger and correction | Proposal | Focused regression |
| --- | --- | --- |
| A supported zero-current setter before event activation returned success without an applying worker. It now refuses with EAGAIN before changing desired state. | [Current frontend](../../experiments/p105-current/FRONTEND.md) | [Inactive zero](../../tests/test-experimental-current-inactive-zero.py) |
| Failed child publication or delayed nested-device references could outlive partition storage. Constructor and parent-device references now retain it, and final cleanup runs under the MTD table mutex. | [H2FMI lifetime](../../experiments/p105-h2fmi-lifetime/EVIDENCE.md) | [Lifetime](../../tests/test-h2fmi-lifetime.py), [partitions](../../tests/test-h2fmi-partitions.py) |
| Gate/reset readiness disappeared after a completed controller operation. Restoring ready bits previously allowed the same context to resume. Such errors now remain latched. | [Controller](../../experiments/p105-usb-controller/EVIDENCE.md) | [Readiness](../../tests/test-usb-controller-readiness.py) |
| A cold-adopted peer could retain reset membership while a new empty caller lost a provider or epoch. The failure path now considers all16 slots. | [Reset client](../../experiments/p105-usb-reset/EVIDENCE.md) | [Adopted peer](../../tests/test-usb-reset-adopted-peer.py) |
| A ready gate could commit a clock membership without a local write. Fault retention now considers all16 masks, not only the write marker. | [Register backend](../../experiments/p105-usb-registers/EVIDENCE.md) | [No-write membership](../../tests/test-usb-register-no-write-retention.py) |

The optional MTD proposal also changes generic core/header/partition interfaces.
It needs a focused generic-MTD review, beyond the H2FMI driver review. Its
[checked installer](../../experiments/p105-h2fmi-lifetime/PRIVATE_APPLICATION.md)
accepts only an acknowledged private qualification tree and has no boot or
normal-image publication action. It is built-in-only and enables no flash writes.

## Source evidence and remaining integration

The [17 October charging checkpoint](p105-charging-checkpoint-20261017.md)
separates the completed Linux USB500 observation from offline REA coverage.
The [source VBUS getter tests](../../tests/test-source-vbus-getter.py) execute
the original measurement wrapper with mocked gate/raw ADC and refused PMU I/O;
they establish error propagation, not caller policy or physical cutoff.

[Native RSA evidence](../../experiments/p105-iap-native-policy/rsa-closure/REAL_RSA_PROFILE.md)
replaces synthetic public-key getters with actual native DER import and key-size
checks. [Signature-link cases](../../experiments/p105-iap-native-policy/rsa-closure/NATIVE_LINK.md)
run actual RSA modular exponentiation with a real host SHA256 provider. Certificate
objects, CF and heap are synthetic; full certificate parsing/path trust and
accessory identity remain unqualified. Mathematical success grants no power or
role permission. Original-input replays are separate from public-only tests.

See the [integration order](p105-feature-integration.md) for the still-required
physical ownership, ADC/IOP retirement, analog protection, NAND geometry/durability,
shared WLAN continuity and protected VBUS evidence. Passing offline tests is not
permission to activate a charger, MTD writer or host supply.

The initial device session uses a separately identified normal RAM package,
with its own source commit and artifact hashes. Research HEAD is not its image
version. No experimental fault fix in this table is silently added to that image.
