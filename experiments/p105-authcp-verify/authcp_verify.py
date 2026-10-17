#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Offline AuthCPAID mathematical signature verification; never an authorization.

Inputs are immutable byte snapshots from a separately owned exchange. No bus,
network, certificate-trust store, private-key loading, or power/role interface.
"""
from dataclasses import asdict, dataclass
import argparse
import hashlib
import json
import os
import stat
from pathlib import Path

from cryptography import x509
from cryptography.exceptions import InvalidSignature, UnsupportedAlgorithm
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, padding, rsa, utils

MAX_KEY_BYTES = 16384
P256_ORDER = int("ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551", 16)


@dataclass(frozen=True)
class Verification:
    signature_valid: bool
    reason: str
    scheme: str | None = None
    public_key_sha256: str | None = None
    certificate_sha256: str | None = None
    certificate_chain_trusted: None = None
    accessory_identity_verified: None = None
    authorization_granted: bool = False


def _one_der_sequence(data: bytes) -> bool:
    """Require one bounded, definite, minimally encoded outer SEQUENCE."""
    if len(data) < 2 or data[0] != 0x30:
        return False
    first = data[1]
    if first < 128:
        return len(data) == first + 2
    count = first & 127
    if not 1 <= count <= 2 or len(data) < 2 + count or not data[2]:
        return False
    length = int.from_bytes(data[2:2 + count], "big")
    return length >= 128 and length.bit_length() > 8 * (count - 1) and len(data) == 2 + count + length


def verify(version: int, nonce: bytes, signature: bytes, key_data: bytes,
           key_format: str = "spki") -> Verification:
    """Verify against an explicitly supplied public key, without trusting it.

    v2: RSA-1024 PKCS1-v1.5 SHA1-DigestInfo over the 20 input bytes.
    v3 supported profile: P-256 ECDSA over the 32 input bytes, raw BE r||s.
    Prehashed is essential: these source nonce bytes are NOT hashed again.
    P-256 is this helper's supported key profile, not an accessory allowlist.
    """
    if (type(version) is not int or version not in (2, 3) or
            type(nonce) is not bytes or type(signature) is not bytes or
            type(key_data) is not bytes or type(key_format) is not str or key_format not in ("spki", "certificate")):
        return Verification(False, "invalid_input")
    nonce_size, signature_size = (20, 128) if version == 2 else (32, 64)
    if len(nonce) != nonce_size or len(signature) != signature_size:
        return Verification(False, "invalid_length")
    if not 0 < len(key_data) <= MAX_KEY_BYTES or not _one_der_sequence(key_data):
        return Verification(False, "invalid_key_encoding")
    certificate_hash = None
    try:
        if key_format == "certificate":
            # Parsing/using this leaf's public key does not verify its signature,
            # issuer, validity dates, iAP policy, chain, or accessory identity.
            certificate = x509.load_der_x509_certificate(key_data)
            if certificate.public_bytes(serialization.Encoding.DER) != key_data:
                return Verification(False, "invalid_key_encoding")
            public_key = certificate.public_key()
            certificate_hash = hashlib.sha256(key_data).hexdigest()
        else:
            public_key = serialization.load_der_public_key(key_data)
        spki = public_key.public_bytes(serialization.Encoding.DER,
                                      serialization.PublicFormat.SubjectPublicKeyInfo)
        if key_format == "spki" and spki != key_data:
            return Verification(False, "invalid_key_encoding")
    except (ValueError, TypeError, UnsupportedAlgorithm):
        return Verification(False, "invalid_key_encoding")
    fingerprint = hashlib.sha256(spki).hexdigest()
    scheme = "rsa1024-pkcs1v15-prehashed-sha1" if version == 2 else "p256-ecdsa-prehashed32-raw-rs"
    def result(valid: bool, reason: str) -> Verification:
        return Verification(valid, reason, scheme, fingerprint, certificate_hash)
    try:
        if version == 2:
            if not isinstance(public_key, rsa.RSAPublicKey) or public_key.key_size != 1024:
                return result(False, "unsupported_key_profile")
            public_key.verify(signature, nonce, padding.PKCS1v15(), utils.Prehashed(hashes.SHA1()))
        else:
            if not isinstance(public_key, ec.EllipticCurvePublicKey) or not isinstance(public_key.curve, ec.SECP256R1):
                return result(False, "unsupported_key_profile")
            r = int.from_bytes(signature[:32], "big")
            s = int.from_bytes(signature[32:], "big")
            if not 0 < r < P256_ORDER or not 0 < s < P256_ORDER:
                return result(False, "invalid_signature_encoding")
            # SHA256 names the 32-byte prehash contract for the backend; it
            # does not introduce hashing or claim a SHA256 nonce producer.
            der_signature = utils.encode_dss_signature(r, s)
            public_key.verify(der_signature, nonce, ec.ECDSA(utils.Prehashed(hashes.SHA256())))
    except InvalidSignature:
        return result(False, "invalid_signature")
    except (ValueError, TypeError, UnsupportedAlgorithm):
        return result(False, "verification_backend_error")
    return result(True, "mathematically_valid_untrusted_key")


def _read_bounded(path: Path, limit: int) -> bytes:
    descriptor = os.open(path, os.O_RDONLY | os.O_CLOEXEC | os.O_NONBLOCK | os.O_NOFOLLOW)
    with os.fdopen(descriptor, "rb") as stream:
        if not stat.S_ISREG(os.fstat(stream.fileno()).st_mode):
            raise ValueError("input must be a regular file")
        data = stream.read(limit + 1)
    if len(data) > limit:
        raise ValueError("input exceeds size limit")
    return data


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", type=int, choices=(2, 3), required=True)
    parser.add_argument("--nonce", type=Path, required=True)
    parser.add_argument("--signature", type=Path, required=True)
    parser.add_argument("--public-key", type=Path, required=True)
    parser.add_argument("--key-format", choices=("spki", "certificate"), default="spki")
    args = parser.parse_args()
    try:
        outcome = verify(args.version, _read_bounded(args.nonce, 32),
                         _read_bounded(args.signature, 128),
                         _read_bounded(args.public_key, MAX_KEY_BYTES), args.key_format)
    except (OSError, ValueError):
        outcome = Verification(False, "input_read_error")
    print(json.dumps(asdict(outcome), sort_keys=True))
    return 0 if outcome.signature_valid else 1


if __name__ == "__main__":
    raise SystemExit(main())
