# SPDX-License-Identifier: GPL-2.0
"""Real ephemeral synthetic keys/certificates only; no private material saved."""
import unittest
import hashlib,json
from pathlib import Path
import authcp_verify
from dataclasses import asdict
from datetime import datetime

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, padding, rsa, utils
from cryptography.x509.oid import NameOID, ObjectIdentifier, ExtensionOID
from direct_evidence import examine, SOURCE_ANCHORS

DER = serialization.Encoding.DER


def name(cn):
    return x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, cn)])


def certificate(public, signer, subject, issuer, serial=1, before=datetime(2007,1,1),
                extensions=(), after=datetime(2030,1,1)):
    builder=(x509.CertificateBuilder().subject_name(subject).issuer_name(issuer)
             .public_key(public).serial_number(serial).not_valid_before(before)
             .not_valid_after(after))
    for extension,critical in extensions:
        builder=builder.add_extension(extension,critical)
    return builder.sign(signer, hashes.SHA256()).public_bytes(DER)


def sha1_certificate(template, signer):
    """Test-only standard DER re-signing; Builder forbids generating SHA1 certs.

    SHA1 and SHA256 RSA OIDs differ in their last octet and have equal length.
    The original ephemeral signature has the same modulus-dependent length.
    The generated object contains no preexisting/private/source certificate.
    """
    sha256=bytes.fromhex('300d06092a864886f70d01010b0500')
    sha1=bytes.fromhex('300d06092a864886f70d0101050500')
    assert template.count(sha256)==2
    original=x509.load_der_x509_certificate(template)
    changed=template.replace(sha256,sha1)
    changed_cert=x509.load_der_x509_certificate(changed)
    signature=signer.sign(changed_cert.tbs_certificate_bytes,padding.PKCS1v15(),hashes.SHA1())
    assert len(signature)==len(original.signature)
    assert changed.endswith(original.signature)
    result=changed[:-len(signature)]+signature
    signer.public_key().verify(signature,x509.load_der_x509_certificate(result).tbs_certificate_bytes,
                               padding.PKCS1v15(),hashes.SHA1())
    return result


class Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.rsa_issuer=rsa.generate_private_key(public_exponent=65537,key_size=2048)
        cls.rsa_leaf=rsa.generate_private_key(public_exponent=65537,key_size=1024)
        cls.ec_issuer=ec.generate_private_key(ec.SECP256R1())
        cls.ec_leaf=ec.generate_private_key(ec.SECP256R1())
        cls.issuer_name=name('Synthetic explicitly untrusted issuer')

    def fixture(self, version=2, cn='IPA_synthetic', before=datetime(2007,1,1),
                leaf_extensions=(), anchor_extensions=(), after=datetime(2030,1,1)):
        issuer,leaf=(self.rsa_issuer,self.rsa_leaf)if version==2 else(self.ec_issuer,self.ec_leaf)
        anchor=certificate(issuer.public_key(),issuer,self.issuer_name,self.issuer_name,
                           extensions=anchor_extensions)
        cert=certificate(leaf.public_key(),issuer,name(cn),self.issuer_name,serial=2,
                         before=before,extensions=leaf_extensions,after=after)
        nonce=bytes(range(20 if version==2 else 32))
        if version==2:
            signature=leaf.sign(nonce,padding.PKCS1v15(),utils.Prehashed(hashes.SHA1()))
        else:
            encoded=leaf.sign(nonce,ec.ECDSA(utils.Prehashed(hashes.SHA256())))
            r,s=utils.decode_dss_signature(encoded)
            signature=r.to_bytes(32,'big')+s.to_bytes(32,'big')
        return version,cert,anchor,nonce,signature

    def assert_no_grant(self,result):
        self.assertIsNone(result.certificate_chain_trusted)
        self.assertIsNone(result.accessory_identity_verified)
        self.assertFalse(result.authorization_granted)

    def test_real_rsa_sha256(self):
        out=examine(*self.fixture())
        self.assertTrue(out.certificate_signature_valid)
        self.assertTrue(out.nonce_signature_valid)
        self.assertTrue(out.supported_facets_pass)
        self.assertFalse(out.source_anchor_matches_version)
        self.assertEqual(out.certificate_signature_scheme,'rsa-pkcs1v15-sha256')
        self.assert_no_grant(out)

    def test_real_rsa_sha1(self):
        v,c,a,n,s=self.fixture()
        out=examine(v,sha1_certificate(c,self.rsa_issuer),a,n,s)
        self.assertTrue(out.supported_facets_pass)
        self.assertEqual(out.certificate_signature_scheme,'rsa-pkcs1v15-sha1')
        self.assert_no_grant(out)

    def test_real_ecdsa_sha256(self):
        out=examine(*self.fixture(3))
        self.assertTrue(out.supported_facets_pass)
        self.assertEqual(out.certificate_signature_scheme,'ecdsa-sha256')
        self.assert_no_grant(out)

    def test_wrong_issuer_key(self):
        v,c,a,n,s=self.fixture()
        wrong=rsa.generate_private_key(public_exponent=65537,key_size=2048)
        a=certificate(wrong.public_key(),wrong,self.issuer_name,self.issuer_name)
        out=examine(v,c,a,n,s)
        self.assertEqual(out.reason,'invalid_certificate_signature')
        self.assert_no_grant(out)

    def test_tbs_corruption(self):
        v,c,a,n,s=self.fixture()
        self.assertEqual(c.count(b'IPA_synthetic'),1)
        out=examine(v,c.replace(b'IPA_synthetic',b'IPA_synthetix'),a,n,s)
        self.assertEqual(out.reason,'invalid_certificate_signature')
        self.assert_no_grant(out)

    def test_signature_corruption(self):
        for version in(2,3):
            v,c,a,n,s=self.fixture(version)
            out=examine(v,c[:-1]+bytes([c[-1]^1]),a,n,s)
            self.assertEqual(out.reason,'invalid_certificate_signature')
            self.assert_no_grant(out)

    def test_nonce_signature_corruption(self):
        for version in(2,3):
            v,c,a,n,s=self.fixture(version)
            out=examine(v,c,a,n,s[:-1]+bytes([s[-1]^1]))
            self.assertTrue(out.certificate_signature_valid)
            self.assertFalse(out.nonce_signature_valid)
            self.assertFalse(out.supported_facets_pass)
            self.assert_no_grant(out)

    def test_native_name_facet(self):
        for cn in('ipa_synthetic','Other','IPA_é'):
            out=examine(*self.fixture(cn=cn))
            self.assertEqual(out.reason,'native_common_name_facet_failed')
            self.assertTrue(out.certificate_signature_valid)
            self.assert_no_grant(out)

    def test_native_cutoff_strict_boundary(self):
        for before in(datetime(2006,5,30,23,59,59),datetime(2006,5,31)):
            out=examine(*self.fixture(before=before))
            self.assertEqual(out.reason,'native_not_before_facet_failed')
        out=examine(*self.fixture(before=datetime(2006,5,31,0,0,1)))
        self.assertTrue(out.supported_facets_pass)
        self.assert_no_grant(out)

    def test_expired_math_is_not_trust(self):
        out=examine(*self.fixture(after=datetime(2008,1,1)))
        self.assertTrue(out.supported_facets_pass)
        self.assert_no_grant(out)

    def test_identifier_compare(self):
        ski=x509.SubjectKeyIdentifier(b'abc')
        matching=x509.AuthorityKeyIdentifier(b'abc',None,None)
        mismatch=x509.AuthorityKeyIdentifier(b'abd',None,None)
        out=examine(*self.fixture(leaf_extensions=((matching,False),),anchor_extensions=((ski,False),)))
        self.assertTrue(out.supported_facets_pass)
        self.assertTrue(out.identifier_comparison_performed)
        out=examine(*self.fixture(leaf_extensions=((mismatch,False),),anchor_extensions=((ski,False),)))
        self.assertEqual(out.reason,'native_identifier_link_facet_failed')
        self.assertTrue(out.identifier_comparison_performed)
        out=examine(*self.fixture(leaf_extensions=((matching,False),)))
        self.assertTrue(out.supported_facets_pass)
        self.assertFalse(out.identifier_comparison_performed)

    def test_unsupported_authority_serial(self):
        aki=x509.AuthorityKeyIdentifier(b'abc',[x509.DirectoryName(self.issuer_name)],123)
        out=examine(*self.fixture(leaf_extensions=((aki,False),)))
        self.assertEqual(out.reason,'unsupported_authority_identifier_profile')
        self.assert_no_grant(out)

    def test_real_unsupported_general_name_type(self):
        # Cryptography accepts the Certificate object and decodes this extension
        # lazily. x400Address [3] is unsupported and must become denied evidence.
        ext=x509.UnrecognizedExtension(ExtensionOID.SUBJECT_ALTERNATIVE_NAME,
                                     bytes.fromhex('3002a300'))
        args=self.fixture(leaf_extensions=((ext,False),))
        cert=x509.load_der_x509_certificate(args[1])
        with self.assertRaises(x509.UnsupportedGeneralNameType):list(cert.extensions)
        out=examine(*args)
        self.assertEqual(out.reason,'unsupported_or_invalid_certificate')
        self.assert_no_grant(out)
        # The same actual lazy decoder failure in the supplied issuer is denied.
        out=examine(*self.fixture(anchor_extensions=((ext,False),)))
        self.assertEqual(out.reason,'unsupported_or_invalid_certificate')
        self.assert_no_grant(out)

    def test_real_malformed_general_name_der(self):
        ext=x509.UnrecognizedExtension(ExtensionOID.SUBJECT_ALTERNATIVE_NAME,
                                     bytes.fromhex('300281ff'))
        args=self.fixture(leaf_extensions=((ext,False),))
        cert=x509.load_der_x509_certificate(args[1])
        with self.assertRaises(ValueError):list(cert.extensions)
        out=examine(*args)
        self.assertEqual(out.reason,'unsupported_or_invalid_certificate')
        self.assert_no_grant(out)

    def test_leaf_critical_deny(self):
        out=examine(*self.fixture(leaf_extensions=((x509.BasicConstraints(False,None),True),)))
        self.assertEqual(out.reason,'unsupported_leaf_critical_profile')
        self.assert_no_grant(out)

    def test_unknown_leaf_extension_deny(self):
        ext=x509.UnrecognizedExtension(ObjectIdentifier('1.2.3.4.5'),b'synthetic')
        for critical in(False,True):
            out=examine(*self.fixture(leaf_extensions=((ext,critical),)))
            self.assertEqual(out.reason,'unsupported_leaf_extension')
            self.assert_no_grant(out)

    def test_anchor_critical_not_promoted_to_trust(self):
        ext=x509.UnrecognizedExtension(ObjectIdentifier('1.2.3.4.5'),b'synthetic')
        out=examine(*self.fixture(anchor_extensions=((ext,True),)))
        self.assertTrue(out.certificate_signature_valid)
        self.assertTrue(out.supported_facets_pass)
        self.assertFalse(out.source_anchor_matches_version)
        self.assert_no_grant(out)

    def test_unknown_paths_inputs_bounds(self):
        args=self.fixture()
        for extra in((args[1],),[],b''):
            out=examine(*args,intermediates=extra)
            self.assertEqual(out.reason,'unsupported_path_or_input')
            self.assert_no_grant(out)
        v,c,a,n,s=args
        for bad in(b'',c+b'\0',c[:-1],b'\x30\x80'+c,bytearray(c),b'\0'*16385):
            out=examine(v,bad,a,n,s)
            self.assertEqual(out.reason,'unsupported_or_invalid_certificate')
            self.assert_no_grant(out)
        out=examine(True,c,a,n,s)
        self.assertEqual(out.reason,'unsupported_path_or_input')

    def test_inner_outer_algorithm_mismatch(self):
        v,c,a,n,s=self.fixture()
        old=bytes.fromhex('300d06092a864886f70d01010b0500')
        self.assertEqual(c.count(old),2)
        at=c.rfind(old)
        changed=c[:at]+old.replace(b'\x0b\x05\x00',b'\x05\x05\x00')+c[at+len(old):]
        out=examine(v,changed,a,n,s)
        self.assertEqual(out.reason,'unsupported_or_invalid_certificate')
        self.assert_no_grant(out)

    def test_wrong_name_same_signing_key(self):
        v,c,a,n,s=self.fixture()
        alternate=name('Different explicitly untrusted issuer')
        a=certificate(self.rsa_issuer.public_key(),self.rsa_issuer,alternate,alternate)
        out=examine(v,c,a,n,s)
        self.assertTrue(out.certificate_signature_valid)
        self.assertEqual(out.reason,'unsupported_issuer_name_link')
        self.assert_no_grant(out)

    def test_pinned_existing_nonce_verifier(self):
        dependency=json.loads((Path(__file__).parent/"dependencies.json").read_text())["nonce_verifier"]
        actual=hashlib.sha256(Path(authcp_verify.__file__).read_bytes()).hexdigest()
        self.assertEqual(actual,dependency["sha256"])

    def test_readonly_result_and_anchor_pins(self):
        out=examine(*self.fixture())
        with self.assertRaises(Exception):out.authorization_granted=True
        with self.assertRaises(TypeError):SOURCE_ANCHORS[2]=frozenset()
        self.assertEqual(len(SOURCE_ANCHORS[2]),2)
        self.assertEqual(len(SOURCE_ANCHORS[3]),1)
        self.assertTrue(all(len(h)==64 for hs in SOURCE_ANCHORS.values()for h in hs))
        self.assertFalse(any(isinstance(v,bytes)for v in asdict(out).values()))

if __name__=='__main__':
    unittest.main(verbosity=2)
