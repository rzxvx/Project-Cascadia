# SPDX-License-Identifier: GPL-2.0
"""Bounded offline certificate-link/nonce math and separately qualified facets.

This is not a SecTrust replacement, a trust store, a path builder, an accessory
identity verifier, or an electrical authorization. There is no I/O interface.
"""
from dataclasses import dataclass
from datetime import datetime
from types import MappingProxyType
import hashlib

from cryptography import x509
from cryptography.exceptions import InvalidSignature, UnsupportedAlgorithm
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, padding, rsa
from cryptography.x509.oid import ExtensionOID, NameOID
from authcp_verify import verify

MAX_DER = 16384
# Proven original Gregorian / GMT+0 API parameters, not a measured clock value.
NOT_BEFORE_CUTOFF = datetime(2006, 5, 31)
SOURCE_ANCHORS = MappingProxyType({
    2: frozenset((
        '884bf87d549f2de7c4fceb4284a8cff9f8b817f36dcf77a40660b33b8afefd0d',
        '2eb53dbefdc6f5e7e13d6690fc8cbf9cf4c3f7e501feafb3bc052431dbc83180')),
    3: frozenset((
        '81da17dabaf55012d1ea3946c02c3a423405bc290403cc54cdb88bfe95677472',)),
})
# Source-pinned anchor identity is reported, never taken as electrical authority.
KNOWN_LEAF_EXTENSIONS = frozenset((
    ExtensionOID.BASIC_CONSTRAINTS, ExtensionOID.KEY_USAGE,
    ExtensionOID.SUBJECT_KEY_IDENTIFIER, ExtensionOID.AUTHORITY_KEY_IDENTIFIER))
SIG_ALGORITHMS = {
    '1.2.840.113549.1.1.5': ('rsa-pkcs1v15-sha1', hashes.SHA1),
    '1.2.840.113549.1.1.11': ('rsa-pkcs1v15-sha256', hashes.SHA256),
    '1.2.840.10045.4.3.2': ('ecdsa-sha256', hashes.SHA256),
}


@dataclass(frozen=True)
class Evidence:
    certificate_signature_valid: bool = False
    nonce_signature_valid: bool = False
    supported_facets_pass: bool = False
    reason: str = 'invalid_input'
    certificate_signature_scheme: str | None = None
    leaf_sha256: str | None = None
    anchor_sha256: str | None = None
    source_anchor_matches_version: bool = False
    identifier_comparison_performed: bool = False
    certificate_chain_trusted: None = None
    accessory_identity_verified: None = None
    authorization_granted: bool = False


def _tlv(data, pos, limit):
    """One definite, minimally encoded single-byte-tag TLV within its parent."""
    if pos < 0 or limit > len(data) or pos + 2 > limit:
        raise ValueError('truncated DER')
    tag = data[pos]
    if tag & 31 == 31:
        raise ValueError('unsupported high tag')
    first = data[pos + 1]
    header = 2
    if first & 128:
        n = first & 127
        if not 1 <= n <= 2 or pos + 2 + n > limit or not data[pos + 2]:
            raise ValueError('invalid DER length')
        length = int.from_bytes(data[pos + 2:pos + 2 + n], 'big')
        if length < 128 or length.bit_length() <= 8 * (n - 1):
            raise ValueError('nonminimal DER length')
        header += n
    else:
        length = first
    content, end = pos + header, pos + header + length
    if end > limit:
        raise ValueError('DER beyond parent')
    return tag, content, end


def _children(data, content, end, max_count=12):
    rows = []
    pos = content
    while pos < end:
        if len(rows) >= max_count:
            raise ValueError('DER child count bound')
        tag, start, stop = _tlv(data, pos, end)
        rows.append((tag, pos, start, stop))
        pos = stop
    return rows


def _certificate(data):
    if type(data) is not bytes or not 0 < len(data) <= MAX_DER:
        raise ValueError('invalid snapshot')
    tag, content, end = _tlv(data, 0, len(data))
    if tag != 0x30 or end != len(data):
        raise ValueError('one certificate required')
    outer = _children(data, content, end, 3)
    if len(outer) != 3 or [v[0] for v in outer] != [0x30, 0x30, 0x03]:
        raise ValueError('invalid certificate structure')
    tbs = _children(data, outer[0][2], outer[0][3])
    serial = 1 if tbs and tbs[0][0] == 0xa0 else 0
    if len(tbs) < serial + 6 or tbs[serial][0] != 0x02 or tbs[serial + 1][0] != 0x30:
        raise ValueError('invalid TBS')
    inner = tbs[serial + 1]
    alg = outer[1]
    if data[inner[1]:inner[3]] != data[alg[1]:alg[3]]:
        raise ValueError('mismatched signature algorithms')
    bit = outer[2]
    if bit[2] == bit[3] or data[bit[2]] != 0:
        raise ValueError('invalid signature bit string')
    cert = x509.load_der_x509_certificate(data)
    if cert.public_bytes(serialization.Encoding.DER) != data:
        raise ValueError('invalid certificate encoding')
    # Only supported public algorithm encodings. Reject RSA-PSS/extra parameters
    # rather than silently select PKCS1v1.5 from an issuer key alone.
    entries = _children(data, alg[2], alg[3], 2)
    if not entries or entries[0][0] != 0x06:
        raise ValueError('invalid algorithm')
    oid = cert.signature_algorithm_oid.dotted_string
    if oid not in SIG_ALGORITHMS:
        raise UnsupportedAlgorithm('signature algorithm outside supported subset')
    if oid.startswith('1.2.840.113549.'):
        if len(entries) == 2:
            null = entries[1]
            if null[0] != 0x05 or null[2] != null[3]:
                raise ValueError('unsupported RSA parameters')
    elif len(entries) != 1:
        raise ValueError('unsupported ECDSA parameters')
    # Force lazy extension decoding now: duplicate/malformed extensions reject.
    tuple(cert.extensions)
    return cert


def _extension(cert, oid):
    try:
        return cert.extensions.get_extension_for_oid(oid).value
    except x509.ExtensionNotFound:
        return None


def examine(version, leaf_der, anchor_der, nonce, nonce_signature, intermediates=()):
    """Return partial evidence from immutable caller-owned snapshots.

    One direct leaf→explicit issuer only. Supplied issuer is NOT trusted merely
    because a certificate signature verifies. No wall-clock/expiry/TLS policy
    substitute, chain discovery, preferences override or result4 fabrication.
    """
    if (type(version) is not int or version not in (2, 3) or
            type(intermediates) is not tuple or intermediates or
            type(nonce) is not bytes or type(nonce_signature) is not bytes):
        return Evidence(reason='unsupported_path_or_input')
    try:
        leaf = _certificate(leaf_der)
        anchor = _certificate(anchor_der)
        issuer_key = anchor.public_key()
        scheme, hash_class = SIG_ALGORITHMS[leaf.signature_algorithm_oid.dotted_string]
        leaf_hash = hashlib.sha256(leaf_der).hexdigest()
        anchor_hash = hashlib.sha256(anchor_der).hexdigest()
        source_match = anchor_hash in SOURCE_ANCHORS[version]
        def result(reason, cert_ok=False, nonce_ok=False, facets=False, compared=False):
            return Evidence(cert_ok, nonce_ok, facets, reason, scheme, leaf_hash,
                            anchor_hash, source_match, compared)
        if scheme.startswith('rsa-'):
            if not isinstance(issuer_key, rsa.RSAPublicKey) or not 1024 <= issuer_key.key_size <= 4096:
                return result('unsupported_issuer_key_profile')
            issuer_key.verify(leaf.signature, leaf.tbs_certificate_bytes,
                              padding.PKCS1v15(), hash_class())
        else:
            if (not isinstance(issuer_key, ec.EllipticCurvePublicKey) or
                    not isinstance(issuer_key.curve, ec.SECP256R1)):
                return result('unsupported_issuer_key_profile')
            issuer_key.verify(leaf.signature, leaf.tbs_certificate_bytes,
                              ec.ECDSA(hash_class()))
        # This exact name equality is a deliberately narrower admission than
        # native normalized-name discovery; it is not its implementation.
        if leaf.issuer != anchor.subject:
            return result('unsupported_issuer_name_link', cert_ok=True)
        if any(ext.oid not in KNOWN_LEAF_EXTENSIONS for ext in leaf.extensions):
            return result('unsupported_leaf_extension', cert_ok=True)
        if any(ext.critical for ext in leaf.extensions):
            # Original known-critical classification is not fully qualified.
            return result('unsupported_leaf_critical_profile', cert_ok=True)
        cn = leaf.subject.get_attributes_for_oid(NameOID.COMMON_NAME)
        if (len(cn) != 1 or not cn[0].value.isascii() or
                not cn[0].value.startswith('IPA_')):
            return result('native_common_name_facet_failed', cert_ok=True)
        if leaf.not_valid_before <= NOT_BEFORE_CUTOFF:
            return result('native_not_before_facet_failed', cert_ok=True)
        ski = _extension(anchor, ExtensionOID.SUBJECT_KEY_IDENTIFIER)
        aki = _extension(leaf, ExtensionOID.AUTHORITY_KEY_IDENTIFIER)
        compared = ski is not None and aki is not None and aki.key_identifier is not None
        if aki is not None and (aki.authority_cert_issuer is not None or
                                aki.authority_cert_serial_number is not None):
            return result('unsupported_authority_identifier_profile', cert_ok=True)
        if compared and ski.digest != aki.key_identifier:
            return result('native_identifier_link_facet_failed', cert_ok=True, compared=True)
        # Leaf key profile and nonce bounds are implemented by pinned verifier;
        # successful profile satisfies the observed native weak-key minima.
        nonce_result = verify(version, nonce, nonce_signature, leaf_der, 'certificate')
        if not nonce_result.signature_valid:
            return result('nonce_' + nonce_result.reason, cert_ok=True, compared=compared)
        return result('partial_evidence_only_native_trust_unqualified', True, True, True, compared)
    except InvalidSignature:
        return Evidence(reason='invalid_certificate_signature')
    except (ValueError, TypeError, UnsupportedAlgorithm, x509.DuplicateExtension,
            x509.UnsupportedGeneralNameType):
        return Evidence(reason='unsupported_or_invalid_certificate')
