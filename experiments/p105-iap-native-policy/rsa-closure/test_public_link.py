# SPDX-License-Identifier: GPL-2.0
"""Independent real public-key check of the synthetic native-link fixture."""
import json,unittest
from pathlib import Path
from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes,serialization
from cryptography.hazmat.primitives.asymmetric import rsa,padding

ROOT=Path(__file__).parent
DATA=json.loads((ROOT/'synthetic-public-link.json').read_text())
DER=bytes.fromhex(DATA['pkcs1_public_der_hex'])
TBS=bytes.fromhex(DATA['tbs_hex'])
SIGNATURE=bytes.fromhex(DATA['rsa_pkcs1v15_sha256_signature_hex'])

class Tests(unittest.TestCase):
    def test_real_signature_and_public_identity(self):
        key=serialization.load_der_public_key(DER)
        self.assertIsInstance(key,rsa.RSAPublicKey)
        self.assertEqual(key.key_size,1024)
        self.assertEqual(key.public_numbers().n,int(DATA['public_modulus_hex'],16))
        self.assertEqual(key.public_numbers().e,DATA['public_exponent'])
        key.verify(SIGNATURE,TBS,padding.PKCS1v15(),hashes.SHA256())

    def test_corruption_and_wrong_tbs(self):
        key=serialization.load_der_public_key(DER)
        with self.assertRaises(InvalidSignature):
            key.verify(SIGNATURE[:-1]+bytes([SIGNATURE[-1]^1]),TBS,padding.PKCS1v15(),hashes.SHA256())
        with self.assertRaises(InvalidSignature):
            key.verify(SIGNATURE,TBS[:-1]+bytes([TBS[-1]^1]),padding.PKCS1v15(),hashes.SHA256())

    def test_original_oracle_scope_and_links(self):
        data=json.loads((ROOT/'native-link-oracle.json').read_text())
        self.assertIsNone(data['certificate_chain_trusted'])
        self.assertFalse(data['authorization_granted'])
        rows=data['cases']
        self.assertEqual(len(rows),3)
        self.assertEqual([r['native_path_verify_result']for r in rows],[0,1,0])
        self.assertEqual([r['native_completed_links']for r in rows],[1,0,1])
        for r in rows:
            self.assertLess(r['instruction_count'],2000000)
            self.assertTrue(any(c.get('original_RSA_public_verify')for c in r['calls']))
            self.assertTrue(any(c.get('original_corecrypto_RSA_public_modexp')for c in r['calls']))

if __name__=='__main__':unittest.main(verbosity=2)
