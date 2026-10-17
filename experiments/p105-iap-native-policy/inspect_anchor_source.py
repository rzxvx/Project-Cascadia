#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Official13G anchor-source metadata only; no Apple cert/key bytes are output."""
import argparse,hashlib,json,mmap,struct
from pathlib import Path
from cryptography import x509
from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes,serialization
from cryptography.hazmat.primitives.asymmetric import ec,padding,rsa
from cryptography.hazmat.primitives.serialization import pkcs7
SHA='4f5b5d73244c53fd0bdc5b8012106b10d0dd7607cc850bf7c8f9e9ef3a3f733b'


def inspect(path):
    with path.open('rb')as stream:
        digest=hashlib.sha256()
        while block:=stream.read(2**20):digest.update(block)
        if digest.hexdigest()!=SHA:raise ValueError('wrong pinned official cache')
        with mmap.mmap(stream.fileno(),0,access=mmap.ACCESS_READ)as data:
            mo,mc=struct.unpack_from('<2I',data,16)
            maps=[struct.unpack_from('<3Q2I',data,mo+32*i)for i in range(mc)]
            def get(va,n):
                row=next((r for r in maps if r[0]<=va and va+n<=r[0]+r[1]),None)
                if row is None:raise ValueError('unmapped source range')
                off=row[2]+va-row[0];return data[off:off+n]
            def word(va):return struct.unpack('<I',get(va,4))[0]
            rows=[]
            for version,ordinal,byte_ptr,len_ptr in[(2,1,0x3674c068,0x3674c06c),(2,2,0x3674c070,0x3674c074),(3,1,0x3674c078,0x3674c07c)]:
                address=word(byte_ptr);length=word(word(len_ptr));blob=get(address,length)
                if version==2:certificates=[x509.load_der_x509_certificate(blob)];container='DER'
                else:certificates=pkcs7.load_der_pkcs7_certificates(blob);container='PKCS7-certificates-only'
                if len(certificates)!=1:raise ValueError('unexpected source anchor cardinality')
                cert=certificates[0];raw=cert.public_bytes(serialization.Encoding.DER);key=cert.public_key()
                ca=None
                try:ca=cert.extensions.get_extension_for_class(x509.BasicConstraints).value.ca
                except x509.ExtensionNotFound:pass
                extensions=[]
                for extension in cert.extensions:
                    row={'oid':extension.oid.dotted_string,'critical':extension.critical}
                    if isinstance(extension.value,x509.SubjectKeyIdentifier):row['key_id_sha256']=hashlib.sha256(extension.value.digest).hexdigest()
                    if isinstance(extension.value,x509.AuthorityKeyIdentifier):row['key_id_sha256']=hashlib.sha256(extension.value.key_identifier).hexdigest()if extension.value.key_identifier is not None else None
                    extensions.append(row)
                self_signature=None
                if cert.issuer==cert.subject:
                    try:
                        if isinstance(key,rsa.RSAPublicKey):key.verify(cert.signature,cert.tbs_certificate_bytes,padding.PKCS1v15(),cert.signature_hash_algorithm)
                        elif isinstance(key,ec.EllipticCurvePublicKey):key.verify(cert.signature,cert.tbs_certificate_bytes,ec.ECDSA(cert.signature_hash_algorithm))
                        else:raise ValueError('unsupported source key')
                        self_signature=True
                    except InvalidSignature:self_signature=False
                rows.append({'version':version,'ordinal':ordinal,'byte_pointer_address':hex(byte_ptr),'length_pointer_address':hex(len_ptr),'source_blob_address':hex(address),'source_blob_length':length,'source_blob_sha256':hashlib.sha256(blob).hexdigest(),'container':container,'parsed_certificate_count':len(certificates),'certificate_sha256':hashlib.sha256(raw).hexdigest(),'certificate_length':len(raw),'subject':cert.subject.rfc4514_string(),'issuer':cert.issuer.rfc4514_string(),'basic_constraints_ca':ca,'key_algorithm':'RSA'if isinstance(key,rsa.RSAPublicKey)else'EC','key_bits':key.key_size,'curve':key.curve.name if isinstance(key,ec.EllipticCurvePublicKey)else None,'public_key_spki_sha256':hashlib.sha256(key.public_bytes(serialization.Encoding.DER,serialization.PublicFormat.SubjectPublicKeyInfo)).hexdigest(),'signature_algorithm_oid':cert.signature_algorithm_oid.dotted_string,'signature_hash':cert.signature_hash_algorithm.name,'self_signature_mathematically_valid':self_signature,'extensions':extensions})
    return {'cache_sha256':SHA,'scope':'actual embedded anchor provenance/metadata; source root provenance is not a validated peer chain','certificate_chain_trusted':None,'authorization_granted':False,'anchors':rows}

if __name__=='__main__':
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('cache',type=Path);args=ap.parse_args();print(json.dumps(inspect(args.cache),indent=2))
