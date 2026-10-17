# P105 THS7383 IDbus transport: bounded native evidence and missing integration

Status: source-only; all I2C and event callbacks below are fake RAM operations.
No device, GPIO, PMU, regulator, USB controller or rail is accessed. No new
runtime driver/backend is implemented in this evidence group.

Source: verified official 13G36 kernelcache SHA256
`8eae16f060a71d4f3790385951588f8198d0f21cdb74146f9750941a3d764125`.
This is research evidence; the normal Cascadia 12H321 boot payload is unchanged.

## Class/ownership boundary

Native AppleAuthCPAID `start(0x80ec216c)` calls its superclass, casts the
provider, calls provider slot `+0x3b4`, stores the returned object in `+0x54`,
retains it, and opens it with client=self/options=`0x10000`. Only after the
open succeeds does it query transport slot `+0x348` with arguments1 and2,
storing returned windows in `+0x58` and `+0x5c`. The option value is not named
as a power/rail permission. Failed setup follows source close/error paths;
this group does not emulate all service construction/unwind behavior.

The constructed BuiltInUSB driver vptr `0x809f09b4` has `+0x3b4` pointing
to `0x809ebd84`, an accessor returning its stored `+0x818` object.
Its factory `0x809eb980` reads `TriStarICClass`, performs native allocation/
type-check calls and stores the object in `+0x818`. Actual prelink P105
compatible strings `tristar,ths7383` / `tristar,cbtl1608` select
`AppleTHS7383`; this is source/personality association, not a runtime probe
of our connected device. The full dynamic factory, actual open lease and
physical IDbus accessibility are not validated by the method fixtures.

Actual THS object vptr `0x809f12cc` has:

| Slot | Target | Qualified source behavior |
| --- | --- | --- |
| +348 | 809eebd8 | returns32 regardless of direction argument |
| +34c | 80800d20 | inherited IOAccessoryIDBusTransport transferData |
| +370 | 809eeabc | native status/response handler |
| +3ac | 809eebdc | native bounded low-level submission primitive |

Do not confuse that IC transport object with its BuiltInUSB I2C provider.
THS stores the I2C provider at `+0x74`; provider slot `+0x40c` writes and
`+0x410` reads. The inherited transport API also has gate/workloop, request
objects, asynchronous completion and client arbitration. Those parts are
not replaced by direct calls to a low-level primitive.

## Actual submission and response

`emulate-ths-idbus13.py` executes the original size method, submission
primitive and response handler with a 3000-instruction budget per case.
The 21 independent cases include4 size arguments,4 valid submissions,
4 invalid size controls,3 write errors and6 response/status/read-error cases.

The submit primitive accepts transmit and receive lengths1..32. Before any
I2C transfer it stores pending RX length in object`+0x84`. It then performs:

1. Bulk-write TX bytes to register`0x20`.
2. Write register`0x41 = 2 * (rx_length - 1)`.
3. Write register`0x40 = 2 * tx_length - 1`.

Every write return is checked; failure stops later writes and propagates
the error. The already stored pending RX length remains. A partial operation
must therefore retain uncertain state, never assume that zero return from
a later retry repaired the previous transaction. These values are original
source evidence, not instructions authorized for hardware execution.

The response handler reads register`0x0d`. A first read error stops processing.
When `(status & 0x38) == 0x10` and pending RX is nonzero, it reads that many
bytes at register`0x40 - rx_length`, reverses their order, clears pending RX,
and dispatches an IDbus event callback. Other status bits invoke separate
native handlers, which this narrowly scoped fixture does not execute.

Diagnostic source gap: the native **bulk** response read return is not
checked before reversal/event publication. A synthetic failed read with
poisoned bytes demonstrates publication of poisoned data and clearing of
pending RX. A Linux implementation must check that read and latch the
uncertain/error state rather than copying this source behavior. This is an
original Apple firmware diagnostic, not a newly fixed Cascadia driver bug.

## Concrete P105 framing consequence

Unlike earlier generic synthetic64/17-window cases, the THS source advertises
**32 for both windows**. This necessarily selects two16-byte nonce chunks
for version3; the native AuthCPAID data reader retrieves signature bytes
in payload chunks of at most31, because the response opcode occupies one
byte. Reproducing the already frozen original signer emulator with32/32 gives:

| Version | Nonce requests | Signature bytes | Total native positive transfers |
| --- | --- | --- | --- |
| 2 | 94+20 nonce bytes | 128, five31-byte-or-smaller data chunks | 9 |
| 3 | two94 headers+16 nonce bytes each | 64, three31-byte-or-smaller data chunks | 8 |

`reproduce-authcp-ths-window.py` pins the reviewed signer emulator's exact
source SHA before loading its existing bounded `run` function. The semantic
oracle contains only synthetic packet/signature bytes. The two methods are
composed through fake advertised windows; no actual provider service, I2C
transaction, cryptography or accessory is exercised.

## Minimal next Linux integration

The existing unregistered AuthCPAID packet parser can use these32/32 framing
inputs, but a real transport still needs an exclusive generation-owned THS
I2C owner, checked staged writes, GPIO/IRQ/workqueue routing, cancellation/
timeout semantics, exact bulk-read checking and a shared accessory lifecycle.
An error must preserve pending/uncertain ownership; a response belongs only
to the same live accessory generation and successfully submitted request.
Actual asynchronous IRQ routing, gate/lock lifetime, complete status-bit
handling, connection reset, rail state, certificate/nonce verification,
Camera identity-to-host policy and native/external5V VBUS are still open.
No register-on observation, digital ID, authentication property or packet
success can supply those permissions. No new C backend is included.

Reproduce with caller-provided firmware and existing source helper:

```
python3 emulate-ths-idbus13.py KERNELCACHE --source-root REPOSITORY
python3 reproduce-authcp-ths-window.py SIGNER_EMULATOR FIRMWARE_DIRECTORY --source-root REPOSITORY
```

The exact text files and fixture hashes are in `manifest-idbus-transport.json`.
No Apple binary/private key/device record is part of this group.

Root independently reproduced both text-only original groups and exact normalized JSON equality. The two32/32 signer sequences are additionally checked against the actual Linux C parser under ASan/UBSan. No real IRQ/I2C transport or cryptographic verifier is supplied.
