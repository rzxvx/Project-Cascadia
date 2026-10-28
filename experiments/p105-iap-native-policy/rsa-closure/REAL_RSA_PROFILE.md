# Native RSA public-key decoding through weak-root enforcement

This group closes a specific limitation of the earlier weak-key checker fixture:
it no longer replaces public-key creation, block-size or algorithm getters with
synthetic threshold values. The original 13G36 routines decode a synthetic
public RSA DER object, initialize the native key representation, read its real
native descriptor and execute the weak-key consumer.

The synthetic certificate object contains only the expected public-key fields;
**the original certificate DER parser is not executed**. Its values are a public
PKCS1 RSA modulus/exponent fixture, OID rsaEncryption and NULL parameters. The
modulus `2^(bits-1) | 0x12345` is a source-owned odd public test integer; no private
key, factorization, Apple key/certificate bytes or signing operation exists in
this fixture. It does not demonstrate a physically supplied accessory chain.

Actual source chain executed:

1. `SecCertificateIsWeak` at `0x20c9b510` → `SecCertificateCopyPublicKey` at
   `0x20c9b4bc` → generic key creation `0x20ca84bc`.
2. Named `SecKeyCreateRSAPublicKey` at `0x20cb1f9c` uses the actual RSA public
   descriptor `0x36b033e8` and key constructor `0x20ca8638`.
3. Original descriptor initializer `0x20cb1760` invokes the named corecrypto
   `ccder_decode_rsa_pub_n` at `0x2044d80c`, allocates context storage and invokes
   `ccrsa_import_pub` at `0x2044db28`. Original public DER decoding and internal
   public-key initialization arithmetic run; no replacement key decoder or
   synthetic bit-size getter is supplied.
4. `SecKeyGetBlockSize` at `0x20ca8860` invokes the original descriptor's block
   getter `0x20cb1e38`; `SecKeyGetAlgorithmIdentifier` at `0x20ca7ed8` uses the
   descriptor's actual fallback to RSA algorithm1 (slot+0x34 is zero).
5. Two further controls execute the original `SecPVCParentCertificateChecks` at
   `0x33980` plus the real `SecCertificateIsWeak` path and actual result setter.
   With the synthetic WeakRoot option present, a 512-bit root clears its result
   flag; a 1024-bit root preserves it. Path membership, anchored status and the
   other CF/certificate metadata getters are synthetic as listed below.

Seven cases pass:

| Public test modulus | Native block bytes | Native weak | Scope |
| --- | --- | --- | --- |
| 512 bits | 64 | true | Real native key decode/size/algo |
| 1023 bits | 128 | false | Actual ceil-byte-size threshold |
| 1024 bits | 128 | false | Actual threshold |
| 2048 bits | 256 | false | Larger source-owned public fixture |
| Truncated 1024-bit DER | no successful key | true | Original decode failure |
| 512 bits, parent-processing entry | 64 | true / result flag cleared | Original WeakRoot consumer |
| 1024 bits, parent-processing entry | 128 | false / result flag preserved | Original WeakRoot consumer |

The important units are **public-key block bytes**, not an assumed exact bit
minimum: 1023 bits occupy128 bytes and pass this native weak test. The separate
experimental nonce verifier deliberately supports exactly RSA-1024 and is a
narrower profile. This does not qualify ECDSA curves, entire native path policy,
root/key legitimacy, nonce/certificate signatures or any physical permission.

## Explicit synthetic boundaries

- CF type initialization returns an arbitrary synthetic type ID. CF object
  creation and calloc provide bounded, zeroed fake memory. Each request starts
  from fresh fake RAM/source mappings; this is not a CF lifetime or heap test.
- CFRelease is a bounded observer; native destructor/refcount semantics do not
  run. Failure logging is mocked, without creating real logging/IPC endpoints.
- Named corecrypto libc import veneers for memcpy/memmove/memset/memset_s/memcmp
  and the Security libc memory veneers perform bounded fake-RAM operations.
  Their actual names/addresses are verified from the original Mach-O indirect
  symbol table. Native arithmetic and RSA DER decoders are not mocked.
- Parent-processing controls supply a two-entry synthetic path, anchored status,
  fake option dictionary containing WeakRoot, CF array selection and certificate
  validity/basic-constraints metadata. The WeakRoot branch, native key creation,
  descriptor getters and original result setter are real instructions.
- Unknown corecrypto or parent API calls fail the fixture immediately. No native
  process, dynamic-linker startup, file/device/network request or trust store is
  provided. Calls return across ARM/Thumb ABI boundaries using the original LR
  state; no guessed CPU capabilities or platform commpage remains mapped.

An earlier debugging hypothesis called the key setup "Montgomery" initialization;
that specific arithmetic label is not established by this group. The proved
scope is original public-key initialization and getters, identified by actual
function entry/callsites and named RSA import APIs.

## Reproduce

```sh
python3 emulate-real-rsa-key.py CACHE SECURITYD > real-rsa-oracle.json
```

Requires the existing Unicorn runtime; maximum1,000,000 instructions per request,
heap allocations/memory helpers≤4096 bytes and object-space bounds checked.
The most expensive 2048-bit request completes in632,536 instructions; no
unbounded loop is permitted. Inputs are private read-only official13G36 files:
cache SHA256 `4f5b5d73244c53fd0bdc5b8012106b10d0dd7607cc850bf7c8f9e9ef3a3f733b`;
securityd SHA256 `1c12fbb48ead1edd3dbf2f4b0d635b45452573499fd9592562bbd76e40b56097`.

A primary contemporaneous implementation describes public-key construction and
weak-key block-size checks:
[Apple SecCertificate.c](https://github.com/apple-oss-distributions/Security/blob/4f36c5f47ec9f03c406c088d2c427bd5582b2631/OSX/sec/Security/SecCertificate.c).
It is supporting interpretation; exact matching to the shipped iOS build is
not assumed. The fixture qualifies its source functions/descriptor/import names
from the actual pinned inputs, not from address guesses or the comparison tree.
