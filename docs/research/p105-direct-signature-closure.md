# Direct certificate signatures and remaining native trust

The host-only `experiments/p105-iap-direct-evidence` adds actual direct
certificate/issuer signature verification to the existing nonce-signature
verifier. Supported algorithms are RSA PKCS1v1.5 SHA1/SHA256 and P-256 ECDSA
SHA256.22 real cryptography groups test correct/wrong issuers, changed TBS and
signatures, nonce errors, a strict fixed notBefore cutoff, identifier linkage,
unsupported paths/extensions and actual lazy-parser errors. Synthetic keys are
ephemeral; no source or peer certificate bytes are published.

Original13G36 certificate/path/helper instructions separately qualify9 routing
and error-normalization cases with explicitly fake digest/key-verification
providers. This is distinct from the real mathematical tests.6 additional
original basic-path cases qualify selected explicit-anchor exemptions and leaf
checks under synthetic metadata.2 original calendar-construction controls
establish Gregorian/GMT0/yMd2006-05-31 API arguments; the time conversion boundary
remains synthetic. Apple's CFAbsoluteTime contract specifies the intended cutoff.
No generic TLS expiry check is substituted for the observed iAP options.

Another9 original post-no-handler fallback/getter cases identify the private
1.2.840.113635.100.6.* unknown-critical exception. They do not execute the DER or
handler-dictionary parser and do not define private extension semantics. The
narrow direct helper continues rejecting every critical leaf extension.

The actual caller still requires native SecTrustEvaluate result4. Direct
certificate/nonce math and these qualified facets do not supply complete chain
trust, accessory identity, adapter admission, current, GPIO or VBUS authority.
All returned evidence leaves trust/identity unknown and authorization false.
Actual source anchor identities are reported separately; an arbitrary supplied
issuer that verifies mathematically never becomes trusted by that success.

The v2 helper fixes a real lazy-parser failure: a signed synthetic certificate
with an unsupported x400Address SAN loads as a Certificate but decoding its
extensions raises UnsupportedGeneralNameType. This specific exception now yields
rejected evidence for both leaf and issuer; malformed SAN DER also rejects.
The old exception was independently reproduced. No broad exception catch or
fallback trust result was added.

Reproduction, immutable-source hashes and explicit provider boundaries are in
[DIRECT_EVIDENCE.md](../../experiments/p105-iap-direct-evidence/DIRECT_EVIDENCE.md)
and [native path evidence](../../experiments/p105-iap-native-policy/path-closure/BASIC_PATH.md).
The normal test runner executes real crypto only; optional original-input
replays require locally supplied SHA-pinned official cache/securityd files and
an existing Unicorn runtime. No download, install, IPC or device connection is
performed by these components. The normal kernel and RAM image do not activate
them.
