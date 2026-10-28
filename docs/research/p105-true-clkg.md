# True clkG factory/parent bridge, correcting the cpuI label

Pinned12H321 kernelcache db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c,
P105ADT5c346586116de1bb32606acb27cad3158bcaa8a1318a10ee9631de09884cfd72.
Offline fake providers only; no hardware or shared changes.

## Explicit correction

The earlier private native-clock-next decoder804b56d4/vptr804d45a8 is
**AppleARMPerformanceControllerFunctionCPUIdle**, selected by literal804b032c
bytes49757063 (`cpuI` in native FourCC order). Its true36 opcode controls remain
valid for that class, not clkG. Do not export their raw89→Unsupported result as
an S5LIO clock gate failure or as a reason to bypass a real clkG provider.

Actual clkG literal804b0328 bytes476b6c63 selects804b0276..28c. Constructed
vptr is804d4468 (original PIC804b0288/28a+8), class named
AppleARMPerformanceControllerFunctionClockGate by original metadata initializer.
Actual+38 init804b5441 and+3c call804b5435 differ from cpuI.

## Executed true bridge

`scripts/emulate-pmgr-clkg.py` runs actual factory804b01ac with source-bound property
words10000000/636c6b47. It allocates the actual typed object and retains its
constructed vptr. Init804b5440 runs **actual inherited804ab490**: generic
OSObject init/retains are mocked, while stores+8(parent),+c(OSData),+10(name)
are original instructions. Actual safe metacast call804b5460 stores the returned
Performance parent in+14. No anonymous vtable is substituted for the typed object.

Actual S5LIO cached wrapper80b8bb10 passes raw gate/state unchanged to this
object. Actual clkG804b5434 loads parent+14 and tailcalls its+3e0;
actual Performance804b1f6c calls CommandGate+self1b4 with action+self1b8.
The **actual startup block804afa0a..18** defaults that action to the same
actual object's+350 (804b3141). Fake CommandGate preserves that source-bound
owner/action/arguments and executes the original action; the complete original
native recursion/deferred/gate/accessor path then runs against fake PMGR RAM.

Nine supported raw87/89 requests PASS, including shared-parent sequence,
repeat89 and reverse release. Writes and counts match the already-independent
native physical oracle. Inherited OSObject-init refusal returns0 without a call.
A genuine factory-selected cpuI control with raw89 returnsUnsupported with no
PMGR writes, establishing exactly why the earlier class substitution was wrong.
Another negative proves typed clkG init returns1 even if safe metacast returns
null; the retained parent is0 and the later call is deliberately not attempted.
Thus an init success return alone is not proof of a valid parent instance.

All runs are bounded12000 original instructions. Synthetic identities for
IOFunction symbol and successful parent metacast are explicit inputs, ordinary
retains/allocators and CommandGate are fake, references/active masks and fake
PMGR acknowledgements are synthetic. No physical state or inherited ownership
is inferred from them. The source constructor and factory—not guessed slot
numbers—select the class and default callback.

## Registry association evidence and follow-up

P105 arm-io/function-clock_gate raw00000010476b6c63 has prefix00000010,
exactly pmgr/AAPL,phandle raw00000010. Original generic lookup804ab31c reads
that property's first native word and formats **IOFunctionParent%08lX** at
804ab380, with literal804ca341; thus the lookup key is
IOFunctionParent10000000. Registry parent registration API804ab1fc formats the
same key at804ab24e from a supplied provider's AAPL,phandle, then places it on
the supplied service at804ab274. This is a concrete static identity convention.

The [subsequent publisher/lookup proof](p105-clkg-registry.md) now executes the
actual retained-provider startup caller, marker publication, generic lookup and
subclass factory fallback. The fake matcher returns the same original
Performance owner; missing/wrong markers cannot fabricate a parent. The bridge
fixture on this page still supplies its parent directly, preserving its exact
scope and prior oracle. Asynchronous S5LIO cached publication, duplicate live
services, explicit Linux supplier binding and all-global-owner adoption remain
separate requirements. Native runtime admission stays denied.

A superclass-table pointer used in a PIC call can include its two negative C++
vtable header words, whereas a constructed object vptr skips them; do not compare
those offset numbers without checking the original constructor (+8). No global
clock-release, registered Linux clock provider, native5V, VBUS, charge or Linux
host claim follows from this source bridge.

## Public offline reproduction

Run `python3 tests/test-pmgr-clkg.py` or
`python3 scripts/emulate-pmgr-clkg.py --kernelcache <verified-cache>
--device-tree <verified-raw-ADT>`. Inputs must match the pinned hashes above.
No inputs are opened on module import; missing dependencies are explicit test
skips. The root independently repeated the frozen original-instruction group
and obtained byte-identical normalized JSON. The public oracle contains fake
RAM observations only. No firmware, device access or registered provider is
included.
