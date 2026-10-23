# P105 iOS 9.3.5 accessory authentication: source evidence and bounded component

Status: offline source reproduction only. The iPad, IDbus, Tristar, PMU,
USB controller and accessory are never accessed by these tools. Research uses
verified official 13G36 firmware; Cascadia's 12H321 boot payload is unchanged.
This is an authentication **exchange parser**, not a USB host implementation,
cryptographic verifier, Camera Adapter allowlist or electrical permission.

## Sources

Kernelcache SHA256:
`8eae16f060a71d4f3790385951588f8198d0f21cdb74146f9750941a3d764125`.
Actual userland `iaptransportd` SHA256:
`0eabef460b814484590b1337e7349ddeb37385d1fd80c83332dd562c3410ad1e`.
Actual shared cache SHA256:
`4f5b5d73244c53fd0bdc5b8012106b10d0dd7607cc850bf7c8f9e9ef3a3f733b`.
IAPAuthentication `__TEXT` extraction SHA256:
`03164548d75eb991bedb9bd523d4e684eeba43c5c43cd2cfc36530962f091fea`.
The files come from the already verified official IPSW; no binaries, keys,
certificates or private device records are included in the public fixtures.
Fixture packet contents, signatures, RAM objects and results are synthetic.

## Actual authenticated-property producer and consumer

`iaptransportd` true wrapper `0x13cfc` and false wrapper `0x13d10` feed
`SignalUSBDriver(0x13490)`. Native COM `GetDevice` obtains an IOKit service,
then `IORegistryEntrySetCFProperty` at stub `0x27d0c` sets `Authenticated`.
The actual nonlazy-import table resolves `0x284bc` to `_kCFBooleanTrue` and
`0x284b8` to `_kCFBooleanFalse`. No device/property is set by our emulator.
Native property-send failures are not checked by this void wrapper.

Actual AppleEmbeddedUSBDevice constructor resolves vptr `0x809dd674`;
virtual slot `+0xac` is `setProperties(0x809d9eb4)`. It accepts actual
OSBoolean identities, requires a task and entitlement
`com.apple.usb.authentication`, and changes cached byte `+0xd8` only after
those gates. The true transition calls metadata/interface helpers and
publication slot `+0x400` (`0x809d9b00`). The latter publishes iAP interfaces
regardless of authentication and gates other interfaces on cached state.
This is downstream interface publication. A root CableType, USBd low flag
`0x20`, external host role or rail/current grant is not inferred from it.

`emulate-auth-property13.py` executes the original producer and the original
consumer identity/entitlement/cache branches: five sender and nine consumer
controls. COM, loader, task, entitlement, metadata and publication boundaries
are fake. Two explicitly documented diagnostic-only native blocks are cut;
no revocation/termination or real interface publication is claimed.

## Remote authentication chip transport, not host-side private signing

Actual prelink personality `AppleAuthCPAID` matches `IOAccessoryDock9Pin`,
category `AU` and digital-ID value/mask bytes `000100000000000100000000`.
`AppleAuthCPI2C` has a different `AppleARMIICDevice/authcp,2c` personality.
These personality bytes are not a Camera Adapter product identity. The
inspected P105 ADT did not contain an `authcp` node; this absence alone does
not prove every possible runtime service arrangement.

Native `iaptransportd cpGetDeviceInfo(0x1d450)` and `cpCopyCert(0x1d470)` use
properties. The nonce wrapper `0x1d490` calls
`IOConnectCallStructMethod` **selector 0**, with output capacity **128**.
Kernel userclient `0x80ec36bc` checks selector 0 and input/output pointers,
then dispatches provider virtual `+0x340`. Actual AppleAuthCPAID constructor
resolves vptr `0x80ec4808`; this slot is `0x80ec2b6c`.

That complete original method and its data-reader `0x80ec2880` run in the
bounded emulator. IDbus provider `+0x34c`, continuation `+0x178`, OSData,
memcpy, version getter, interval and delay boundaries are mocked. No signature
is generated or verified. The source sends a nonce to a **remote signer**:

| Stage | Native request / response | Qualified meaning |
| --- | --- | --- |
| v2 nonce | `94` + 20 nonce bytes; 3-byte reply | version getter must return 2 |
| v3 large window | `94 00 00 00 20` + 32 nonce bytes | source field `+0x5c > 32` |
| v3 small window | `94 00 00 00 10` + first 16; then `94 00 10 00 10` + next 16 | source field `+0x5c <= 32` |
| final nonce ACK | `95`, signed byte1 < 0, delay byte2 | `95 ff 00` qualifies; `95 00 00` does not |
| signer status | `96`; `97` + status | bits `0xc0` together admit next stage |
| output status | `9a`; `9b 00`, then another `9a` / `9b 00` | exact positive original sequence |
| signature read | `98 offset_be16 length_be16`; `99` + payload | bounded chunks: native field `+0x58 - 1` |

Native output size remains 128 for v2 and becomes 64 for v3. Synthetic
outputs `00..7f` / `00..3f` test transport and copying only. The source's
first small-v3 reply is not validated like its final reply; the C counterpart
intentionally validates both. Native retries/cleanup transfers and variable
poll delay exist; the component does not automatically repeat them. The
native busy fixture executes 92 bounded transfers before failure, whereas
our component conservatively faults at its first unready reply.
The provider window is a source framing field, not measured IDbus capacity.
The source interval arguments are not asserted to be measured wall time.

## Narrow unregistered Linux C counterpart

`cascadia-authcp-aid.c/.h` contain no registration, hardware access, DT node,
cryptography, authentication boolean, Camera identity, current or power
callback. Fresh context requires externally authorized IDbus ownership and
an explicit generation; unknown/default authority is denied. Caller must
serialize it and preserve the real ownership lease around each transfer.
A handed-out request is conservatively `touched` and remains pending until
a matching completion. No request is silently reissued. Error, revocation,
or generation mismatch latches a fault, clears cached signature bytes,
retains pending/uncertain ownership evidence and denies later continuation.
Fresh initialization cannot erase an existing transaction.

`plan()` only supplies one bounded packet. `complete()` parses one exact
reply from the external owner. No timing scheduler or bus driver exists.
A completed exchange permits copying **unverified signature bytes** only;
matching iAP certificate policy plus nonce verification remain separate.
Returned output buffers are cleared on failures for valid disjoint bounded
buffers; overlapping context/output/length storage is rejected without
clobbering the context. Reply bytes are snapshotted before cache mutation.
The source admits output capacity 128 at its userclient boundary; this
parser's copy operation permits 64 for a completed v3 result and is not a
replacement implementation of that userclient API.

## Public-key verification boundary (static original instruction evidence)

Actual IAPAuthentication `VerifyNonceSigned(0x31a0fab8)` unwraps CFData and
calls `0x31a0e870`. This source creates **SecPolicyCreateiAP**, sets version
specific anchors, evaluates trust, requires API success plus result 4,
obtains **SecTrustCopyPublicKey**, then calls **SecKeyRawVerify**. v2 passes
padding `0x8002`; the qualified v3 path passes `0x4000`. Verify result 0 is
required. Source `GetRandomDigest(0x31a0fbbc)` allocates random bytes through
`arc4random_buf`; the name is not evidence of an additional hash operation.
Our bounded transport/property experiments do not execute these cryptographic
APIs. No Linux verifier or copied private key/certificate policy is supplied.

Modern primary Apple API definitions describe `0x8002` as PKCS1/SHA1 and
`0x4000` as raw ECDSA signature `(r || s)` with big-endian integers:
[SecKey.h](https://github.com/apple-oss-distributions/Security/blob/db15acbe6a7f257a859ad9a3bb86097bfe0679d9/keychain/headers/SecKey.h).
Trust result 4 is Unspecified, accepted by the native source:
[SecTrust.h](https://github.com/apple-oss-distributions/Security/blob/db15acbe6a7f257a859ad9a3bb86097bfe0679d9/trust/headers/SecTrust.h).
These newer headers define APIs, not the matching 13G36 iAP certificate
anchors/extensions. Generic TLS trust cannot substitute for that policy.
No host-private-signing call was found in this **audited nonce exchange and
verification path**; that is not a claim about all iAP authentication modes.

## Reproduction and open integration

With caller-provided official source files, Unicorn and existing repository
helper, run each emulator with `FIRMWARE_DIRECTORY --source-root REPOSITORY`.
`emulate-authcp-aid13.py`: 16 original-method cases including five positive
framing/chunk sequences, four native admission controls, transfer error,
malformed/zero nonce ACK, unready/busy, signer refusal and malformed read.
`emulate-auth-property13.py`: five producer and nine consumer controls.
All executions have explicit instruction budgets and hash pins.

`tests/test-experimental-authcp-aid.py` runs the actual staged C with cc, strict warnings,
ASan and UBSan against the synthetic pinned original oracle. Five positive
packet/reply sequences match exactly. Seven negative native controls fault
without retries; three nonce/version admission controls reject. Eight extra
ownership/bounds groups cover denied owner, invalid windows, reinit, stale
epochs, pending double-plan, unowned completion, malformed reply, revoked
ownership and aliasing. ARM compilation against actual Linux 6.12 headers
passes with strict warnings. These checks prove software behavior only.

Still required: real Tristar/IDbus owner and accessory connection epoch;
full cryptographic iAP trust/nonce verification; authenticated accessory
identity-to-manager policy; safe accessory rail owner; quiesced shared-PHY
EHCI port1 transition; verified accessory 5V conversion/external supply;
physical Camera Adapter and USB enumeration tests. CarPlay `51/53` roles,
post-enumeration hub `05ac:100c/100d` personalities and 900mA policy requests
are separate evidence, never pre-host authentication or measured power grants.

Root review additionally rejects dirty uninitialized storage without clearing pending/fault/evidence. Its added actual-C group preserves the complete previous bytes. Epochs must not be reused while an older physical transaction/event can still exist. Buffer/owner lifetimes and serialization remain caller obligations.

Root review refuses dirty uninitialized storage rather than overwriting a pending transaction. Nonce admission checks the actual planned packet fits the transmit window (21-byte small/v2,37-byte large v3) and the receive window holds the3-byte nonce ACK. Windows33..36 for v3 reject, unlike the unchecked original threshold. The owner remains external; initialized configuration must remain immutable, epochs cannot be reused until all old transactions retire, and pointers must address valid live memory. Eight extra ownership/bounds C groups pass with ASan/UBSan.

Independent root reproduction exactly matches the16 original exchange cases and14 property controls. The separate [original verification control audit](CRYPTO_BOUNDARY.md) repeats11 bounded original cases with explicitly fake cryptography and certificate trust. Remote signature bytes remain unverified; no host private-key signing is required by this audited exchange/verification path. Matching actual certificate policy and freshness are still missing.

The [actual THS transport audit](P105_IDBUS_TRANSPORT.md) establishes32/32 advertised windows. Two additional source-backed native sequences now match the complete Linux C parser (v2nine transfers, v3eight; seven positive sequences total). The21 original THS capability/submit/response controls independently reproduce with exact JSON equality. Source identifies an unchecked bulk-response error that a future real Linux transport must reject; no active THS transport is implemented here.
