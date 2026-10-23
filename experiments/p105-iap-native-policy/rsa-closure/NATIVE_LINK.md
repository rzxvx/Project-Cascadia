# Native saved-chain key extraction and real RSA link verification

The original named `SecCertificatePathVerify` at0x20c9e638 is exercised with a
source-owned two-entry synthetic path. It invokes the original
`SecCertificatePathCopyPublicKeyAtIndex` at0x20c9e488 for the issuer, which passes
its actual selected algorithm/parameters/raw public-key fields into native key
creation. Named `SecKeyCreateRSAPublicKey`, real corecrypto public DER decoding
and the original RSA public descriptor all run.

The certificate-signature helper0x20c9a9d0 and named `SecKeyDigestAndVerify` at
0x20ca8bcc produce the original SHA256 DigestInfo. The SHA256 digest itself is
computed by a Python hashlib provider, explicitly **not native hash execution**.
Then the original RSA public verify callback0x20cb18e4 executes named corecrypto
`ccrsa_pub_crypt` at0x204407e4. Its original integer modular exponentiation runs.
The Security callback performs the original PKCS1-v1.5 block/padding checks and
compares the actual recovered bytes with the supplied DigestInfo. No synthetic
"signature valid" provider or replaced RSA verification result exists.

This closes an earlier fixture limitation: previously the key verifier returned
an arbitrary success/failure while original code only proved dispatch/error
ordering. Here an actual public RSA fixture determines the native outcome:

| Request | Native result | Completed links |
| --- | --- | --- |
| Valid synthetic RSA/SHA256 TBS signature |0|1|
| One signature-byte corrupted |1|0|
| Valid signature, issuer RSA parameters absent, no later parent |0|1|

The third case proves this no-later-parent RSA extraction path can construct the
key with absent parameters; it does not generalize to ECDSA/inherited parameters.
Different parent algorithms/parameter inheritance, original path construction,
issuer-name discovery or a real captured accessory chain are outside this group.

## Synthetic public fixture and independent check

`synthetic-public-link.json` contains a1024-bit public modulus, exponent65537,
PKCS1 public DER, a public text TBS primitive and its public RSA/SHA256 signature.
An ephemeral synthetic private key created those values in host process memory;
no private factors/key encoding, Apple certificate/key bytes, accessory identity
or captured device reply were saved. The public TBS primitive is **not parsed
X509 certificate DER**. Source-owned certificate field objects supply it to the
native signature API at its known internal boundary.

An independent cryptography41.0.7 check verifies that public fixture and rejects
the corrupted signature and modified TBS. A third test checks the frozen native
outcomes/actual RSA call evidence and instruction bounds. It does not pretend
that comparing an oracle replaces executing its native emulator.

## Explicit remaining boundary

The path and certificate field objects are synthetic. Original certificate DER
parsing, normalized-name/issuer matching, complete native policy enforcement,
path discovery/anchoring, private critical extensions and `SecTrustEvaluate`
are not run. Result0 here means native **link math**, not SecTrust result4.
The output keeps certificate_chain_trusted=None and authorization_granted=False.

CF type/object/heap/release and bounded libc memory helpers are fake. Imported
memory helper names are checked against actual corecrypto indirect symbol tables.
No original CF refcount/destructor or process/dynamic-linker initialization is
claimed. Unknown import calls fail; each request starts in fresh fake RAM, has
at most2,000,000 instructions and bounded allocations/helper lengths. ARM/Thumb
returns preserve the actual LR instruction-state bit. There is no hardware,
network, system trust store, IPC, clock/rate, GPIO, current, rail or role interface.

## Reproduce

```sh
python3 emulate-native-link.py CACHE SECURITYD > native-link-oracle.json
python3 test_public_link.py
```

First command needs the existing Unicorn runtime plus the private exact official
13G36 files; second needs cryptography41.0.7 and only public source/fixture files.
No installation is performed. The public fixture SHA is checked before use.
Native input SHA256 pins:
cache `4f5b5d73244c53fd0bdc5b8012106b10d0dd7607cc850bf7c8f9e9ef3a3f733b`;
securityd `1c12fbb48ead1edd3dbf2f4b0d635b45452573499fd9592562bbd76e40b56097`.

Primary PKCS1 algorithm reference: [RFC8017](https://www.rfc-editor.org/rfc/rfc8017.txt).
Contemporary Apple [SecCertificate.c](https://github.com/apple-oss-distributions/Security/blob/4f36c5f47ec9f03c406c088d2c427bd5582b2631/OSX/sec/Security/SecCertificate.c)
supports signature API interpretation but is not assumed an exact shipped13G36
build match. The actual pinned source and genuine public cryptographic fixture
determine these controlled results.
