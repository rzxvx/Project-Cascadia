# SPDX-License-Identifier: GPL-2.0
"""Bounded original certificate-signature routing; fake certificate/key/digest.

No native cryptography, certificate parser, process, IPC, hardware or Apple bytes
are executed/exported as fixtures. The digest and key verify are synthetic.
"""
import argparse,json,struct
from pathlib import Path
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import *
from native13 import Sources,OBJ,STACK,END,CACHE_SHA,DAEMON_SHA
p=argparse.ArgumentParser();p.add_argument('cache',type=Path);p.add_argument('daemon',type=Path);args=p.parse_args()
src=Sources(args.cache,args.daemon)
CERT,KEY,DESC,OID,PARAM,TBS,SIG,VERIFY=[OBJ+0x1000*i for i in range(1,9)]
SIGN=0x20c9a9d0; DIGEST_VERIFY=0x20ca8bcc; ROUTE=0x20ca8c48; PREFIX=0x20ca93cc
# PKCS1 signature OIDs / ECDSA-with-SHA256, public standard encodings only.
ALGS={'rsa-sha1':'2a864886f70d010105','rsa-sha256':'2a864886f70d01010b','ecdsa-sha256':'2a8648ce3d040302','unsupported':'2a030405'}
EXPECTED={'rsa-sha1':('3021300906052b0e03021a05000414',20,1),'rsa-sha256':('3031300d060960864801650304020105000420',32,1),'ecdsa-sha256':('',32,3)}

def run(alg,provider_result=0,whole_path=False):
    u=src.context();calls=[];digest_pc=None;digest_size=None;steps=0
    def read(a,n):
        return bytes(u.mem_read(a,n)) if OBJ<=a<OBJ+0x10000 or STACK<=a<STACK+0x10000 else src.cache_read(a,n)
    def put(a,v):u.mem_write(a,struct.pack('<I',v&0xffffffff))
    raw=bytes.fromhex(ALGS[alg]);u.mem_write(OID,raw);u.mem_write(PARAM,b'\x05\x00')
    u.mem_write(TBS,b'synthetic-tbs-only');u.mem_write(SIG,b'not-a-real-signature')
    put(CERT+0x3c,OID);put(CERT+0x40,len(raw));put(CERT+0x44,PARAM);put(CERT+0x48,2)
    put(CERT+0x10,TBS);put(CERT+0x14,18);put(CERT+0x28,SIG);put(CERT+0x2c,20)
    path=OBJ+0x9000
    put(path+8,2);put(path+0x10,0);put(path+0x14,0xffffffff);put(path+0x1c,CERT);put(path+0x20,CERT)
    put(KEY+8,DESC);put(DESC,1);put(DESC+0x18,VERIFY|1);put(DESC+0x34,(VERIFY+4)|1)
    def hook(mc,pc,size,cookie):
        nonlocal digest_pc,digest_size,steps
        steps+=1
        r0,r1,r2,r3=[mc.reg_read(r)for r in(UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3)]
        def ret(v):mc.reg_write(UC_ARM_REG_R0,v&0xffffffff);mc.reg_write(UC_ARM_REG_PC,mc.reg_read(UC_ARM_REG_LR))
        if pc==0x20c9e488:
            assert whole_path and r0==path and r1==1;calls.append({'synthetic_path_issuer_public_key':True});ret(KEY);return
        if pc==src.exports['_CFRelease']:
            assert r0==KEY;ret(0);return
        if pc==VERIFY+4:
            assert r0==KEY;ret(3 if alg.startswith('ecdsa')else 1);return
        if pc==0x20ca8e8a:
            digest_pc=r3&~1;digest_size=mc.reg_read(UC_ARM_REG_R8)
            assert r0==TBS and r1==18 and digest_size in(20,32)
            calls.append({'synthetic_digest_backend':hex(digest_pc),'requested_digest_length':digest_size,'input':'synthetic-tbs'})
        if digest_pc is not None and pc==digest_pc:
            assert r0==TBS and r1==18;mc.mem_write(r2,b'\xa5'*digest_size);ret(0);return
        if pc==VERIFY:
            sp=mc.reg_read(UC_ARM_REG_SP);sg,n=struct.unpack('<2I',mc.mem_read(sp,8))
            assert r0==KEY and r1==1 and sg==SIG and n==20
            payload=bytes(mc.mem_read(r2,r3));prefix,length,_=EXPECTED[alg]
            assert payload==bytes.fromhex(prefix)+b'\xa5'*length,(alg,payload.hex())
            calls.append({'synthetic_key_verify':True,'padding_argument':r1,'digest_info_prefix':prefix,'digest_length':length,'total_length':r3,'signature_length':n,'provider_result':provider_result})
            ret(provider_result);return
        if pc==0x20cc8f38:
            assert r2<=32;left=read(r0,r2);right=read(r1,r2);ret(0 if left==right else 1);return
        if pc==0x20cc8f48:
            assert r2<=64;mc.mem_write(r0,read(r1,r2));ret(r0);return
        if pc in(SIGN,DIGEST_VERIFY,ROUTE,PREFIX):calls.append({'original_entry':hex(pc)})
        if pc==0x2051f69c:raise AssertionError('stack canary failure')
    u.hook_add(UC_HOOK_CODE,hook);u.reg_write(UC_ARM_REG_R0,path if whole_path else CERT);u.reg_write(UC_ARM_REG_R1,KEY)
    try:u.emu_start((0x20c9e638 if whole_path else SIGN)|1,END,count=1800)
    except Exception as e:raise RuntimeError(f'{alg}:pc={u.reg_read(UC_ARM_REG_PC):x},calls={calls}')from e
    assert u.reg_read(UC_ARM_REG_PC)==END
    result=u.reg_read(UC_ARM_REG_R0);result=result if result<2**31 else result-2**32
    expected=0 if alg!='unsupported'and provider_result==0 else (1 if whole_path else -26267)
    assert result==expected,result
    if alg=='unsupported':assert not any('synthetic_key_verify'in c for c in calls)
    return {'whole_native_path':whole_path,'algorithm':alg,'synthetic_key_verify_result':provider_result,'native_result':result,'calls':calls,'instruction_count':steps}
rows=[run(a,r)for a in('rsa-sha1','rsa-sha256','ecdsa-sha256')for r in(0,-1)]+[run('unsupported')]+[run('rsa-sha256',r,True)for r in(0,-1)]
print(json.dumps({'cache_sha256':CACHE_SHA,'securityd_sha256':DAEMON_SHA,'scope':'actual routing and error normalization, synthetic certificate fields/digest/key verification','cases':rows},indent=2,sort_keys=True))
