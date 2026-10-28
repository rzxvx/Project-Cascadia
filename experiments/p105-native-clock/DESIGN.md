# Checked native P105 host clock broker — source-only candidate

This component supplies concrete regmap transitions and local owner accounting
for the verified 12H321 Performance graph subset. It has no probe, registered
clock provider, device-tree node, GPIO/rail operation, VBUS operation, firmware
microcode or role publication. Kconfig defaults off. The existing gadget and
HSIC-WLAN driver are not connected to it. It cannot activate on today's binding.

## Source and scope

Pinned official kernelcache SHA256
`db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c`,
P105 ADT `5c346586116de1bb32606acb27cad3158bcaa8a1318a10ee9631de09884cfd72`.
Native property stride44 and limits56/128 are independently executed source
blocks. Original constructed Performance vptr80b90938 +350→804b3141,
+3e8→80b8c975, +448→804b2a8d (optional observer), +3ec→80b8cbf1,
+488→80b8de39, +490/+494→80b8df5d/df65 establish this interface. The retained
PMGR provider mapping at+self550 is unchanged, actual physical reg[0]3f100000.
This does not resolve the USB-complex's platform pointer on a running unit.
The actual clkG forwarder reaches this graph through a CommandGate; the earlier
804b56d4 opcode decoder is cpuI, not clkG. The [true clkG factory and bridge](../../docs/research/p105-true-clkg.md)
now have public bounded original-instruction regression coverage. Registry
[source registry association](../../docs/research/p105-clkg-registry.md) is now
executed with explicit fake matching; live Linux binding and inherited global
adoption still need separate proof.

Source-bound nodes are2/3/4/5(PREDIV),40(UPERF),42(USBREG),87(PHY),89(EHCI).
Roots87/89 >=56 have native idempotent active bits; parents<56 have counts,
which recurse only on their own first/last activation. Own client masks are
aggregated *before* changing those root active bits. IDs88 and90/91 are refused;
DWC2's optional domain policy, OHCI's additional closure and encodedC0 formats
are not approximated. Metadata is ordinary private RAM, not Apple object layout.

Direct clocks are PMGR+38+4*index, index1..51, bit31 enable, bit30 busy. Our
fixed table contains only proven direct codes4/5/6/7/25/45. Gates are
PMGR+1000+4*index32/34/38. On applies clocks ascending then gates ascending;
off applies gates descending then clocks descending. Gate write mask clears
0x10f (including bit8), targetF for on and default4 for off. Explicitly adopted
owned zero-off policy may select0 for these three supported gates only. Neither
4 nor0 is assigned an invented electrical meaning. Existing resetbit31 blocks
these gate operations; the separate controller owner handles reset.

The native monotonic helper wrongly returns success for targetF/ack0 without
polling. The candidate always checks acknowledgement (even without a write),
checks final clock bit/busy state, and bounds each poll to1..1000 reads. Source
firmware's unbounded polls are represented as capped negative controls. Bounded
iterations do not impose a deadline on an underlying regmap provider that hangs.

## Adoption and foreign owners

Initialization leaves permitted=false and adopted=false. Adoption requires six
explicit external conditions: source binding, dynamic provider association,
**all global graph owners** adopted, inherited hardware state verified, every
shared client quiesced, and gate power policy owned. These certificate booleans
are integration contracts, not a security boundary or a substitute for evidence.
No current Linux implementation supplies them. Readback cannot reconstruct old
reference counts or prove that CPU/display/nonUSB users have joined one lock.

The supplied baseline holds protected native parent references and foreign
87/89 root active bits. Own client refs begin empty. Sanity checks reject absent
required parents, impossible foreign root bits and unsupported power policy.
Adoption reads active clocks/gates and refuses unready/busy/reset state. Protected
foreign87/89 roots keep their parent contributions; arbitrary adopted foreign
parent pins (including an existing88/CPU/display dependency) remain after own
last release. A fake baseline from the original88 scenario demonstrates this
accounting, not domain handling or physical adoption. All root and parent owners
must use the same pre-held sleepable global mutex. lockdep verifies this contract
where enabled; mutex_is_locked alone cannot identify the current owner task.

This fixed broker cannot release, migrate or discover foreign pins. Future native
provider integration must first adopt truthful global counts/active masks and
ensure no bypassing writer. Do not set gate_release_verified elsewhere merely
because this file compiled. Its owner/epoch lifecycle must be joined atomically
to the PHY/client lease aggregator and the existing clock/power backends.

## Failure and lifecycle

Each request computes a pending RAM snapshot. No count/owner commit occurs until
the checked physical plan, active-state verification and final epoch check all
succeed. Every I/O checks the epoch before and after. A write is marked touched
before calling the provider, because errors can arrive after physical effects.
Failures preserve the previous committed snapshot **and an unknown pending
physical disposition**, latch the original negative error and block later I/O.
No speculative rollback, reset, forced success or refcount repair occurs.

An actual validator failure or missing validator after adoption latches even
before the first write;
a caller supplying the wrong epoch is rejected without corrupting the lease.
Epoch invalidation/publication and provider lifetime remain the owner framework's
serialized obligations. No recovery or teardown API exists: a faulted/owned
context must remain alive for diagnosis and externally reviewed reconciliation;
it may not be reinitialized/freed to forget hardware ownership. init refuses
reinitialization and ERR_PTR/null providers/locks. Software plan overflow or
unowned/duplicate client requests fail before I/O. The pending plan after such a
planning failure is not an executed hardware operation.

## Validation and remaining integration

Normalized oracle SHA256
`9ca8bc80f28aeb5e0ec5005d527ddde05afc95d7cc052877879ef1ce238e0a9c`.
It contains19 native positive and7 negative requests, source hashes, decoded ADT
metadata and semantic write traces. Original emulators use explicit fake RAM,
PMGR autoack, IRQ/lock and ordinary copy callbacks. No firmware bytes are embedded.
Run `python3 tests/test-experimental-native-clock.py` from the repository.
Kconfig/Makefile fragments are deliberately unsourced.
Tests compile the actual C and expose its fixed metadata, compare native write
orders/counts for supported cases, test protected global/foreign peers, first/
last clients, default denied certificates, zero-off policy,32-bit preservation,
every early/late I/O failure and epoch boundary, missing-validator retention
with an actual earlier-source counterexample, unready native shortcut, poll
limits, overflow, invalid resources and reinit. ASan/UBSan executable and real
Linux6.12 ARM headers compile this same source. OHCI/domain successes in the
native fixture are explicitly unsupported by the candidate, not claimed passes.

Next integration gates are explicit Linux Performance-equivalent supplier binding,
all-global-owner migration/baseline adoption, parent-clock/rate lifecycle and
mapping/clock reference ownership, full controller/PHY sequencing, accessory
identification/authorization, and verified externally supplied VBUS. A HS device
or HS hub with TT avoids requiring the direct-FS OHCI companion for the first
hardware target, but does not eliminate these gates. Native5V output, Linux host,
charging, camera authentication and physical Wi-Fi continuity are untested.
