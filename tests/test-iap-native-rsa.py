#!/usr/bin/env python3
"""Pinned public RSA evidence and real crypto; no original-input/device replay."""
from pathlib import Path
import hashlib,json,subprocess,sys
root=Path(__file__).resolve().parents[1];group=root/'experiments/p105-iap-native-policy/rsa-closure'
for name in ['authcp-savedchain13-next','authcp-native-link13-next']:
 v=json.loads((group/(name+'-provenance.json')).read_text())
 assert v['normal_runtime_activation']is False and v['authorization_granted']is False and v['full_chain_trust']is None
 for n,expected in v['files'].items():
  b=(group/n).read_bytes();assert len(b)<100000 and hashlib.sha256(b).hexdigest()==expected,n
 assert v['independent_root_review']['status']=='PASS'
v=json.loads((group/'real-rsa-oracle.json').read_text());rows=v['cases'];assert len(rows)==7
assert [(r['public_test_modulus_bits'],r['native_weak'])for r in rows]==[(512,True),(1023,False),(1024,False),(2048,False),(1024,True),(512,True),(1024,False)]
for r in rows:
 assert r['instruction_count']<1000000
 if not r['synthetic_truncated_der']:
  assert any(c.get('actual_entry')=='0x2044db28'for c in r['calls'])
  assert any(c.get('actual_entry')=='0x20cb1e38'for c in r['calls'])
  assert any(c.get('native_blocksize')==(r['public_test_modulus_bits']+7)//8 for c in r['calls'])
subprocess.run([sys.executable,str(group/'test_public_link.py')],check=True,timeout=30)
print('PASS public crypto/pinned records; native source replays separately root-qualified; physical permission absent')
