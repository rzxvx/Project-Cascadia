# Original P105 shared PHY ownership and controller lifecycle

Research date: 2026-10-11. Firmware source: the locally verified official
iPad2,5 8.4.1/12H321 cache, SHA-256
`db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c`.
No hardware access, new firmware download, active routing or runtime patch is
part of this group. The published tool contains no firmware binary bytes.

## Shared client selection is an actual firmware mechanism

The actual `AppleS5L8930XUSBPhy` instance vtable `0x80cad0a8` has:

| Instance slot | Verified target | Scope in this investigation |
|---|---|---|
| `+0x344` | `0x80cabea9` | Staged shared PHY power-level changes; recorded rather than executed by this emulator |
| `+0x348` | `0x80cac14d` | Device-mode enable helper; physical `+0x1c` bit 1 operation is established separately |
| `+0x34c` | `0x80cac201` | Host-mode helper; host/device tuning and reset are established separately |
| `+0x350` | `0x80861c5d` | Actual shared-client update and maximum-level selector, executed here |

The selector acquires its lock through import stub `0x80864e40`, snapshots the
five collection counts at object `+0x64/+0x68/+0x6c/+0x70/+0x74`, and calls
the actual update helper `0x80861db0`. That helper removes the requesting
client from every previous collection and inserts it into the collections
selected by the five low mask bits. This is client identity membership, not
an unbalanced increment for repeated requests from the same client.

The selector then scans all five collections. The highest occupied index
controls the requested shared power level. It additionally compares the old
and new counts of collection 2 and 3: the first client enables the corresponding
host/device helper, and the last removal disables it. It performs global
power-level zero only when no collection above index zero remains occupied.
Unlock is through `0x80864e50`; the function returns zero after the audited
successful collection-provider path. It is not an electrical fault-report API.

The fixture executes this real selection and update code, including its NEON
stack-scratch initialization and stack-canary checks. Synthetic collection
methods implement count, indexed get/remove and insertion. Lock imports are
mocked. The power/mode callbacks are recorded and **never executed**, so a
recorded `power(4)` is not a measured or applied hardware state.

## Independent expected callback evidence

| Synthetic requests in order | Recorded callback consequence |
|---|---|
| Host client A mask 4, host client B mask 4, remove A, remove B | Power 2/host on initially; removing A retains host; removing B calls host off then power 0 |
| Same client host mask 4 twice | One membership remains; second call does not repeat host on |
| Move same client from host mask 4 to device mask 8 | Power 3, then host off and device on; old membership is removed |
| Level-4 client A mask 16, host B mask 4, remove B | Power stays 4, even though host off is requested; no global power 0 until A also leaves |
| Empty request, or only index-zero client mask 1 | Power 0 is requested; no host/device enable |

These are software policy proofs with synthetic identities. The mask bits
are shared-PHY client-state requests, not the ADT `clock-mask` namespace.
Mask 4 here selects collection 2; it must not be confused with the P105 WLAN
HSIC-port bitmask 4 or a PMGR gate mask with the same numeral.

## EHCI startup, shutdown and temporary ownership

Actual EHCI helpers are stronger than their earlier shorthand description
as clock-request helpers:

- Instance `+0x474` targets `0x80c907cc`. It locks object `+0x1580`, checks
  a counter at `+0x157c`, calls retained PHY `+0x348(1)` only for zero-to-one,
  increments that counter, and unlocks.
- Instance `+0x478` targets `0x80c9080c`. It locks, decrements that counter,
  calls PHY `+0x348(0)` only when the count reaches zero, and unlocks.
- The lock import stubs `0x80c9196c/0x80c9197c` resolve imports
  `0x80c92008/0x80c9200c` to actual kernel symbols `_lck_mtx_lock` at
  `0x800ace4c` and `_lck_mtx_unlock` at `0x800acfdc`.

ARM EHCI startup at `0x80c902cc` retains its provider PHY at `+0x1574` and
allocates its lock at `+0x1580`. The observed successful/failure ordering is:

1. Call instance `+0x474` to hold the temporary enable.
2. Call PHY `+0x34c(1)` at `0x80c90444..0x80c9044a`.
3. Call inherited startup through static base slot `+0x170`.
4. Call `+0x478` to drop the temporary enable.
5. If inherited startup returned false, call own cleanup slot `+0x16c` and
   return false. This is observed cleanup dispatch, not a proof that every
   partially acquired downstream resource is restored.

Shutdown wrapper `0x80c904b4` similarly calls `+0x474`, inherited stop via
static slot `+0x174`, PHY `+0x34c(0)` if retained, then `+0x478`. Cleanup at
`0x80c904fc` checks the counter under the lock before freeing it and releasing
the retained PHY. It does not demonstrate an external VBUS-off callback.

The S5L **EHCI** subclass transition wrappers around inherited controller
transitions have additional order constraints. Actual 12H321 prelink metadata
places these addresses in `com.apple.driver.AppleS5L8930XUSBEHCI` at
`0x80c96000..0x80c99000`, with IOKit class `AppleS5L8930XUSBEHCI` matching
`usb-synopsys_ehci,s5l8930x`. "Synopsys" here refers to that EHCI subclass,
not the DWC2/OTG device controller. `AppleSynopsysOTGDevice` is a separate
prelinked image at `0x80bb4000..0x80bc3000`, matching
`usb-device,s5l8900x`. These EHCI transition orders are not an OTG recipe:

| Wrapper/input | Verified call order | Return/error boundary |
|---|---|---|
| `0x80c96b5c`, argument 0 | `+0x474`, inherited `+0x358(0)`, `+0x478`, PHY `+0x34c(0)` | Keeps and returns the inherited result; the PHY call occurs even if that result is nonzero |
| Same, argument 1 | Inherited `+0x358(1)`, then `+0x478` | Returns inherited result; no extra host-mode call in this branch |
| `0x80c96bd4`, argument 1 | PHY `+0x34c(1)`, `+0x474`, inherited `+0x35c(1)`, `+0x478` | PHY result is not checked; no local rollback follows an inherited error |
| Same, argument 2 | `+0x474`, tail-dispatch inherited `+0x35c(2)` | No local `+0x478`; inherited ownership continuation still needs tracing |

PIC pairs `0x80c96b70/0x80c96b74`, `0x80c96ba8/0x80c96bac`,
`0x80c96be8/0x80c96bec` and `0x80c96c1e/0x80c96c22` resolve import
`0x80c97024` to static dispatch table `0x80c92a40`. Its `+0x358` target is
`0x80841979` and `+0x35c` target is `0x80c916f5`. These are the actual imported
table slots; adding another presumed vtable prefix would select different
methods. Their complete inherited asynchronous/error lifecycle remains open.

## What this means for external Port1 and WLAN

Apple's path distinguishes controller lifetime, removable-port clock clients,
temporary PHY ownership and global power state. The existing
[EHCI investigation](p105-usb-ehci.md) already maps external type-2 Port1
separately from HSIC WLAN Port3; OHCI startup retains the same PHY and requests
both helpers. The shared client selector now explains a concrete protection
mechanism: removal of one client need not power down the shared block while
another retains a higher state.

The current Linux driver has two generic PHY instances with distinct core
counts/mutexes but one shared MMIO object. It does not reproduce this complete
shared power-level/client selection or Apple controller transition policy.
Neither this firmware selector nor the DWC2 host build is a ready Linux
external-port initialization recipe. Controller-specific clock masks, full
OHCI shutdown/error continuation, asynchronous completion, physical mux
routing, PHY resets and preserved WLAN enumeration still need evidence.
VBUS switch/boost/current protection and connector wiring are independent
open questions; collection membership proves none of them.

## Verification and remaining limits

`tests/test_usb_phy_clients.py` has six passing tests with no skips in the
existing network-disabled image. They assert independent expected transitions
for two clients, duplicate updates, migration, a retained level-4 client,
index-zero/empty state, and refused malformed/unpinned input. The tool checks
the cache hash and the actual instance callback targets before executing.
Its synthetic request count is bounded and the CPU has an instruction budget.

Run `python3 scripts/emulate-usb-phy-clients.py build/firmware/kernelcache.12H321.macho`
to obtain the software evidence table. This requires the same local verified
firmware and Unicorn as the other offline emulators. Nothing is fetched or
transferred to a device. Collection allocation failure, callback side effects,
the nested real power/mode implementation and simultaneous consumers are
not modeled. Source disassembly supports the EHCI orders above; the client
emulator does not execute complete EHCI/OHCI startup/shutdown.
