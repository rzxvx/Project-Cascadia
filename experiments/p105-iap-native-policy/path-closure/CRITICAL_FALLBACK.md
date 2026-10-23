# Native unregistered-critical-extension fallback

The actual 13G36 certificate parser has a namespace-specific fallback for a
critical extension that has no registered handler. The original instructions
`0x20c98a86..0x20c98ab2` require an OID byte pointer, a length of at least nine
bytes and the eight-byte literal `2a864886f7636406` before preserving the unknown
critical flag. This is the public namespace 1.2.840.113635.100.6.*; the ninth or
later octets are not inspected in this fallback.

A different or shorter unknown critical OID sets certificate byte `+0x9c`.
A noncritical extension leaves it unchanged. The original named
`SecCertificateHasUnknownCriticalExtension` at `0x20c9b6dc` reads that byte.

Nine bounded original-code cases exercise the fallback and actual getter:
private .36, another private trailing value, a longer encoding, the exact
8-byte prefix, a different namespace, an unrelated OID, a noncritical OID,
already-latched state, and synthetic internal null metadata. The last case is
an internal-metadata negative control, not a claim that the preceding DER parser
accepts null OIDs. The comparison provider is a synthetic bounded memcmp; its
operands and the exact actual source literal are checked.

This is **not** a complete DER decoder or extension classifier. Entry is after
the preceding extension-handler dictionary returned no parser; registered
extension handlers are not replaced with the fallback. Their syntax and
semantic validation are not tested here. No complete leaf/chain, CF dictionary,
trust evaluation, certificate signature or authorization runs in this oracle.

The source-embedded v3 anchor has critical OID1.2.840.113635.100.6.36. If it reaches
this unregistered fallback, its OID satisfies the native exception; independently,
the already qualified anchored-root basic path does not query the unknown
critical getter. Neither result defines the private extension's content or
proves every path's treatment. The stricter experimental direct-evidence
component continues denying all critical **leaf** extensions and never returns
trusted identity or electrical authority.

The contemporary Apple OSS implementation contains an `isAppleExtensionOID`
check with the same public namespace prefix in the unknown-handler branch:
[SecCertificate.c](https://github.com/apple-oss-distributions/Security/blob/4f36c5f47ec9f03c406c088d2c427bd5582b2631/OSX/sec/Security/SecCertificate.c).
That source is supporting interpretation, not an authenticated exact iOS build
match; the pinned original instructions determine these results. This fallback
does not remove certificate signature verification or create a signing key.

## Reproduce and input provenance

```sh
python3 emulate-critical-branch.py CACHE SECURITYD > critical-oracle.json
```

Requires the existing Unicorn runtime and private official 13G36 inputs:
cache SHA256 `4f5b5d73244c53fd0bdc5b8012106b10d0dd7607cc850bf7c8f9e9ef3a3f733b`,
securityd SHA256 `1c12fbb48ead1edd3dbf2f4b0d635b45452573499fd9592562bbd76e40b56097`.
Each request is bounded to100 fallback instructions plus20 getter instructions.
It executes original bytes only inside fake RAM; no native executable, system
certificate store, IPC, file beyond the supplied source inputs, network, device,
Apple certificate byte fixture or power operation is provided.
