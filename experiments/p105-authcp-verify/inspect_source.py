#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Hash-pinned 13G36 IAPAuthentication CA metadata, never certificate/key bytes.

An embedded CA's curve is not a proof of accessory leaf identity or policy.
No native Security/trust code, bus transport or device registers are executed.
"""
import argparse
import hashlib
import json
import mmap
from pathlib import Path
import struct
from cryptography import x509
from cryptography.hazmat.primitives.asymmetric import ec, rsa

CACHE_SHA = '4f5b5d73244c53fd0bdc5b8012106b10d0dd7607cc850bf7c8f9e9ef3a3f733b'
TEXT_SHA = '03164548d75eb991bedb9bd523d4e684eeba43c5c43cd2cfc36530962f091fea'
IMAGE = '/System/Library/PrivateFrameworks/IAPAuthentication.framework/IAPAuthentication'
CERTS = [
    (0x31a111c0, 1091, '884bf87d549f2de7c4fceb4284a8cff9f8b817f36dcf77a40660b33b8afefd0d'),
    (0x31a11608, 1026, '2eb53dbefdc6f5e7e13d6690fc8cbf9cf4c3f7e501feafb3bc052431dbc83180'),
    (0x31a11a3d, 588, '81da17dabaf55012d1ea3946c02c3a423405bc290403cc54cdb88bfe95677472'),
]

def inspect(path):
    # Keep the same opened file for hashing/parsing; caller owns immutable source.
    with path.open('rb') as stream:
        digest = hashlib.sha256()
        while block := stream.read(1024 * 1024):
            digest.update(block)
        if digest.hexdigest() != CACHE_SHA:
            raise ValueError('wrong official cache SHA256')
        with mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
            mo, mc, io, ic = struct.unpack_from('<4I', data, 16)
            if not 1 <= mc <= 16 or not 1 <= ic <= 4096:
                raise ValueError('unexpected pinned table count')
            mappings = [struct.unpack_from('<3Q2I', data, mo + 32*i) for i in range(mc)]
            def offset(address, size):
                row = next((r for r in mappings if r[0] <= address and address + size <= r[0] + r[1]), None)
                if row is None or row[2] + address - row[0] + size > len(data):
                    raise ValueError('source address outside cache mapping')
                return row[2] + address - row[0]
            images = {}
            for i in range(ic):
                va, _, _, po, _ = struct.unpack_from('<3Q2I', data, io + 32*i)
                end = data.find(b'\0', po, min(len(data), po + 4096))
                if end < 0:
                    raise ValueError('unterminated image path')
                images[data[po:end].decode('utf-8')] = va
            if images.get(IMAGE) != 0x31a0d000:
                raise ValueError('unexpected image address')
            start = offset(0x31a0d000, 356352)
            text = data[start:start+356352]
            if hashlib.sha256(text).hexdigest() != TEXT_SHA:
                raise ValueError('unexpected IAP TEXT hash')
            rows = []
            for address, size, expected in CERTS:
                raw = text[address - 0x31a0d000:address - 0x31a0d000 + size]
                if hashlib.sha256(raw).hexdigest() != expected:
                    raise ValueError('embedded certificate hash mismatch')
                certificate = x509.load_der_x509_certificate(raw)
                key = certificate.public_key()
                algorithm = 'RSA' if isinstance(key, rsa.RSAPublicKey) else 'EC' if isinstance(key, ec.EllipticCurvePublicKey) else 'unsupported'
                try:
                    ca = certificate.extensions.get_extension_for_class(x509.BasicConstraints).value.ca
                except x509.ExtensionNotFound:
                    ca = None
                row = {'address': hex(address), 'length': size, 'certificate_sha256': expected,
                       'key_algorithm': algorithm, 'key_bits': key.key_size,
                       'basic_constraints_ca': ca}
                if isinstance(key, ec.EllipticCurvePublicKey):
                    row['curve'] = key.curve.name
                rows.append(row)
    return {'cache_sha256': CACHE_SHA, 'iap_text_sha256': TEXT_SHA,
            'scope': 'embedded public CA metadata only; no leaf/policy/trust proof',
            'certificates': rows}

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('cache', type=Path)
    args = parser.parse_args()
    print(json.dumps(inspect(args.cache), indent=2))
