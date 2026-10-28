# P105 native clkG registry owner association — original pipeline

Pinned12H321 kernelcache SHA256
`db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c`
and P105 ADT `5c346586116de1bb32606acb27cad3158bcaa8a1318a10ee9631de09884cfd72`.
Original code executes only with fake RAM, registry, generic IOKit objects,
CommandGate, interrupt/locks and PMGR acknowledgements. No device operations.

## The actual publisher and retained instance

The original Performance constructor PIC80b8bc80/82+8 establishes vptr80b90938.
Actual superclass start block804af6ea..700 accepts the supplied PMGR provider,
runs its metacast and stores the returned instance in+self1ac at804af6fe.
The fixture executes this block; it does not simply handwrite that store.

At the end of the real S5LPerformance startup, caller80b8c0b8..c2 invokes
that constructed object's+3d8, resolving804b1d11. This callback loads
**the same retained PMGR provider** from+self1ac at804b1d28, keeps the same
Performance instance as argument1 and calls804ab1fc at804b1d2c. The fixture
executes the actual virtual startup block and full callback, with domaincount0
and mode540=0 explicitly synthetic. It does not replace the publisher with
a guessed slot or anonymous service.

The registration API's actual PIC804ab21e/222 resolves IAT804d0344 pointing
to kernel symbol `_gIODTPHandleKey` (804053f4). Apple defines this key as
`AAPL,phandle` in [XNU2782.40.9 IODeviceTreeSupport.cpp](https://github.com/apple-oss-distributions/xnu/blob/xnu-2782.40.9/iokit/Kernel/IODeviceTreeSupport.cpp).
The supplied fake PMGR provider retains the source-constructed ARMIODevice
vptr804d2bb8, and its generic property getter returns the actual PMGR phandle
bytes00000010. The helper formats IOFunctionParent10000000, creates an empty
OSData marker and calls setProperty on **that Performance instance** at804ab274.
After returning, callback804b1d30..3a registers the same service with flag2.

There was a private diagnostic error before this discovery: Capstone's branch
operand.imm for high addresses is signed. A branch target comparison must
normalize to32 bits; an earlier no-call scan omitted this and was invalid.
No absence claim survives into this evidence. The actual publisher call is
now executed and recorded, not inferred from an import.

## Actual lookup, subclass fallback, typed initialization and graph

Actual generic creator804ab31c reads arm-io/function-clock_gate's first word,
formats the same key and uses **IOService::propertyMatching** with the empty
OSData marker, then **IOService::waitForService**. These import names are
resolved from their real IAT targets and the pinned image's LC_SYMTAB;
Thumb bit is masked when comparing symbol addresses. Apple documents the
matching/wait interface in [IOService.h](https://github.com/apple-oss-distributions/xnu/blob/xnu-2782.40.9/iokit/IOKit/IOService.h).
The registry itself is an explicit fake dictionary, not an executed real IOKit
scheduler or a live-boot observation. Its successful result is precisely the
instance on which the original publisher placed the marker.

The creator then invokes **that instance's actual+1e8 override80b8c168**.
The true clkG FourCC is not a subclass special power case, so its original
fallback reaches superclass factory804b01ac. Actual factory selects
ClockGate vptr804d4468; actual inherited804ab490 and typed804b5440 initialization
retain and metacast the same matched parent. The cached S5LIO wrapper and
true clkG call then reach original Performance+3e0, source-chosen CommandGate
+350, recursion/deferred operations and actual PMGR accessors. Four native
87/89 requests reproduce the independent physical oracle's order/counts.
Source stage list is independently asserted:
80b8c0b8 →804b1d2c →80b8c168 →804b01ac.

Synthetic IOFunction selector, requested function-name symbol and phandle-key
symbol use distinct identities. Their initialization/interning/retaining is
explicitly mocked; original typed object stores, factory branch, parent cast,
registry key formatting, publisher and caller are original instructions.
The actual AAPL,phandle/clock function prefix agreement comes from pinned ADT,
not symbol-address equality or a hand-selected callback.

## Negative controls and exact scope

Three independent inputs—wrong PMGR phandle, missing publisher, missing phandle
property—cannot supply a matching parent. The fake waitForService stops with an
explicit no-match diagnostic, never returns a fabricated service. This bounds
the offline control; it does not claim original IOKit's internal wait is bounded.
The source publisher ignores the registration helper's missing-property error
and can still return0; startup return0/1 alone is not association evidence.
No typed clock object or PMGR request is executed for these blocked controls.

The public entry point adds optional `--registry` to emulate-pmgr-clkg.py,
with a separate helper emulate-pmgr-registry.py. Default is false and the old
nine requests and all existing negative JSON are **exactly preserved**. The
root independently rehearsed the frozen candidate and repeated its exact oracle. Normalized
oracle contains semantic data only, no firmware binary or private paths. The
emulator requires exact firmware/ADT hashes, limits56/128/table5104 and bounded
12000-instruction calls; supplied provider/regmap addresses are synthetic RAM.

This closes the **source association route** and corrects the earlier cpuI/clkG
confusion. It does not execute full boot/startup, all domain modes, asynchronous
S5LIO cached publication, a real registry with duplicate services or inherited
clock-owner adoption. Linux's explicit provider binding and no-bypass/global
owner admission still need implementation and real handoff verification. Current
native broker guard remains denied. No charging, native5V, VBUS, accessory auth,
role publication, hardware host or physical WLAN continuity is claimed.

## Public offline reproduction

Run `python3 tests/test-pmgr-registry.py` or
`python3 scripts/emulate-pmgr-clkg.py --registry` with separately obtained,
exactly pinned official inputs. The existing default nine-request proof and
negative controls remain unchanged. Both entry modes emulate fake services
only and never contact a device or a real IOKit registry.
