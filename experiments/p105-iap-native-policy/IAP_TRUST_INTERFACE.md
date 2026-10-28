# Native13G36 iAP trust interface and qualified facets

The mathematical nonce verifier is already implemented separately. This group
closes actual source-provenance and policy-facet gaps; it does **not** provide a
complete peer-chain validator or authorize an accessory. A generic TLS verifier
cannot be labelled equivalent to the observed iAP policy.

Source inputs are caller-owned immutable files from verified official13G36:

| Input | SHA256 |
| --- | --- |
| dyld_shared_cache_armv7 | 4f5b5d73244c53fd0bdc5b8012106b10d0dd7607cc850bf7c8f9e9ef3a3f733b |
| securityd | 1c12fbb48ead1edd3dbf2f4b0d635b45452573499fd9592562bbd76e40b56097 |

Original daemon Mach-O nlists retain named checkers. `native13.py` maps original
source segments and resolves the executable's imported global slots to actual
named exports in the pinned dyld cache. This is an explicit synthetic loader;
it does not launch securityd or connect to its services. Source API addresses
are checked before bounded original instruction execution. Original code reads
only source/synthetic RAM. CF/certificate/path/crypto operations described below
are deliberate mock boundaries. No Apple firmware, certificate DER, private
key, or real peer identity is included in this text package.

## Actual anchor selection and certificate-container boundary

`emulate-anchor-flow.py` runs original IAPAuthentication `0x31a0e870`, its actual
certificate-extraction caller `0x31a0ec28`, and actual once block `0x31a0f97c`.
Five controls pass: v2, supported v3, unsupported-preference v3 with synthetic
trust failure, a two-certificate input container, and an empty input container.
CF allocations, CMS parsing, trust evaluation and nonce signature verification
are synthetic in this original-flow experiment; successful return is never
presented as real trust. In particular, the empty-anchor control's failed trust
result is a synthetic input, not native path evaluation.

The certificate helper calls actual imported
`SecCMSCertificatesOnlyMessageCopyCertificates`, requires a nonempty array and
selects index0. Even a synthetic two-certificate input contributes exactly one
certificate to `SecTrustCreateWithCertificates`. This proves the caller's
selection/order, not native CMS decoding or signature validity of a container.
Missing issuer certificates may have to come from native issuer sources; the
whole remote container is not automatically a validated certificate chain.

For v2, source pointers supply two DER certificates to
`SecCertificateCreateWithBytes`. Their SHA256 fingerprints are:

- 884bf87d549f2de7c4fceb4284a8cff9f8b817f36dcf77a40660b33b8afefd0d
- 2eb53dbefdc6f5e7e13d6690fc8cbf9cf4c3f7e501feafb3bc052431dbc83180

These are RSA2048 accessory CA intermediates, issued by named Apple roots; their
issuer signature is not verified by this metadata proof. The native caller
installs these certificates themselves as custom anchors. Self-signing is not
a prerequisite for a certificate to serve as an explicitly configured anchor.

The actual v3 once block queries
`CFPreferencesGetAppBooleanValue("AppleIDAuth30Supported", "com.apple.iaptransportd", NULL)`
and normalizes that returned support value. The true branch supplies a637-byte
embedded certificates-only PKCS7 container at `0x31a11a10`; false leaves the
explicit anchor array empty. This preference is support configuration, not
Camera identity, debugger authority, authentication success or electrical grant.
Do not introduce a Linux setter/override or interpret an unknown value as true.

`inspect_anchor_source.py` independently parses that source container with real
cryptography. It contains one588-byte secp256r1 certificate, fingerprint
`81da17dabaf55012d1ea3946c02c3a423405bc290403cc54cdb88bfe95677472`.
The source blob hash is `ad270bdad5be67e79070701c435710e87a1e1dc2faa38ddc3a9018c1eba8a416`.
The embedded certificate is self-issued; its actual ECDSA/SHA256 self-signature
verifies mathematically using its public key. This is real signature math, not
trust in a remote certificate. It has no BasicConstraints extension and one
critical Apple extension, OID `1.2.840.113635.100.6.36`. No meaning of that private
OID is invented, and its bytes are not exported. Native treatment of an explicit
anchor must be distinguished from treatment of an untrusted leaf/intermediate.

## Eighteen actual cache API leaf controls

`emulate-trust-leaves.py` executes source APIs with fake CF/public-key providers:

- Five anchor setter controls execute `SecTrustSetAnchorCertificates` at
  `0x20cb44a0`. A nonnull supplied array sets native anchorsOnly, invalidates the
  cached result, retains the new array and releases the old array. Array
  cardinality is not read by this API: the synthetic empty-array pointer has
  the same nonnull branch. Null trust returns-50 and changes nothing.
- Thirteen weak-key controls execute `SecCertificateIsWeak` at `0x20c9b510`.
  Actual imported `SecKeyGetAlgorithmIdentifier` is distinguished from similarly
  named GetAlgorithmId/ID. RSA algorithm1 requires a block size of at least128;
  ECDSA algorithm3 requires at least20; missing/unknown public keys are weak.
  Threshold/control values come from fake providers, not a real certificate.

## Twenty-three actual securityd facet controls

`emulate-checkers.py` executes named original checkers and, crucially, the full
native `SecPVCSetResultForced` body rather than returning a fake success:

| Facet | Actual bounded result |
| --- | --- |
| SubjectCommonNamePrefix,0x329ec | exactly one CN beginning case-sensitive `IPA_`; zero/two/missing/wrong-prefix CN reports failure |
| NotValidBefore,0x32b5c | rejects certificate notBefore less than or equal to the configured cutoff; accepts the tested greater value |
| IdLinkage,0x32038 | adjacent SKI/AKI mismatch reports failure when both identifiers exist; absence of either does not trigger this comparison |
| SetResultForced,0x33840 | missing optional policy key ignores a nonforced failure; a present key or forced failure clears the native result flag |
| ParentCertificateChecks,0x33980 | native weak-key failure is rejected; nonanchor CA absence or present KU without keyCertSign is forced failure; an explicitly anchored final certificate is exempt from those CA/KU tests |

The parent controls also demonstrate that a synthetic invalid anchored root's
nonforced ValidRoot failure is ignored when that policy key is absent, but
rejected when added. This is a narrow actual consumer/setter fact; it does not
prove every possible native time/path check is absent. The observed iAP producer
has no ValidRoot/ValidLeaf/ValidIntermediates option. Do not replace this with
an undocumented claim of universal expiry acceptance or introduce TLS date
semantics under the native profile's name. A stricter separate local policy
would need an explicit name and compatibility limits.

An explicit anchor's CA/KU exemption supports the embedded v3 certificate's
missing BasicConstraints being compatible with that parent facet. It does not
by itself prove handling of its critical Apple extension: complete native path
processing, unknown-critical classification and root/leaf distinctions remain
part of the qualified chain implementation. The named CriticalExtensions and
BasicContraints dispatch callbacks are small stubs; checks also live elsewhere
in the path processor. A callback's lack of work does not prove lack of policy
enforcement.

## Concrete future trust boundary

Inputs to an authorization-capable interface must remain separate:

1. **Owned immutable exchange:** connector/owner generation, externally proven
   current identity and transport completion/drain, fresh unpredictable nonce,
   declared protocol version, certificate container, complete unverified
   signature. Software request IDs cannot create a chip-side late-reply fence.
2. **Qualified source policy:** exact13G profile revision; source-selected anchor
   family and original support configuration; source-pinned anchor provenance;
   native CMS certificate-selection rules; policy option dictionary and fixed
   cutoff conversion. Missing/unknown support cannot enable the v3 anchor.
3. **Real certificate path:** the leaf actually used for nonce verification,
   locally supplied issuer path, exact trusted anchor identity, real signatures
   over certificate TBS bytes, parser results and extension/path constraints.
   The current research contains anchors, not an actual accessory's signed leaf
   or verified issuer path. A mathematical self-signature on a supplied root does
   not select that root as trusted.
4. **Qualified native enforcement:** critical extensions, name/policy constraints,
   CA/path length/KU rules including anchor exemptions, exact key algorithms and
   strength rules, linkage and subject rules, date semantics, issuer discovery,
   any applicable trust exceptions/OTA state. Generic TLS success, arbitrary
   peer-provided roots or the labels of CF options are insufficient.
5. **Independent connector policy:** certificate-derived accessory class/identity,
   fresh physical owner binding and allowed operation. Even trusted iAP nonce
   authentication does not by itself grant Camera host routing/current/VBUS.

The intended output should report certificate-path math, policy qualification,
source anchor identity, nonce signature validity and owned generation separately.
Until the real path and complete rule coverage are qualified, the public contract
remains `certificate_chain_trusted=null`, `accessory_identity_verified=null`,
`authorization_granted=false`. Failures must clear usable outputs; no callback
may manufacture native trustResult4. No validator bypass or fake success adapter
is proposed here.

The next implementable step is a bounded, explicitly supported offline path
profile against source-pinned anchors, using real synthetic positive/negative
certificate signatures and original checker controls. It must reject unsupported
critical/path semantics and prove its accepted subset cannot evade native checks
before reporting native-profile trust. A real accessory chain is additionally
needed for compatibility; it is unavailable in current offline source evidence.

## Primary comparison and reproduction

Apple's contemporaneous published
[Security57337.60.2 SecPolicy.c](https://github.com/apple-oss-distributions/Security/blob/4f36c5f47ec9f03c406c088d2c427bd5582b2631/OSX/sec/Security/SecPolicy.c)
provides comparison context for the observed option names and2006/5/31 prototype.
[SecTrust.c](https://github.com/apple-oss-distributions/Security/blob/4f36c5f47ec9f03c406c088d2c427bd5582b2631/OSX/sec/Security/SecTrust.c)
describes explicit anchor-only configuration and native trust-service inputs.
[SecPolicyServer.c](https://github.com/apple-oss-distributions/Security/blob/4f36c5f47ec9f03c406c088d2c427bd5582b2631/OSX/sec/securityd/SecPolicyServer.c)
and
[SecCertificate.c](https://github.com/apple-oss-distributions/Security/blob/4f36c5f47ec9f03c406c088d2c427bd5582b2631/OSX/sec/Security/SecCertificate.c)
are comparison references. Security.Info.plist's short version10.0/platform13F68
does not establish an exact OSS build match. Actual claims above are grounded in
pinned native instructions/nlists and bounded controls, not assumed tag parity.
Private downloaded source mirrors are excluded from export.

```
python3 emulate-anchor-flow.py DYLD_CACHE
python3 emulate-trust-leaves.py DYLD_CACHE
python3 emulate-checkers.py DYLD_CACHE SECURITYD
python3 inspect_anchor_source.py DYLD_CACHE
```

Native emulation requires existing Unicorn; metadata inspection uses existing
cryptography41.0.7. All source files are read/hash-verified before execution.
No downloads, package installations, privileged containers, service/device
access, hardware writes or native daemon execution occur in these scripts.


## Real public-key and RSA link follow-up

The earlier weak-key controls supplied synthetic size/algorithm getters. The
[rsa-closure](rsa-closure/REAL_RSA_PROFILE.md) follow-up independently replays
seven native public DER import, descriptor, block-size and weak-root consumer
cases. A1023-bit public modulus occupies128 bytes and is not weak under that
specific native byte threshold. The supported experimental nonce profile remains
exact RSA-1024; this evidence does not widen it or establish a trusted root.

[Native link controls](rsa-closure/NATIVE_LINK.md) independently execute actual
RSA public modular exponentiation and original PKCS1 padding/signature checks:
valid, corrupt and absent-issuer-parameters RSA cases. Each case reaches native
RSA verification; absent parameters in this two-entry/no-later-parent path do not
mean verification was skipped. Python supplies SHA256; certificate field objects,
CF and heap are synthetic. The full certificate parser/path builder/trust policy
are not executed. Three independent public cryptography tests corroborate the
signature fixture. No chain trust, accessory identity or power/role permission is
provided. New ordinary tests validate only public crypto and pinned records;
manual original-input replays remain separate from those tests.
