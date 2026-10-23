# Isolated standard reset-client primitive

Source-only, default n, no driver/DT registration, MMIO mapping, native power
provider, controller/MUX operation or device access. Normal PHY0/gadget and
WLAN stay unchanged. This is a real Regmap reset-membership backend primitive,
not complete external host/client-state5, charging or VBUS implementation.

See `reset-membership-next/EVIDENCE.md` for actual12H321 code. Its pinned hash
is`db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c`.
Only standard kinds0/1/5 and their shared resetbit10h are supported; kinds2/3/4
are explicitly refused before I/O. The actual WLAN producer uses kind3,
resetbit4h and shared power membership2h (Level1), independently from external
EHCIkind0/resetbit10h/Level2. No new HSIC reset write is implemented here.

Sixteen owned slots record per-kind identity membership, matching original
OSSet semantics. An owner can hold different standard kinds simultaneously;
each contributes separately to the0/1/5 total. First group add releases bit10;
last group removal asserts it. Last removal of a specific kind waits20µs even
when another standard kind remains and keeps the reset released. RMW preserves
all other register bits. The fixture models no physical/electrical semantics.

Source requires an actual dedicated temporary shared-PHY Level4 identity lease
callback taking mask16/0, complete standard-group ownership/state adoption,
exact P105 binding and global affected-controller quiescence, denied by default.
That callback must be the actual shared power membership owner, not a blind
generic-PHY power_count operation, static success or unowned put. No native
callback is registered. The actual staged power primitive is necessary but
not sufficient: shared maximum-level membership aggregation still must supply
and restore the correct retained peer level, with native clocks/parents/errors.

Caller already holds the one mutex shared across all power, clock, PHY,
controller and connector clients. Lockdep asserts it; tests assert with a
fake mutex. Controller/role workers may sleep, so this is not an IRQ/spinlock
operation. Bitmap adoption must include every standard group owner; missing
identity count cannot be inferred from observed reset bits. Reset group0/1/5
is complete, while separate HSIC group ownership is outside this module.

All changed operations take/release the checked temporary lease. Duplicate
add and unowned remove intentionally skip the original firmware's unnecessary
temporary power cycle, preserving its no-register/no-membership effect while
making repeated requests harmless. Tests explicitly separate that deliberate
error/no-op policy from exact original normal changed-operation ordering.

Every register operation, lease callback, delay boundary and final membership
commit checks connector epoch. Each retained slot binds its epoch, so a newly
valid connector epoch cannot release/reuse an old owner. Old-epoch cleanup or
new state adoption must be a separately reviewed owner operation; no automatic
cleanup is implemented. Existing memberships plus an epoch failure latch fault
even when the failing call itself touched nothing. Invalid retained bitmap
kinds are refused instead of silently affecting counts.

Lease acquisition/release may fail after applying effects. The source marks
lease_maybe_owned true/lease_known false before either provider call, and only
updates known ownership on successful return. A failure retains uncertain or
held lease state, old membership bitmap and pending target, latches the original
error and blocks all future I/O. No invented rollback, forced reset, power-off
or substitute connector epoch occurs. Successful lease release followed by a
late epoch error leaves known lease-free state but still holds fault/old bitmap;
that mismatch is explicit, not success. Reinit refuses EALREADY and erases no
member/fault state. Recovery is absent and requires separate design.

13 actual-C unittest groups PASS/no skips against fake Regmap/lease/epoch,
with original instruction oracle comparisons of all three standard kinds,
two owners and mixed groups. Cases include late applied lease errors, positive
provider return, every I/O/error/epoch location, final commit, new epoch against
retained old owner, repeated no-ops, high bits/other reset domains and reinit.
The source compiles as an ARM object against real Linux6.12 generated headers
with ARMv7 kernel defines, build-equivalent header-warning exclusions and
-Wall -Wextra -Werror, zero warnings. No kernel/config/DT build changed.

No native lease owner, verified adoption/migration, full clock-parent release,
external controller quiesce, connector role/MUX/authentication, externally
supplied VBUS/protection or physical WLAN-continuity validation exists yet.
These boundaries stay blocked; no client5 or functional first-boot success
is returned by this primitive.

Portable reproduction: `python3 tests/test-experimental-usb-reset.py`. Source
is compiled with fake Regmap/lease/mutex and UBSan. The original semantic
fixture has synthetic providers; no firmware bytes or native mapping is supplied.


## Cold-adopted peer fault retention

Failure checks include all16 retained membership slots, rather than only the
requested owner. A valid cold-adopted peer can exist without any prior local
write; a request from a new empty slot must not forget it when the validator,
temporary power provider or attachment epoch is lost. Restoring the provider
cannot resume this faulted context. The original error, peer bitmap/epoch and
zero-I/O disposition remain retained. A completely empty untouched context keeps
its prior cold admission/retry behavior.

A separate read-only peer review confirms this permitted adoption case. Thirty
actual historical controls (standard kinds0/1/5, adopted slots1/15, missing
providers and initial/no-op/pre-lease epoch failures) reproduce resumption
without reconciliation. Thirty-five fixed/cold cases pass using
complete actual C with ASan/UBSan and explicitly fake adoption/Regmap/lease/epoch
providers. The historical complete source is reconstructed from a SHA-pinned
failed tail and matches its preserved source hash. Existing13 reset tests and5
provider-loss tests remain separate. No physical adoption, DMA fence, reset,
USB-host, WLAN, VBUS or iPad test is performed. Repeat:

    python3 tests/test-usb-reset-adopted-peer.py
