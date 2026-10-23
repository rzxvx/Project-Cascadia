# Direct certificate-link evidence, not certificate trust

This offline component makes a real certificate-signature verification available
alongside the already tested nonce verifier. It also checks a deliberately narrow
subset of independently observed iAP metadata facets. It provides **no successful
chain-trust result**, accessory identity, Camera Adapter admission, input-current
permission, GPIO/rail state or VBUS/USB-role grant.

`certificate_chain_trusted` and `accessory_identity_verified` always remain `None`;
`authorization_granted` always remains `False`, including positive tests. The
frozen result exposes mathematical evidence separately from
`supported_facets_pass`. An arbitrary synthetic issuer can pass the mathematical
checks without matching any source anchor. That is intentional evidence testing,
not an added trust anchor.

## Qualified source and implemented subset

Official iPad2,5 iOS 9.3.5 / 13G36 dyld cache SHA256:
`4f5b5d73244c53fd0bdc5b8012106b10d0dd7607cc850bf7c8f9e9ef3a3f733b`.
Original securityd SHA256:
`1c12fbb48ead1edd3dbf2f4b0d635b45452573499fd9592562bbd76e40b56097`.
These inputs are private, caller-supplied, read-only data for the emulator; no
Apple binary bytes or certificates form part of this source group.

The actual `SecCertificatePathVerify` at `0x20c9e638` calls the local certificate
helper at `0x20c9a9d0`. The latter passes the certificate's TBS signature algorithm,
TBS bytes and signature to the named `SecKeyDigestAndVerify` at `0x20ca8bcc`.
The original mapper `0x20ca8c48` and DigestInfo builder `0x20ca93cc` are exercised:

| Public certificate signature algorithm | Original mapped payload |
| --- | --- |
| RSA PKCS1-v1.5 with SHA1 | SHA1 DER DigestInfo + 20 digest bytes |
| RSA PKCS1-v1.5 with SHA256 | SHA256 DER DigestInfo + 32 digest bytes |
| ECDSA with SHA256 | 32 digest bytes, no RSA DigestInfo |

Nine bounded original-code cases cover each algorithm with synthetic verification
success/failure, an unsupported OID with no verification call, and whole two-entry
`SecCertificatePathVerify` success/failure. The digest and public-key verification
providers are **fake** in that oracle: it proves routing, payload construction and
error normalization, not cryptographic validity. The separate Python tests use
real cryptography for those three schemes and real nonce signatures. The local
certificate-helper name is inferred from its actual callsite and argument use;
no missing symbol name is invented.

The implementation accepts only one direct leaf → explicit issuer. It verifies
the real TBS signature with the selected issuer public key, requiring the inner
and outer signature AlgorithmIdentifier bytes to agree. It rejects RSA-PSS and
other algorithms, unexpected parameters, trailing objects, excessive sizes,
nonminimal inspected lengths, duplicate/malformed extensions and intermediate
paths. RSA issuer keys are bounded to 1024–4096 bits; the supported EC issuer
profile is P-256. This is a supported subset, not every native key/curve.

Independently qualified native metadata facets used here:

- Exactly one common name begins with case-sensitive `IPA_`. This component further
  restricts it to ASCII; it does not reproduce all CoreFoundation Unicode rules.
- The notBefore instant is strictly after the original calendar API contract:
  Gregorian `yMd` / 2006 / 5 / 31 with an explicitly set GMT+0 timezone, therefore
  the intended cutoff is 2006-05-31 00:00:00Z. The calendar oracle executes the
  original producer/blocks but mocks CF arithmetic; no measured native absolute
  time or device clock is claimed. The negative compose-status control shows the
  native caller ignores that status; this helper does not weaken its cutoff.
- Adjacent authority-key/subject-key identifier bytes are compared if both exist.
  Missing identifiers are reported as comparison not performed; optional issuer
  names or serial-number AKI profiles are rejected as unqualified.
- The pinned nonce verifier's supported RSA-1024 / P-256 leaf profiles satisfy the
  native weak-key minima observed from actual key-block-size consumers.

The direct issuer-name equality is an extra supported-subset restriction. It is
not Apple's normalized-name discovery algorithm. All critical leaf extensions
are denied because native known-critical classification is not completely
qualified. Even unknown **noncritical** leaf extensions are outside this subset.
Supported noncritical leaf extensions are only BasicConstraints, KeyUsage, SKI
and AKI; their presence is not promoted to a complete path-policy result.

An explicitly supplied issuer is not verified against its own issuer, and an
embedded intermediate used as a custom native anchor need not be self-signed.
The original anchored-root basic-processing control skips root critical/CA/KU
getters; this helper does not reinterpret a critical issuer extension as a leaf
extension. Mathematical success with such an issuer remains untrusted.

## Source anchor identity, not a trust shortcut

The immutable SHA256 identity table records actual source certificate bytes:

- v2: `884bf87d549f2de7c4fceb4284a8cff9f8b817f36dcf77a40660b33b8afefd0d` and
  `2eb53dbefdc6f5e7e13d6690fc8cbf9cf4c3f7e501feafb3bc052431dbc83180`.
- v3: `81da17dabaf55012d1ea3946c02c3a423405bc290403cc54cdb88bfe95677472`.

For v3 the native configuration `AppleIDAuth30Supported` under
`com.apple.iaptransportd` still determines whether the explicit anchor array is
populated. This component does not read or override it. A source fingerprint
match alone does not establish that configuration, certificate-chain trust,
accessory provenance or hardware authority. No actual accessory leaf or signed
leaf→anchor path was supplied in this offline work.

## Remaining trust boundary

The actual native caller requires successful `SecTrustEvaluate` with result 4
before copying the public key. This component cannot manufacture that result.
Full native path discovery, intermediate constraints/policies, all private and
critical extensions, certificate parser classification, revocation or trust
configuration and captured peer-chain interoperability remain unqualified.
Native iAP policy omits the observed ValidLeaf/ValidIntermediates/ValidRoot options;
wall-clock expiry is therefore **not** substituted with a generic TLS check here.
An expired synthetic leaf can produce mathematical/facet evidence and still
never becomes trusted. This does not prove that no other native path enforces
validity elsewhere.

External integration still needs immutable generation-owned peer snapshots,
actual transport authentication/lease/IRQ attribution, proven native trust-policy
coverage and the adapter-specific role/power owner. No request, bus, system trust
store, preference, network, device or private-key interface is provided.

## Reproduce

Host dependency: `cryptography==41.0.7`; no installation is performed by this
source. The dependency on the existing `authcp_verify.py` is pinned in
`dependencies.json`. Put that directory on `PYTHONPATH` to run:

```sh
python3 test_direct_evidence.py
python3 emulate-cert-signature.py CACHE SECURITYD > cert-routing-oracle.json
```

The second command additionally needs the existing Unicorn runtime and the two
exact official files; it is always bounded to 1800 instructions per request.
No native executable is run. No certificate/key byte fixture is saved by the
crypto tests: all synthetic private keys stay in process memory. The SHA1 test
re-signs an ephemeral RSA certificate's public DER using a real SHA1 signature
because cryptography's CertificateBuilder deliberately rejects SHA1 signing.
This test does not alter a source certificate.

Primary algorithm references: [Apple SecKey.h](https://github.com/apple-oss-distributions/Security/blob/db15acbe6a7f257a859ad9a3bb86097bfe0679d9/keychain/headers/SecKey.h),
[RFC 8017](https://www.rfc-editor.org/rfc/rfc8017.txt), and
[cryptography 41.0.7 RSA API](https://cryptography.io/en/41.0.7/hazmat/primitives/asymmetric/rsa/).
The intended calendar conversion uses the [CFAbsoluteTime contract](https://developer.apple.com/documentation/corefoundation/cfabsolutetime);
original CF arithmetic is not emulated. Contemporary Apple OSS
[SecCertificate.c](https://github.com/apple-oss-distributions/Security/blob/4f36c5f47ec9f03c406c088d2c427bd5582b2631/OSX/sec/Security/SecCertificate.c)
supports interpretation of the named API but is not an authenticated exact match
to the 13G36 implementation. Actual pinned instructions determine the oracle.

## v2 malformed-DER correction

A real synthetic certificate with SAN extension DER `3002a300` loads as an
X509 Certificate but lazy extension decoding raises the specifically supported
`x509.UnsupportedGeneralNameType` exception (x400Address). The baseline lets that
exception escape. v2 catches exactly this additional exception and produces
denied evidence for both leaf and issuer. Another genuine malformed SAN
`300281ff` raises ValueError and remains denied. There is no blanket exception
catch. UnicodeDecodeError is already a ValueError subclass; an OverflowError
path has not been reproduced and is not invented as a test/provider behavior.

The original nine routing cases are byte-identical to the prior source group;
the changed real-crypto suite now contains 22 groups. No original frozen files
are changed.
