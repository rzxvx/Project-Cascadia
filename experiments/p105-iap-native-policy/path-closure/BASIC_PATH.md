# Actual13G36 direct anchored basic path-processing facets

Six bounded cases execute the entire original securityd
`SecPolicyCheckBasicCertificateProcessing` at `0x31904`, its actual policy-tree
functions and actual `SecPVCSetResultForced` body. This extends the separate
46-case source-policy group; it does not replace full certificate path building,
certificate signatures, native CMS/certificate parsing or complete trust.

Inputs are caller-owned immutable official13G36 files, hash checked before use:
cache SHA `4f5b5d73244c53fd0bdc5b8012106b10d0dd7607cc850bf7c8f9e9ef3a3f733b`,
securityd SHA `1c12fbb48ead1edd3dbf2f4b0d635b45452573499fd9592562bbd76e40b56097`.
Executable nlists identify the named original entry. The synthetic source loader
resolves imported globals from actual named cache exports and preserves the
original nlist Thumb bit in function-pointer bindings. It also resolves the
source stack-canary variable from libsystem components; no live daemon startup
or system entropy initialization is claimed. Global/data bindings and API-name
resolution do not create trust in synthetic provider values.

The model provides a direct two-certificate path, leaf index0 and explicit anchor
index1. Certificate/path/CF metadata getters are synthetic; name constraints,
certificate policies, policy mappings and policy constraints are absent. Heap
allocation and CF array operations are bounded synthetic providers. Policy-tree
allocation/traversal/pruning instructions themselves run from original source.
This proof does not claim CF object/refcount correctness outside that model.
No Apple bytes or real peer data are embedded.

| Synthetic control | Actual native result flag |
| --- | --- |
| Baseline direct explicit-anchor path | remains true |
| Root has unknown critical flag, leaf does not | remains true; root critical getter never requested |
| Leaf has unknown critical flag | clears false |
| Invalid leaf validity metadata, no ValidLeaf option | remains true; original optional-failure setter ignores absent key |
| Same invalid leaf plus ValidLeaf option | clears false |
| Weak leaf metadata | clears false |

All final values and getter selection are independently asserted. The root
unknown-critical control changes a provider flag; it does **not** run the native
DER parser on the actual embedded v3 certificate or establish that parser's
classification. It proves the explicit anchored-final-certificate exemption in
this tested basic-processing path. In contrast, an unknown critical **leaf**
flag is evaluated and rejected. The embedded root's private extension therefore
must not be used as an excuse to ignore unknown leaf/intermediate extensions.

This strengthens the narrow observed non-expiry policy fact, but does not claim
that every native chain-builder/issuer/exception path lacks time handling.
The independent fixed NotValidBefore rule, exact cutoff conversion, source
configuration, complete path policy and actual accessory chain remain separate.
A stronger local validity policy could be named explicitly; generic TLS success
is not native-iAP equivalence.

Original signature checks and chain-builder anchoring are external prerequisites,
not fake successes in this method. Even the positive result flag under synthetic
metadata is not `certificate_chain_trusted`. No authentication property,
accessory identity, USB/current/VBUS grant or source code activation follows.

Reproduce with existing Unicorn:

```
python3 emulate-basic.py DYLD_CACHE SECURITYD
```

The portable semantic fixture is `basic-oracle.json`. Unsupported provider APIs
fail loudly; native execution has a2000-instruction budget per case. No network,
package installation, device access, IPC/service connection or native daemon
execution is performed. Complete policy coverage and signed peer inputs remain
required before an offline trust implementation is eligible for review.
