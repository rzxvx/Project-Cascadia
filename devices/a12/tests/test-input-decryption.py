#!/usr/bin/env python3
"""Exercise boot-input decryption fallback without network or real firmware."""
import hashlib,importlib.util,sys,tempfile,unittest
from pathlib import Path
from unittest.mock import patch
A12=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(A12/'boot'))
spec=importlib.util.spec_from_file_location('fetch_inputs',A12/'boot/fetch-device-inputs.py')
f=importlib.util.module_from_spec(spec);spec.loader.exec_module(f)

class Decode(unittest.TestCase):
    def check(self,name,encrypted,bad_key=False):
        with tempfile.TemporaryDirectory() as d:
            out=Path(d)/'component.part';calls=[];plain=b'validated plaintext'
            def run(*a):
                calls.append(tuple(map(str,a)))
                if a[0]=='img4':out.write_bytes(b'encrypted' if encrypted else plain)
                else:out.write_bytes(b'wrong key output' if bad_key else plain)
            with patch.object(f,'run',side_effect=run):
                f.decode_component(Path('stock.im4p'),out,hashlib.sha256(plain).hexdigest(),{'product':'iPad11,6','build':'21B91'},name)
            return calls
    def test_plain_no_lookup(self):self.assertEqual(len(self.check('iBSS.raw',False)),1)
    def test_encrypted_product_and_build_lookup(self):
        calls=self.check('iBSS.raw',True)
        self.assertEqual(calls[1][:9],('ipsw','img4','im4p','extract','--lookup','--lookup-device','iPad11,6','--lookup-build','21B91'))
    def test_wrong_plaintext_still_rejected(self):
        with self.assertRaises(ValueError):self.check('iBSS.raw',True,True)
    def test_no_decryption_fallback_for_wrong_adt(self):
        with self.assertRaises(ValueError):self.check('DeviceTree.raw',True)
if __name__=='__main__':unittest.main()
