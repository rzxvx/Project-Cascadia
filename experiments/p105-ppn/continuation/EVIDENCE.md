# Remaining inherited IOP/CDMA stop qualification: actual continuation audit

Source-only follow-up, 2026-10-12. This closes the previously unexecuted
continuation after AppleA5IOP's platform-quiesce software-byte assertion. It does
not discover a new hardware reset/stop postcondition or enable a backend.

## Avoided duplicates

Before this audit, existing evidence already qualifies:

* original iBSS/iBEC entry BSS clearing, main/early/CDMA clock initialization;
* selective boot reset accepted-ID matrix and AP's distinct ARST pulse;
* actual IOP sleep response preceding module callbacks/CPU sleep;
* AP stopCPU's control write and ignored provider IRQ/gate failures;
* native CDMA CONTROL2/poll/global-disable cancel and late IRQ registration;
* FMI client accounting and Linux-owned AP handler synchronization.

These established paths are not rerun or reimplemented here. None provides an
owned, no-future-memory-access receipt for inherited CDMA transactions. This
follow-up creates no additional abstract gate, receipt, C callback or writer.

## New exact local source bindings

| Scope | 12H321 | 13G36 |
| --- | --- | --- |
| AppleA5IOP platform method | 80f418cc | 811b48a8 |
| actual continuation parent pointer | 80f4204c | 811b504c |
| original parent vtable | 804f19a0 | 8059b958 |
| parent method at vtable+1f0 | 802ed664 | 80333c1c |
| actual object getProvider at+1cc | 802ed368 | 8033390c |

Actual Mach-O SYMTAB resolves the parent to
IOService::callPlatformFunction(OSSymbol const*,bool,void*,void*,void*,void*)
and the getter to IOService::getProvider() const. The continuation's exact
PC-relative load/add instructions are asserted, not just neighboring literal
pointers. In13G36, the neighboring811b5050 table happens to contain the same
parent function but is NOT the actual caller's table; bounded native execution
caught that distinction and the frozen facts use the actual811b504c binding.

## What this continuation actually does

After the already qualified software-running assertion:

1. It compares the supplied function-name symbol with isIOPCoherent.
2. On that comparison's true result, a non-null output pointer receives a byte1
   and the function returns0; a null output pointer returns raw e00002c2.
   This is a property response, not a core/DMA halt or a hardware status read.
3. Otherwise it invokes the original parent's callPlatformFunction.
4. That real parent obtains the provider through the object's actual getProvider
   virtual method. A null provider returns raw e00002c7. A present provider causes
   a virtual delegation with unchanged function name/wait/first parameter.
5. The original getter uses its cached provider when the registry generation
   matches. On a miss it invokes the registry-parent lookup, normalizes its
   sentinel, stores provider, executes DMB ISH and stores the generation.
   The barrier is in provider-cache bookkeeping; no CDMA/IOP access, halt-ack,
   pending-transfer count or reset status appears in this qualified getter.

The newly qualified local bodies do not perform MMIO or test hardware quiescence.
A dynamic provider implementation may perform further work: it is an explicit
unqualified boundary, NOT proof that the entire platform graph has no reset.
Nor does a hypothetical provider result0 establish its side effects. The existing
actual IOP device-reset property binds a separate ARST pulse with no stop/queue
acknowledgement; it must not be substituted for this quiesce request by name.

## Bounded original execution and countercontrols

emulate-continuation.py passes24 original method controls across the two official
kernelcaches. Complete local platform, generic-parent and provider-getter bodies
execute. Symbol comparison, registry-parent lookup, provider delegation and fatal
REQUIRE are explicitly synthetic boundaries. Their real implementations, dynamic
registry association and concurrency are not executed or inferred.

Controls cover matching-quiesce/running panic, inactive quiesce, coherent query
with/without output, unrelated function, cached/missing-cache provider,
null/present provider and exact propagation of raw synthetic provider results
0/e00002c7/80000000. The independent result/call/output oracle rejects software
flag mutations and unexpected cache updates. Cache-miss writes remain confined
to actual object offsets18/1c; the coherent output is one byte. No MMIO region is
mapped. Any physical address access or unexpected native instruction/write fails.
Each run has600 original instructions,601 step calls and5second wall bounds;
no native exception, timeout or unknown callee becomes success.

inspect-continuation.py independently binds real symbol-table targets and outputs
private sequential disassembly. Both scripts enforce official whole-image hashes.
Inputs: 12H321 db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c;
13G36 8eae16f060a71d4f3790385951588f8198d0f21cdb74146f9750941a3d764125.
No firmware bytes, native programming tables, keys or device user data are exported.

## Precisely remaining hardware qualification

No substantive new hardware stop/reset postcondition was found in these named
local scopes. The remaining requirement cannot be filled with their wrapper
return, software running byte, provider-cache generation/barrier, time delay,
clock target/ack, IRQ mask/route replacement or cleared boot BSS.

An actual backend still needs evidence that, for the correct existing owner and
epoch, the IOP can issue no further CDMA requests, each owned channel can issue no
further descriptor/FIFO/memory accesses, and pending IRQ/captured callbacks are
fenced before old buffers/IDs are released. Required missing pieces are a proven
controller reset/abort completion and outstanding-transaction drain meaning,
actual producer/channel ownership and dynamic provider association, and complete
warm kDFU/BootROM/iBSS/iBEC execution/handoff ordering for our unit. The source
reset pulses and cancel busy-field transition alone do not specify these physical
postconditions. SecureROM/inherited live state and undocumented hardware effects
remain outside the current source evidence.

Safe next qualification is to bind the real provider implementation/caller in a
pinned graph and obtain primary register/postcondition evidence; a reviewed later
read-only device session can record initial status/route/geometry but cannot
prove no-future-access merely by one status sample. No write probe or reset trial
is proposed here. The default-off engine/cancel/completion policies remain intact.

## Repeat

Use existing bounded offline image cascadia-firemarco:a9bf28f with /repo,/references,
/stage readonly and only a new private /out writable, UID1000, no device mounts:

    python3 /stage/inspect-continuation.py
    python3 /stage/emulate-continuation.py

The selected authored tools and semantic facts/traces are pinned in source-provenance.json. Native disassembly outputs are intentionally excluded.
