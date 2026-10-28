# Checked THS7383 IDbus transport primitive

Status: source-only/default-denied/unregistered Linux C. The real provider of
exclusive IDbus ownership, GPIO/IRQ/workloop binding and hardware lifetime is
**not implemented**. Tests never access devices or execute bus operations.
No authentication, Camera identity, current, rail, USB role or Linux boot is
granted by a successful transport operation.

## Native source contract and concrete implementation

The previously frozen21 original THS cases bind official13G36 kernelcache
SHA256 `8eae16f060a71d4f3790385951588f8198d0f21cdb74146f9750941a3d764125`.
Actual THS window method `0x809eebd8` returns32; primitive `0x809eebdc`
accepts TX/RX lengths1..32, writes TX at20h, receive control41h
`2*(rx_length-1)`, then trigger40h `2*tx_length-1`. Each native write return
is checked and pending RX remains after failure. Original response handler
`0x809eeabc` reads status0dh and, when `(status&38h)==10h`, reads RX at
`40h-rx_length` and reverses the byte order. Its bulk-read error is ignored;
our C implementation corrects this behavior before any response publication.
These are source-evidenced operations, not hardware-validated initialization
values or permission to invoke them on an accessory.

`cascadia-ths-idbus.c` uses actual Linux `regmap_bulk_write`, `regmap_write`,
`regmap_read`, `regmap_bulk_read`, mutexes and atomic64 primitives. Initialization
requires a valid **8-bit-value regmap** to avoid buffer overreads from wider
regmap values. Missing lease validation keeps all I/O denied. Construction
only allocates software identity and checks regmap metadata; it performs no
bus I/O. No driver, module initializer, DT node, IRQ handler or worker is
registered. Kconfig/Makefile fragments remain separate, default-n proposals.

## Owner/request identity and failure state

One atomic, monotone, positive63-bit token namespace supplies both owner and
request identities. IDs never wrap or repeat during the same actual
allocator/code lifetime; exhaustion fails with EOVERFLOW before I/O.
There is no token reset API. Code/allocator unload or reload is forbidden
while any old context/callback can still exist. Identity continuity is not
automatically preserved across module reload or reboot; a new lifetime needs
a fresh external namespace/epoch and complete old-callback retirement. Reinitializing an initialized context is
rejected. Fresh storage must be entirely zeroed; `memchr_inv` refuses dirty storage
even when its initialized flag is false, preserving any fault/pending record; copying/externally resetting mutable
contexts is outside the API contract.

The external arbiter must validate the concrete owner/epoch and prove that
it is the sole physical IDbus owner. All users of the same bus must share
that arbiter and mutex. Unique numeric tokens alone do **not** prove physical
exclusion, block bypassing writers or establish an electrical/IRQ lease.
The lease callback is required before and after each attempted I/O and runs
under the caller-provided mutex. Local owner/epoch/request/pending/state and
an irreversible atomic revocation flag are checked before AND after the
external callback; positive callback results map to EPROTO. It must not recursively acquire that mutex.
Context and buffer pointers must be valid, live and properly aligned.
Configuration (regmap, lock, callback/cookie, owner and epoch) is immutable
after initialization. Its resources/cookie/regmap/lock/code must outlive the
context and all callbacks; no real arbiter is
supplied by this experiment. A fake callback in tests is not hardware proof.

A request is minted immediately before the first staged write and stored
with pending RX. Any bus error or actual lease failure after touched I/O
latches UNKNOWN with the error, request identity and pending RX retained.
Restoring a callback does not resume the transaction. No retry, hardware reset,
cleanup or release API exists; successful read completion returns to IDLE
while preserving owner identity. Each later submission gets a new request ID.
A stale caller identity is rejected without changing a valid pending request.
A too-small output buffer is rejected before reading the bus.

`receive()` performs at most one status read and one data read per explicit
call. Not-ready status yields EAGAIN and retains PENDING; it never polls or
resends automatically. A checked successful data read is reversed into a
local bounded buffer before delivery. Every read failure leaves outputs
zero/length0 and retains UNKNOWN/pending ownership. Epoch/lease invalidation
inside either read callback is caught by the post-I/O validation before bytes
can escape. Software responses are accepted only for the current pending request.
Hardware has no knowledge of these software request tokens: a late old reply
can otherwise appear in the RX window of a newer request. A real external
IRQ/request fence must retire/attribute old hardware events before a new
request is admitted or bytes are interpreted. Tokens alone do not provide
that fence; the missing arbiter/IRQ binding must establish it.

The software-only `invalidate()` latches irreversible atomic revocation,
without I/O, reset, token replacement or release. It can be invoked inside
a validator/bus callback; the next local guard detects it. Changing local
owner/epoch fields inside a callback is also rejected defensively, but direct
mutation/copying of identities is outside the supported API contract.

Caller must serialize initialization and preserve the external ownership
lease during calls. The real arbiter must order connection changes and
response consumption against the shared lock/epoch; this unprovided hardware
binding is not made race-free merely by a local atomic flag. Revocation after
a successfully validated completion is a later event; consumers must retain
the transaction epoch when using the returned bytes. Aliasing request output with context/TX or response/
length output with context/each other is rejected without corrupting context;
valid disjoint bounded output buffers are cleared on errors. TX is snapshotted
before writes. Success proves only that the regmap operations returned0 for
this staged transaction, not accessory authentication or physical usability.

## Actual entire-C validation

`transport-fixture.c` includes the **entire unchanged production C file**.
Only Linux primitive/regmap providers are replaced with fake RAM operations;
there are no handwritten copies of production functions. `test-transport.py`
checks exact SHA256 of both synthetic original-method oracle files and
compiles with strict warnings, AddressSanitizer and UndefinedBehaviorSanitizer.

Coverage:

- All1024 length pairs1..32 check exact three-write register order/control
  bytes, response address/reversal and consumed-request rejection.
- All17 original positive AuthCPAID frames for actual32/32 windows (v2 nine
  transfers, v3 eight) match actual source packet bytes and response lengths.
- Failure at each of the three writes/two reads stops subsequent I/O and
  permanently retains UNKNOWN, request and pending RX.
- Lease loss after every one of the five I/O operations prevents continuation/
  publication; the provider's successful return cannot hide an epoch change.
- Missing/denied owner, invalid resources and regmap width, zero/oversized
  lengths, foreign owner/epoch/request, reinit/dirty-uninitialized storage, ERR_PTR
  contexts, pending resubmit, output bounds,
  not-ready status, stale request after a newer submission and alias rejection.
- Missing validator after a completed transaction faults and cannot recover
  merely by restoring the callback.
- Twenty-eight callback-spanning cases revoke or mutate local owner/epoch
  inside each of the seven validator calls, or return positive callback codes.
  All reject stale publication and preserve pending state after touched I/O.
- Exact hash-pinned earlier private V1 source demonstrates stale response
  publication when owner/epoch changes inside the final validator; the
  candidate rejects the same three controls (including positive-error EPROTO).
  This baseline is our prior private primitive, not an upstream kernel version.
- Eight concurrent allocation threads produce2048 distinct monotone token
  values; explicit exhaustion rejects both new request and new owner.

Host and Ubuntu22 ASan/UBSan tests pass. The production file compiles with
strict warnings against actual Linux6.12 ARM headers,0 warnings. The fake
atomic provider uses C11 atomics; the ARM object uses real kernel atomic64
interfaces. These checks prove software behavior, not live arbitration.

## Remaining integration

Needed before any device use: one real generation-owned THS I2C/GPIO/IRQ
arbiter; complete native status-bit/error/reset semantics; IRQ/workqueue
scheduling; request deadline/cancel/cleanup ownership; real regmap/clock/
regulator/device lifetimes and all bypass-writer exclusion. The primitive
handles the narrow receive-ready mask only; other IRQ status bits are not
acknowledged or consumed. No raw register initialization, accessory rail,
VBUS sourcing/conversion, MFi trust policy, EHCI role transition or device
activation is implemented. Existing normal PHY/gadget/WLAN code is untouched.

No Apple firmware/key/private-device bytes are exported. Oracle packets,
responses and register results are synthetic. Public reproduction scripts for
the original methods are in the separately frozen evidence group; this group
contains pinned semantic fixtures and Linux-C source only.

Root export keeps the entire source/header and normalized fixtures unchanged. Hosted sanitizer builds explicitly disable PIE for stable ASan address-space layout. The caller must supply a proven THS7383 map with8-bit register addresses,8-bit values, normal byte stride and live bus/power/IRQ ownership; value-width checking alone does not identify a chip or prove correct wire addressing. No real map/owner factory is provided. Public test entrypoint: `python3 tests/test-experimental-ths-idbus.py`.
