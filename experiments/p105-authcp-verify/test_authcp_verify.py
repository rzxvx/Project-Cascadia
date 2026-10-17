#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Real cryptography, synthetic keys generated only in memory; no Apple bytes."""
import dataclasses
import datetime
import hashlib
import unittest
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, padding, rsa, utils
from cryptography.x509.oid import NameOID
import authcp_verify as helper


def spki(key):
    return key.public_key().public_bytes(serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo)


def sign(key, version, nonce, rehash=False):
    algorithm = hashes.SHA1() if version == 2 else hashes.SHA256()
    digest = algorithm if rehash else utils.Prehashed(algorithm)
    if version == 2:
        return key.sign(nonce, padding.PKCS1v15(), digest)
    r, s = utils.decode_dss_signature(key.sign(nonce, ec.ECDSA(digest)))
    return r.to_bytes(32, 'big') + s.to_bytes(32, 'big')


def synthetic_certificate(key):
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, 'Synthetic untrusted test only')])
    now = datetime.datetime(2020, 1, 1)
    return (x509.CertificateBuilder().subject_name(name).issuer_name(name)
            .public_key(key.public_key()).serial_number(42)
            .not_valid_before(now).not_valid_after(now + datetime.timedelta(days=1))
            .sign(key, hashes.SHA256()).public_bytes(serialization.Encoding.DER))


class VerifierTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.keys = {2: rsa.generate_private_key(public_exponent=65537, key_size=1024),
                    3: ec.generate_private_key(ec.SECP256R1())}
        cls.other = {2: rsa.generate_private_key(public_exponent=65537, key_size=1024),
                     3: ec.generate_private_key(ec.SECP256R1())}
        cls.nonces = {2: bytes(range(20)), 3: bytes(range(32))}
        cls.signatures = {v: sign(k, v, cls.nonces[v]) for v, k in cls.keys.items()}

    def verify(self, version, nonce=None, signature=None, key=None, key_format='spki'):
        return helper.verify(version, self.nonces[version] if nonce is None else nonce,
                             self.signatures[version] if signature is None else signature,
                             spki(self.keys[version]) if key is None else key, key_format)

    def untrusted(self, result):
        self.assertIsNone(result.certificate_chain_trusted)
        self.assertIsNone(result.accessory_identity_verified)
        self.assertFalse(result.authorization_granted)

    def test_positive_versions_are_untrusted(self):
        for version in (2, 3):
            with self.subTest(version=version):
                result = self.verify(version)
                self.assertTrue(result.signature_valid)
                self.assertEqual(result.public_key_sha256, hashlib.sha256(spki(self.keys[version])).hexdigest())
                self.untrusted(result)
                with self.assertRaises(dataclasses.FrozenInstanceError):
                    result.signature_valid = False

    def test_rsa_public_exponent_and_digestinfo_independent_control(self):
        # RFC8017 section9.2: inspect recovered encoded message using only
        # Python integer arithmetic + public modulus/exponent, not verify().
        numbers = self.keys[2].public_key().public_numbers()
        recovered = pow(int.from_bytes(self.signatures[2], 'big'), numbers.e, numbers.n).to_bytes(128, 'big')
        digestinfo = bytes.fromhex('3021300906052b0e03021a05000414') + self.nonces[2]
        expected = b'\0\1' + b'\xff'*(128-len(digestinfo)-3) + b'\0' + digestinfo
        self.assertEqual(recovered, expected)
        self.assertNotEqual(recovered[-20:], hashlib.sha1(self.nonces[2]).digest())

    def test_every_signature_byte_corruption_rejected(self):
        for version in (2, 3):
            for offset in range(len(self.signatures[version])):
                value = bytearray(self.signatures[version]); value[offset] ^= 1
                with self.subTest(version=version, offset=offset):
                    self.assertFalse(self.verify(version, signature=bytes(value)).signature_valid)

    def test_every_nonce_byte_corruption_rejected(self):
        for version in (2, 3):
            for offset in range(len(self.nonces[version])):
                value = bytearray(self.nonces[version]); value[offset] ^= 1
                with self.subTest(version=version, offset=offset):
                    self.assertFalse(self.verify(version, nonce=bytes(value)).signature_valid)

    def test_wrong_key_and_extra_hash_rejected(self):
        for version in (2, 3):
            with self.subTest(version=version):
                self.assertFalse(self.verify(version, key=spki(self.other[version])).signature_valid)
                self.assertFalse(self.verify(version, signature=sign(self.keys[version], version, self.nonces[version], True)).signature_valid)

    def test_encoding_lengths_and_mutable_inputs_rejected(self):
        for version in (2, 3):
            n, s, k = self.nonces[version], self.signatures[version], spki(self.keys[version])
            for bad in (b'', n[:-1], n+b'\0'):
                self.assertEqual(self.verify(version, nonce=bad).reason, 'invalid_length')
            for bad in (b'', s[:-1], s+b'\0'):
                self.assertEqual(self.verify(version, signature=bad).reason, 'invalid_length')
            self.assertEqual(helper.verify(version, bytearray(n), s, k).reason, 'invalid_input')
            self.assertEqual(helper.verify(version, n, bytearray(s), k).reason, 'invalid_input')
            self.assertEqual(helper.verify(version, n, s, bytearray(k)).reason, 'invalid_input')
            for bad in (b'', k[:-1], k+b'\0', b'0\x80\0\0', b'0\x82\0\x80'+b'\0'*128, b'0\x83\x01\0\0', b'0'+b'x'*helper.MAX_KEY_BYTES):
                self.assertEqual(self.verify(version, key=bad).reason, 'invalid_key_encoding')
            private = self.keys[version].private_bytes(serialization.Encoding.DER, serialization.PrivateFormat.PKCS8, serialization.NoEncryption())
            self.assertEqual(self.verify(version, key=private).reason, 'invalid_key_encoding')

    def test_wrong_profiles_rejected(self):
        for version in (2, 3):
            self.assertEqual(self.verify(version, key=spki(self.keys[5-version])).reason, 'unsupported_key_profile')
        self.assertEqual(self.verify(2, key=spki(rsa.generate_private_key(public_exponent=65537, key_size=2048))).reason, 'unsupported_key_profile')
        for curve in (ec.SECP256K1(), ec.SECP384R1()):
            self.assertEqual(self.verify(3, key=spki(ec.generate_private_key(curve))).reason, 'unsupported_key_profile')
        for version in (0, 1, 4, True, '3', None):
            self.assertEqual(helper.verify(version, b'', b'', b'').reason, 'invalid_input')
        self.assertEqual(helper.verify(3, self.nonces[3], self.signatures[3], spki(self.keys[3]), 'pem').reason, 'invalid_input')

    def test_raw_ecdsa_encoding_and_scalar_bounds(self):
        signature = self.signatures[3]
        r, s = int.from_bytes(signature[:32], 'big'), int.from_bytes(signature[32:], 'big')
        self.assertFalse(self.verify(3, signature=utils.encode_dss_signature(r, s)).signature_valid)
        self.assertFalse(self.verify(3, signature=signature[:32][::-1]+signature[32:][::-1]).signature_valid)
        for bad in (0, helper.P256_ORDER, (1 << 256)-1):
            for replace_r in (True, False):
                raw = (bad.to_bytes(32, 'big') + signature[32:] if replace_r else signature[:32] + bad.to_bytes(32, 'big'))
                self.assertEqual(self.verify(3, signature=raw).reason, 'invalid_signature_encoding')
        # Native raw verifier supplies no low-S policy: both equivalents are valid.
        alternate = signature[:32] + (helper.P256_ORDER-s).to_bytes(32, 'big')
        self.assertTrue(self.verify(3, signature=alternate).signature_valid)

    def test_untrusted_expired_self_signed_certificate_never_grants(self):
        for version in (2, 3):
            cert = synthetic_certificate(self.keys[version])
            result = self.verify(version, key=cert, key_format='certificate')
            self.assertTrue(result.signature_valid)
            self.assertEqual(result.certificate_sha256, hashlib.sha256(cert).hexdigest())
            self.untrusted(result)
            self.assertFalse(self.verify(version, key=cert+b'\0', key_format='certificate').signature_valid)

    def test_cli_bounded_regular_files_and_no_authority(self):
        with tempfile.TemporaryDirectory(prefix='authcp-cli-') as directory:
            root = Path(directory)
            nonce, signature, key = root/'nonce', root/'signature', root/'public-key'
            nonce.write_bytes(self.nonces[3]); signature.write_bytes(self.signatures[3]); key.write_bytes(spki(self.keys[3]))
            command = [sys.executable, str(Path(helper.__file__).resolve()), '--version', '3', '--nonce', str(nonce), '--signature', str(signature), '--public-key', str(key)]
            result = subprocess.run(command, capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 0)
            record = json.loads(result.stdout)
            self.assertTrue(record['signature_valid']); self.assertFalse(record['authorization_granted'])
            self.assertIsNone(record['certificate_chain_trusted'])
            for bad in ('oversize', 'symlink', 'fifo'):
                key.unlink()
                if bad == 'oversize': key.write_bytes(b'x'*(helper.MAX_KEY_BYTES+1))
                elif bad == 'symlink': key.symlink_to(nonce)
                else: os.mkfifo(key)
                result = subprocess.run(command, capture_output=True, timeout=5)
                self.assertEqual(result.returncode, 1)
                record = json.loads(result.stdout)
                self.assertFalse(record['signature_valid']); self.assertFalse(record['authorization_granted'])
                self.assertEqual(record['reason'], 'input_read_error')

    def test_independent_rfc4754_public_known_answer(self):
        # RFC4754 section8.1 public coordinates/digest/signature only. No RFC private key.
        x = int('2442A5CC0ECD015FA3CA31DC8E2BBC70BF42D60CBCA20085E0822CB04235E970', 16)
        y = int('6FC98BD7E50211A4A27102FA3549DF79EBCB4BF246B80945CDDFE7D509BBFD7D', 16)
        key = ec.EllipticCurvePublicNumbers(x, y, ec.SECP256R1()).public_key()
        data = key.public_bytes(serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo)
        nonce = bytes.fromhex('BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD')
        signature = bytes.fromhex('CB28E0999B9C7715FD0A80D8E47A77079716CBBF917DD72E97566EA1C066957C86FA3BB4E26CAD5BF90B7F81899256CE7594BB1EA0C89212748BFF3B3D5B0315')
        self.assertTrue(helper.verify(3, nonce, signature, data).signature_valid)
        self.assertFalse(helper.verify(3, hashlib.sha256(nonce).digest(), signature, data).signature_valid)


if __name__ == '__main__':
    unittest.main()
