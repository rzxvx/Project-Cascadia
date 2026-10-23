# SPDX-License-Identifier: GPL-2.0
"""Original unknown-extension branch only, synthetic post-parser metadata.

The extension dictionary has already reported no handler at this entry. This is
not a DER decoder or registered-extension parser and makes no certificate trust.
"""
import argparse,json,struct
from pathlib import Path
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import *
from native13 import Sources,OBJ,STACK,END,CACHE_SHA,DAEMON_SHA
ap=argparse.ArgumentParser();ap.add_argument('cache',type=Path);ap.add_argument('daemon',type=Path);args=ap.parse_args();source=Sources(args.cache,args.daemon)
ENTRY,STOP,LITERAL=0x20c98a86,0x20c98ab2,0x20cd69a8
assert source.cache_read(LITERAL,8)==bytes.fromhex('2a864886f7636406')
assert source.exports['_SecCertificateHasUnknownCriticalExtension']==0x20c9b6dc
CERT,EXT,OID=OBJ+0x1000,OBJ+0x2000,OBJ+0x3000

def run(label,oid,critical=True,prior=False):
    u=source.context();calls=[]
    def put(a,v):u.mem_write(a,struct.pack('<I',v))
    put(CERT+0x110,EXT);u.mem_write(CERT+0x9c,bytes([prior]));u.mem_write(EXT+8,bytes([critical]))
    if oid is not None:u.mem_write(OID,oid)
    put(STACK+0xff00+0x18,OID if oid is not None else 0)
    put(STACK+0xff00+0x1c,len(oid)if oid is not None else 0)
    u.reg_write(UC_ARM_REG_R4,CERT);u.reg_write(UC_ARM_REG_R5,0)
    def hook(mc,pc,size,cookie):
        if pc==0x20cc8f38:
            r0,r1,r2=[mc.reg_read(r)for r in(UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2)]
            assert r0==OID and r1==LITERAL and r2==8
            calls.append({'synthetic_memcmp_length':8,'matches_public_namespace_prefix':oid[:8]==source.cache_read(LITERAL,8)})
            mc.reg_write(UC_ARM_REG_R0,0 if oid[:8]==source.cache_read(LITERAL,8)else 1)
            mc.reg_write(UC_ARM_REG_PC,mc.reg_read(UC_ARM_REG_LR))
    u.hook_add(UC_HOOK_CODE,hook);u.emu_start(ENTRY|1,STOP,count=100)
    assert u.reg_read(UC_ARM_REG_PC)==STOP
    u.reg_write(UC_ARM_REG_R0,CERT);u.reg_write(UC_ARM_REG_LR,END|1)
    u.emu_start(0x20c9b6dc|1,END,count=20)
    assert u.reg_read(UC_ARM_REG_PC)==END
    value=bool(u.reg_read(UC_ARM_REG_R0))
    assert value==bool(u.mem_read(CERT+0x9c,1)[0])
    expected=bool(prior or(critical and(oid is None or len(oid)<9 or oid[:8]!=source.cache_read(LITERAL,8))))
    assert value==expected
    return {'label':label,'synthetic_unregistered_oid_hex':oid.hex()if oid is not None else None,'synthetic_critical':critical,'synthetic_prior_unknown_flag':prior,'native_unknown_flag':value,'calls':calls}
rows=[run('apple-private-36',bytes.fromhex('2a864886f763640624')),run('apple-private-other',bytes.fromhex('2a864886f76364067f')),run('apple-long',bytes.fromhex('2a864886f76364068100')),run('exact-eight-prefix',bytes.fromhex('2a864886f7636406')),run('different-namespace',bytes.fromhex('2a864886f763640724')),run('unrelated',bytes.fromhex('2a030405')),run('noncritical-unrelated',bytes.fromhex('2a030405'),False),run('prior-latched-noncritical',bytes.fromhex('2a030405'),False,True),run('internal-null-oid',None)]
print(json.dumps({'cache_sha256':CACHE_SHA,'securityd_sha256':DAEMON_SHA,'scope':'original unregistered-extension fallback only; no original DER parser/dictionary handler/name/policy','entry':hex(ENTRY),'stop':hex(STOP),'cases':rows},indent=2,sort_keys=True))
