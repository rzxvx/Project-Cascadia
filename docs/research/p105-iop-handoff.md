# P105 IOP boot ownership and quiescence: original-source evidence

Reviewed source audit, 2026-10-12. No device, NAND, real interrupt/MMIO or system services were accessed.
Pinned firmware files were read and selected instructions emulated offline. All physical-looking addresses below were
used only as offsets into process-local Unicorn Fake RAM. No backend/driver,
reset, gate transition, bootloader patch or write operation is enabled here.
Existing completion/engine/planner code remains unchanged and default-off.

## What is now concrete

The native iOS AP path is `EmbeddedIOP` control-message sleep, then `AppleA5IOP`
processor stop. Device reset is a separate platform function. Neither its
success response, the software running flag nor CPU stop proves CDMA quiescence.
Actual native clock metadata and original execution establish that IOP and CDMA
are separate resources; releasing the former leaves the latter active in our
explicitly synthetic controller-owner scenario.

`verify-handoff.py` checks60 exact original instructions/literals, real symbol
bindings, two independently pinned ADTs and both12H321 boot-image identities.
`emulate-handoff-leaves.py` passes45 original AP/IOP leaf/branch controls.
`exercise-pmgr-iop.py` passes12 original full native Performance requests using a
private byte-identical copy of the USB agent's reviewed emulator. All are bounded
and hardware-free. These tests produce evidence, not physical receipts.

## Input provenance and boundaries

| Source | SHA-256 |
| --- | --- |
| 12H321 kernelcache Mach-O | db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c |
| Embedded12H321 IOP at file offseta69000, size24000 | 6dfa5dcc6f8cb38be41e48c0edc543b2014a026f7909c35c9ef050ffd23c22d4 |
| Official12H321 p105 ADT | 5c346586116de1bb32606acb27cad3158bcaa8a1318a10ee9631de09884cfd72 |
| Official13G36 p105 ADT | e8277fd827ba250de3f46567cb1c910d4e1c80633d666510a7780e00f4ad4b59 |
| Original12H321 iBSS, 77,824 bytes | 67d8f8d6fa82e220f436233e89ba0d411b948d5d5aeee1289e2a0d268b03bfb5 |
| Original12H321 iBEC, 282,624 bytes | 30c040052c444222edc8646316f95a355a9e338d6aadc7168f8a7acc7c4578ec |
| Existing local12H321 iBEC.autogo | e987b101800bedf03c086aa55854ff594fcce1b0f55ea1424f3ccf180a40abce |

12H321 ADT provenance is in
`build/usb-host-research/adt-evidence-v4/adt-usb-power.json` (official IPSW member,
whole ZIP and image hashes);13G36 provenance remains in
`references/p105-ios935-official/plaintext-provenance.json`. No firmware bytes,
keys, live memory or device identity records are included in this evidence group.
ADT data are board defaults, not measurements of our unit or current register
state. AP method/IOP instruction execution here is12H321 only;13G36 cross-checks
only the specified ADT resource namespace, not its changed native driver ABI.

## AP/IOP roles and concrete halt sequence

The actual AP kexts include `AppleA5IOP` at80f41000, `EmbeddedIOP` at80dde000,
`IOSlaveProcessor` at804f0000, `AppleARMPlatform` at804a9000 and
`AppleS5L8940X` at80b8a000. Method/table names are bound by original constructor,
vtable and diagnostic references, not by transplanting guessed struct layouts.
The AppleA5IOP object's actual vtable+344 points to80f41a59 (Thumb stopCPU).

`EmbeddedIOP::_iopSleepGated` at80de0338:

1. Checks its software active byte+6c.
2. Sends control word736c6570 through original80de03c0.
3. After a successful control response, clears that software active byte and
   disables the two endpoint/event objects.
4. Calls its imported `IOSleep(100)`.
5. Calls the slave processor's stopCPU virtual method and panics on nonzero.

The API units are documented in Apple's contemporary public
[IOLib.h](https://github.com/apple-oss-distributions/xnu/blob/xnu-2782.1.97/iokit/IOKit/IOLib.h),
which specifies milliseconds for IOSleep and microseconds for IODelay. This
public header is an ABI reference, not a claim that its tag is our exact kernel
source revision. The actual pinned kernel SYMTAB defines `_IOSleep` at
802e4658, and the original Thumb import stub80de19e4 loads its pointer at80de3008.
The control-message scheduling/endpoint APIs remain explicit mocks/not executed
in the leaf fixture; no full iOS workloop or actual AP–IOP handshake is claimed.

`AppleA5IOP::stopCPU` at80f41a58:

1. Takes the object's lock and checks software running byte+78.
2. If running, disables provider IRQ index0.
3. Writes zero to mapped IOP register offset0.
4. Calls provider power-gate interface+358 with false and clock-gate interface+354
   with false, each at index0.
5. Clears its software running byte and unlocks, returning0.

The mapped IOP register resource is3f300000/1000 in both checked ADTs. Index0 is a
provider interface index, not a raw AIC channel number. For the matching IOP
node, its only ADT interrupt is2; no CDMA IRQ55..58 or FMI IRQ33/34 is disabled
by this leaf's own register code. Dynamic registry/provider association remains
a backend obligation, even though personalities/resources match statically.

Original stopCPU does **not test the return codes** of its provider IRQ, power
or clock callbacks. Sixteen original executions cover active/inactive and all
combinations of three injected provider failures: an active leaf still returns0
and clears its software flag. Thus that return cannot be adopted as a checked
Linux halt receipt without independent effects/error handling. The fixture
models the original control write; it does not assert its electrical meaning,
processor halt acknowledgement or absence of future bus-master memory access.
An already-inactive leaf returns original IOReturne00002d7 and performs no core
write/gate action, rather than checking hardware state.

The platform-quiesce assertion prefix at80f418cc binds its imported key through
80f42054 to actual `_gIOPlatformQuiesceActionKey` at80405580. It panics if that key
matches and software running byte+78 is nonzero. Eight original prefix controls
vary the key, software byte and a synthetic core-control register independently:
it never reads the register or CDMA, and a zero software byte passes this
assertion. The whole virtual method after that prefix was not emulated. Passing
that assertion is not a hardware quiescence witness.

## IOP success response precedes the sleep callbacks

The actual IOP control branch at9ac compares736c6570. At9b6 it clears the request
status, at9bc it sends the response throughaf6c, and at9c6 it commits the ring.
Only afterward does it iterate the original module table13378..13380 and enter
CPU sleep throughaf64→20a0. Three original branch executions cover zero/one/two
synthetic FMI contexts while preserving that actual table and callbacks.

The two actual table entries reference callback0ac8 (message/doorbell path) and
1540 (FMI contexts). The first masks IRQ3 in the original AIC routine. The second
calls3050/3070 for sleep/wake; both bodies perform **diagnostic printing only**,
with no FMI/MMIO or CDMA shutdown. Our real-callback execution accepts only the
one AIC mask write and reaches the CPU-sleep entry after the response. CDMA
addresses are not mapped; an unexpected device access would fail the fixture.
The CPU sleep/cache maintenance body20a0 itself is not emulated here.

Therefore the success reply is an earlier milestone than those callbacks or
CPU sleep. It cannot stand in for callback drain, outstanding CDMA completion,
retirement, NAND status, reset or any durable/quiesced receipt. No software wait
or arbitrary grace period creates missing hardware evidence.

## Four different namespaces: clocks, gates, reset and boot helper IDs

Both actual44-byte ADTs agree on these rows:

| Resource | Logical clock ID | Gate index/address | Direct clock index/address | Parents |
| --- | --- | --- | --- | --- |
| IOP | 86 | 31 /3f10107c | 32 /3f1000b8 | 24/25/26/27 |
| CDMA | 85 | 30 /3f101078 | none | none |
| FMI0/FMI0-BCH/FMI1/FMI1-BCH | 59/60/61/62 | 49/50/51/52 /3f1010c4..10d0 | none | 37 |

Direct-clock arithmetic is PMGR+38+4*index; gate arithmetic is PMGR+1000+4*index.
IOP's managed-clock parents add direct indices11/12/13/14 and shared global
PREDIV closures. These shared parents must not be released from a local NAND
reservation without adopting/retaining their real global owners.

IOP `function-device_reset` is the separate tuple
`{provider phandle10000000, selector41525354, resetIndex22}`. The selector's
actual platform decoder selects `AppleARMPerformanceControllerFunctionAssertReset`.
Its class dispatches via Performance+410→80b8cf30, then the registered workloop
callback+46c→80b8d154. That leaf reads PMGR+1000+4*resetIndex, sets bit31, calls
`IODelay(1)`, then restores the **original word**. Actual import pointer80b90000
binds `_IODelay` at802e46cc, so the delay is1µs. Eighteen real reset-leaf controls
cover indices0/22/47/49/87/88 and initial words, including initial bit31 already
set. Index88 does nothing. With bit31 already set there is no proven rising
pulse. No reset completion, clock handoff or DMA readback occurs in this leaf.

For index22 the emitted register is3f101058. This is **not** IOP clock ID86 or
IOP gateIndex31. It is also not proof that an actual reset completed.

The original boot helpers use yet another numerical interface:

- iBSS vector+20 identifies base34000000; its gate helper is image+8dd0.
- iBEC vector+20 identifies base9ff00000; its gate helper is image+1f1ec.
- Both calculate register3f101008+4*WMR-id, bounded above by3f101140 (ID78),
  change target low nibble and poll target versus acknowledged high nibble.
- The iBEC bulk helper at1f440 iterates IDs2..78. Its reset helper at1eb9c has
  separate accepted-ID branches and uses the same WMR register base.

Numerically WMR29 reaches IOP gate register107c, WMR28 reaches CDMA gate1078,
and WMR20 reaches the separate ARST22 register1058. These equal addresses do not
make ADT logical IDs interchangeable with WMR arguments. In particular passing
logical86 to that boot gate helper is not an IOP transition. This group verifies
the helper instructions, **not the complete iBSS/iBEC startup caller graph** or
whether any of these operations ran after a particular warm kDFU entry.

## Original native clock controls, with honest ownership limits

With the USB agent's existing physical emulator privately copied unchanged,
12 actual native Performance+350 requests exercised IOP/CDMA independently and
together. IOP86on writes direct clocks48/4c/50/54/58/5c/64/68/6c/70/b8 then gate107c;
off writes gate107c then the inverse clock order. CDMA85on/off writes only1078.
Repeated86on/off is idempotent at native root active bits, not per-owner counts.

In86on→85on→86off→85off, CDMA85 remains active through86off, and no1078 write
occurs at that step. This concretely separates the IOP clock release from CDMA
fencing; the NAND channels share a CDMA core with other peripherals, so globally
releasing that core would also require all its real owners.

These executions use actual ADT rows/min56/max128 and original platform methods,
but **synthetic zero refs/active/foreign masks, null observer530, domain flag1e9=0,
Fake target/ack and zero policy bitmap**. Native off target4 is not assigned an
invented electrical/no-memory-access meaning. Recursion_flags0 in these runs
is not a complete policy/inherited-state qualification. No physical owner
association or counter/DMA drain has been measured. The copied emulator and
outputs are pinned separately in CHECKPOINT.json; its shared original is intact.

## Current Cascadia handoff: what it actually promises

`scripts/firmware/patch-ibec-autogo.py` SHA256
`efc5f94698dbf43ab76590cee00128e74b713669c209f0e0b98d75ac29898f94`
adds the Recovery EOF trampoline: cache flush, then BLX to the uploaded payload.
It does not invoke the ordinary iOS EmbeddedIOP sleep path or provide an IOP/
CDMA epoch-fence receipt. The reviewed payload assembly masks AP interrupts,
flushes AP/L2 caches and disables AP MMU/caches before entering Linux:

- `pongo/staging-loader.S`, SHA2566b9026420323c21f15013aadea71010cc6d587c073a214597df92501d418f18f.
- `pongo/linux_boot_start.S`, SHA256a799afeb06f8172e1405b191ce9cf1e9af1f2ff170c5494f7965e6c13f60b6d8.

Those specific payload paths have no IOP/CDMA-master handoff procedure. AP IRQ
mask/cache teardown does not establish another processor's state. Conversely,
we do **not** claim stock ROM/iBSS/iBEC leave IOP running: their entire reset/
initialization graph and the actual inherited warm/cold state were not proven.
The continued existence of iOS data says nothing about whether its IOP remains
active. Existing read-only PIO success on upstream hardware is not a lease proof
for our unit or a future concurrent DMA/writable backend.

## Minimal future backend contract now narrowed

A future unbound adapter has concrete source addresses/interfaces to investigate,
but must still establish, before accepting its first DMA lease:

1. Block new producers and adopt every actual controller/channel/global-clock
   owner; retain buffers owned by any old generation until it is fenced.
2. Complete a checked IOP halt/reset handoff with propagated provider failures
   and genuine readback/acknowledgement, not just the original software byte or
   early control reply. Preserve any shared parent/core ownership.
3. Independently stop/quiesce each owned CDMA channel and fence descriptor/FIFO
   access and late IRQ/callback work. An IOP CPU transition cannot replace this.
4. Adopt Linux IRQ-domain routing only afterward, with generation-bound transfer
   identity, barriers and bounded timeout/error handling.
5. Produce the completion module's genuine external no-future-memory-access and
   drained-IRQ receipts. Neither gate target/ack nor status0 is NAND durability.

No newly discovered instruction makes these hardware receipts true. The audit
closes concrete namespace and caller-order gaps and identifies why several
apparently convenient shortcuts are insufficient; it leaves the writable driver
and destructive storage layout disabled.

## Independent local repeat

The root independently repeated the frozen60 exact source/literal checks,
45 original leaf/branch controls and12 native PMGR requests under the bounded,
network-disabled build image. Normalized facts/traces match byte-for-byte.
Private record: `logs/night/iop-handoff-root-repeat.log`; no firmware or physical
receipt is distributed by this document.

The [native IRQ retirement audit](p105-cdma-irq-retirement.md) independently narrows the callback-drain requirement: a software-masked original registration still invokes a handler for a previously supplied event, and a captured callback/token survives later synthetic table replacement. Those software controls do not model physical pending/critical-section races. Checked cancellation accordingly does not manufacture a drained-callback receipt.
