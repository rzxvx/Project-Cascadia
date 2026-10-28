# Original 13G36 iAP verification control flow, synthetic Security backend

Separate evidence group; the previously frozen authentication parser remains
unchanged. This group executes original verification instructions, **not real
cryptography or certificate validation**, and grants no authentication,
Camera identity, current, rail or USB role.

Source: caller-provided official dyld cache SHA256
`4f5b5d73244c53fd0bdc5b8012106b10d0dd7607cc850bf7c8f9e9ef3a3f733b`.
`emulate-iap-verify13.py` stream-hashes that entire file first, locates the
actual IAPAuthentication image in the cache image table, parses original
Mach-O nlist/indirect-symbol tables and checks the imported Security API
names against their real stubs. No extracted Apple binary is published.

The original `0x31a0e870` method runs with a 6000-instruction budget per case.
Unmapped reads map only pages of the pinned cache, never device addresses.
Synthetic certificate parser, CF objects, Security/crypto APIs and an
explicitly synthetic already-initialized optional-anchor/once context are
mocked. The public fixture contains only fake buffers and semantic call
summaries, no certificate/private-key bytes. The source's version-specific
anchor initializers and actual trust implementation are not executed.

Eleven controls pass:

| Input/control | Original result / observable call |
| --- | --- |
| v2, synthetic trust4 and verify0 | result1; RawVerify padding8002, nonce20, signature128 |
| v3, synthetic trust4 and verify0 | result1; RawVerify padding4000, nonce32, signature64 |
| v2/v3 synthetic verification failure | result0; RawVerify called and its error rejected |
| trust result1 (Proceed) | result0; no RawVerify |
| v2/v3 trust result2 | result0; no RawVerify |
| trust API failure | result0; no RawVerify |
| missing public key | result0; no RawVerify |
| certificate parser failure | result0; no RawVerify |
| unsupported version4 | result0; no RawVerify |

Actual branch evidence: source creates `SecPolicyCreateiAP`, builds a chain
and anchor array, invokes `SecTrustSetAnchorCertificates`, accepts only API
success with trust result4, obtains `SecTrustCopyPublicKey`, and passes nonce
and remote signature to `SecKeyRawVerify`. Source success requires verify
API return0. These controls prove ordering and failure handling, not the
truth of any fake certificate, key, trust, signature or accessory.
The separate original AuthCPAID protocol proof sends nonce material to a
remote signer and retrieves response bytes; no private host signing call
exists in this audited verification/exchange path. Other iAP modes remain
outside that claim.

Modern primary Apple definitions qualify API enum semantics:
[SecKey.h](https://github.com/apple-oss-distributions/Security/blob/db15acbe6a7f257a859ad9a3bb86097bfe0679d9/keychain/headers/SecKey.h)
defines8002 as PKCS1/SHA1 and4000 as raw ECDSA signature format;
[SecTrust.h](https://github.com/apple-oss-distributions/Security/blob/db15acbe6a7f257a859ad9a3bb86097bfe0679d9/trust/headers/SecTrust.h)
defines result4 as Unspecified. These definitions do not establish a matching
13G36 root store or certificate policy implementation.

Apple's current
[SecPolicyPriv.h](https://github.com/apple-oss-distributions/Security/blob/db15acbe6a7f257a859ad9a3bb86097bfe0679d9/trust/headers/SecPolicyPriv.h)
and
[SecPolicy.c](https://github.com/apple-oss-distributions/Security/blob/db15acbe6a7f257a859ad9a3bb86097bfe0679d9/OSX/sec/Security/SecPolicy.c)
show why generic TLS trust is unsuitable: iAP uses basic certificate checks,
a leaf name prefix `IPA_`, a special lower date bound and caller-provided
iAP intermediates. This newer public implementation is supporting API
context; the native 13G36 Security policy and version3 anchor initialization
still require independent qualification. No new verifier is activated.

Reproduce offline with Unicorn installed:

```
python3 emulate-iap-verify13.py DYLD_SHARED_CACHE_ARMV7
```

The portable synthetic oracle is `iap-verify13-oracle.json`; its exact source
and fixture hashes are in `manifest-crypto-boundary.json`. Matching iAP
anchors, extensions/key type, nonce freshness and version-specific response
format must be established before implementing an actual Linux verifier.
A successful packet exchange or copied `Authenticated=true` is insufficient.
