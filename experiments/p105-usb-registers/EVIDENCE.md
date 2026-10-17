# Isolated P105 register backend: a concrete partial implementation

This is **source only, default off, no DT/driver registration**. It is not a
working external USB host driver. Normal PHY0, gadget console/network and
existing WLAN driver sources/configuration remain untouched. No hardware was
accessed. These functions contain actual Linux Regmap operations; they were
executed only with fake Regmap/mutex/epoch providers and compiled against real
Linux6.12 ARM headers. They do not map any physical address themselves.

The earlier external-client component establishes checked, owned port state
boundaries. This group implements two narrower backend primitives: idempotent
shared clock-mask membership and a firmware-derived PHY tuning/reset sequence.
It deliberately does not expose `phy_client_state(5)` as success: shared PHY
power-up and reset-client membership are still missing. No callback adapter
pretends that these primitives complete an external host transition.

## Proven parts and exact limits

Source firmware is pinned 12H321 cache SHA256
`db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c`.
The actual `AppleS5L8930XUSBArbitrator` clock entry `0x80b3a334`, base membership
entry `0x80864648` and collection updater `0x808647f0` execute in the new clock
oracle. Nineteen collection providers and lock imports are synthetic; actual
USB-complex RMW instructions operate on ordinary fake RAM. The low-mask
powergate provider records state/index rather than switching hardware.

The actual code looks up `usb_ctl`; the synthetic property supplies the
actual P105 value `0x64`. It forces mask `0x10000` when input intersects
`0x60070`, then opens USB-complex `+0` clock-off bits `0x40`/`0x20` before
low gate updates and closes them after. Ownership is identity membership:
repeating or removing one external owner while the EHCI/WLAN owner remains
produces no gate or clock-off writes. The original fixture records gate-index4
callbacks; the P105 gate table gives index4→ID87→register-index0, so the C
backend intentionally performs **no PMGR I/O** for that logical membership.
Unsupported masks, including `0x40000` or low bits beyond P105's five gates,
are refused instead of generalizing to other hardware.

The C gate primitive uses audited P105 offsets `0x1084/1088/1090/1094`, reads
before writing, requests target bits3:0 with bit8 cleared and polls the observed
bits7:4 with a bounded 1000-iteration timeout. This is the actual
`clock_gate_switch` primitive `0x80b8de38` shape, not a complete implementation
of the firmware PerformanceController/managed-clock graph. The real
AppleARMIODevice provider `0x804ae034` dispatches platform `+0x350`; the complete actual platform instance and parent graph remain unresolved.
Consequently native final clock releases require the separately asserted
`gate_release_verified` flag. It defaults false: a last owner release returns
`-EOPNOTSUPP` **before I/O**, retaining the membership. A release which leaves
another owner covering every bit can proceed without hardware change. Tests
also exercise an explicitly enabled synthetic target0 release, but this is
not proof that the entire native parent/clock-off policy is complete.

`complete_clock_ownership` defaults false. The future owner must migrate or
account for **all** gadget, EHCI/WLAN, external-port and PHY clients before
using these shared clock masks. Reading an enabled register is not an ownership
record; none of these functions silently imports pre-existing consumers.
Existing generic PHY0/PHY1 independent counts cannot satisfy that contract.
No `clk_prepare_enable()` can be substituted without an audited A5 provider
and membership mapping. That provider/parent-clock integration remains open.

The actual host helper `0x80cac200` plus its real read/write/RMW accessors runs
in the second oracle. Host54f/6df3 or device549/2ff3 is written to PHY
`+0x40/+0x44`, reset `+8` bit0 is asserted, followed at original powerlevel≥2
by 20µs, reset release and 1000µs. With HSIC=true, `+64` is not touched. With
HSIC=false, bit1 is set/cleared for host/device, preserving other bits.
Powerlevel1 retains asserted reset; level0 performs no operations. The
candidate refuses levels<2 before any I/O rather than reporting a completed
transition while reset remains asserted.

The original helper uses a temporary shared-power membership16 around this
work. That nested power provider is recorded, not executed by the oracle.
The C primitive therefore requires an externally acquired **real power-level
lease** plus quiescence of every affected controller. Boolean inputs are
explicit caller-contract attestations, **not hardware discovery or proof**.
No Linux adapter currently establishes them. The real power transition at
`0x80cabea8` and reset-client add/remove `0x80cac59c/448` require further checked
integration before this primitive can become a complete backend. In particular
we do not infer a powerlevel from current Linux generic-PHY counts or iBoot
register snapshots, and do not claim WLAN survives a reset without tests.

## Error, concurrency and connection lifetime

The backend has one shared mutex, fixed16 owner slots, a required epoch
validator and flags denying use by default. All register operations check
connection epoch before I/O and immediately after each write; poll iterations
and final membership commits also recheck. Fake stale epochs cover every I/O
position and the final commit. The outer role-owner workqueue/connector lock
must serialize quiesce, policy and epoch changes across all consumers; this
mutex does not implement the missing physical connector lifetime.

Membership changes commit only after all checked operations succeed. A failed
write may have touched hardware. Once touched or holding any membership, any error latches its original
negative error, preserves the old owner masks and records the pending target.
Further calls refuse I/O; no automatic rollback, reset, force success or
substitution of a new connector epoch occurs. Recovery/disposition of that
partial hardware state requires a separate reviewed owner operation and is
not implemented here. Reinit returns `-EALREADY` without erasing retained
membership or fault. Fresh zeroed storage is required.

This primitive returns only completion of its own operations. It does not
claim voltage, current permission, authentication, controller publication,
mux route completion or device enumeration. A failed provider/read before any hardware touch and with no retained
membership leaves no phantom owner commit; late/retained-state errors latch.

## First concrete external-host path

Prioritize EHCI external Port1 and an enumerating **high-speed USB2 storage
device**, followed by a high-speed USB2 hub with a Transaction Translator for
a full-/low-speed keyboard. The pinned Linux6.12
[EHCI documentation](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/Documentation/usb/ehci.rst)
states that directly attached USB1.1 devices hand off to a companion, while
USB1.1 devices behind a USB2 hub use EHCI split transactions through its TT.
A hub which itself falls back to full speed does not solve the companion
requirement. This is protocol architecture, not measured adapter operation.
OHCI0 remains relevant later for directly attached keyboard support.

Actual P105 ADT/firmware instantiate EHCI external removable Port1, separately
from HSIC WLAN Port3. The existing DWC2 host compiler profile is not this EHCI
route. Do not translate EHCI PORTSC or host tuning into DWC2 register writes.

The prioritized power architecture is the **externally powered Apple Lightning
to USB3 Camera Adapter** path already documented from manufacturer sources in
`phy-role-setup/external-supply-primary.md`. Apple's iOS≥9.3 requirements and
power input capability do not prove Linux ID/authentication or MUX behavior.
CarPlay EP0 requests51/53 are separate; no camera ID has been established from
those requests. Native accessory3.3V LDO/loadswitch is not claimed as USB5V.
No external adapter was connected or purchased and no backfeed workaround is
recommended. Actual VBUS source/protection, port-role owner, controller
quiesce, power-level provider and physical validation are still blockers.

## Validation

13 offline unittest groups PASS, no skips, in existing network-disabled image.
They compare five complete owner scenarios and four host/HSIC combinations
against actual pinned firmware instructions, independently assert expected
preserved32-bit fields, and cover default denial, missing provider, malformed
masks/owner, unverified release, every Regmap failure/stale epoch, final commit,
timeout and reinit. Synthetic PMGR acknowledges requests immediately except
in the explicit timeout case; no electrical behavior is modeled.

Candidate source compiles as an ARM object against the real Linux6.12 generated
headers with ARMv7 kernel preprocessor defines and build-equivalent warning
exclusions for header sign/type-limit checks, `-Wall -Wextra -Werror`; zero
warnings. An initial standalone compile missing `__LINUX_ARM_ARCH__=7` failed
in kernel headers, corrected by matching the actual saved kernel compile flags.
No full kernel/config/DT change occurred. The object and fake `.so` are private
build artifacts, not part of a proposed source patch.

Portable tests use the normalized semantic oracle rather than reading firmware.
Run `python3 tests/test-experimental-usb-registers.py`. Fakes replace Regmap,
mutex and delays; actual Linux regmap code is compiled unchanged. This directory
is not included by normal Kconfig, Makefiles, DT or build wrappers.

## Retained-ownership validation correction

The original candidate rejected an epoch-validation failure at the next API
entry without latching it, even after successful writes and retained masks.
Restoring the fake validator could silently reuse that uncertain context.
Entry validation now uses the same fault policy as transaction failures: once
hardware was touched or membership was retained, validator errors (including a vanished validator) latch
and preserve masks; no additional I/O occurs and a later good callback cannot
clear the fault. Before any touch and without retained membership, a rejected admission remains
retryable.
A malformed caller epoch is conservatively rejected/latches if state was retained;
this component has no independently bound per-owner epoch object that could
prove an innocent stale argument. Such an adapter remains necessary.

Initialization also rejects NULL and kernel ERR_PTR regmap handles before
publishing initialized state. Six handle cases, retained-error/positive-error
normalization, missing-validator and pre-touch retry cases execute actual C.
All16 register test groups pass with UBSan. The unchanged old entry-validation
function fails the same retained-state regression test; its failure is retained
privately as a negative control. No runtime or DT hook was introduced.


## Successful membership without a local register write

A supported low-gate acquisition may find target/ack already ready. The actual
API then commits its identity mask after checked reads without a write, leaving
touched=false. Fault retention now scans all16 masks; retained ownership does
not depend on whether this backend wrote the register. Restoring a lost validator
or attachment epoch cannot reuse this context. Positive validator errors retain
the normalized EIO; empty untouched contexts keep initial admission/retry behavior.

An independent read-only source review confirms this ordinary API path; no
manual bitmap adoption is needed. Forty-eight actual historical controls and54
fixed/cold sanitizer cases cover four low gates, owner slots1/15, missing/error/
positive validators, membership and tuning callers, zero-I/O fault retention and
empty recovery. The previous complete source is reconstructed from its pinned
actual fault function and matches its historical hash. Registers/epochs and
ready gate observations are explicit synthetic providers. No physical gate,
USB, PHY, WLAN, VBUS, iPad or running kernel is tested. Repeat:

    python3 tests/test-usb-register-no-write-retention.py
