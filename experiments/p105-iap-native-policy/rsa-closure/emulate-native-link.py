# SPDX-License-Identifier: GPL-2.0
"""Original saved-chain key extraction and RSA certificate-link math.

Synthetic public TBS/signature/key fixture, source certificate field objects,
fake CF/heap/memory helpers and real Python SHA256 provider; no DER certificate
parser, complete trust evaluation or accessory/physical authorization.
"""
import argparse,json,struct
from pathlib import Path
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import *
from native13 import Sources,OBJ,STACK,END,CACHE_SHA,DAEMON_SHA
p=argparse.ArgumentParser();p.add_argument('cache',type=Path);p.add_argument('daemon',type=Path);a=p.parse_args();source=Sources(a.cache,a.daemon)
CERT,OID,PARAM,PUB=OBJ+0x1000,OBJ+0x2000,OBJ+0x2100,OBJ+0x3000
# Independently qualify actual named API/descriptor and imported libc veneers.
for name,address in {'_SecCertificatePathCopyPublicKeyAtIndex':0x20c9e488,'_SecCertificatePathVerify':0x20c9e638,'_SecKeyDigestAndVerify':0x20ca8bcc,'_SecKeyCreateRSAPublicKey':0x20cb1f9c}.items():
    assert source.exports[name]==address,(name,source.exports.get(name))
assert struct.unpack('<I',source.cache_read(0x36b033e8+0x18,4))[0]==0x20cb18e5
assert struct.unpack('<I',source.cache_read(0x36b033e8+0x34,4))[0]==0
image_offset,image_count=struct.unpack_from('<2I',source.cache,24)
for i in range(image_count):
    image,_,_,name_offset,_=struct.unpack_from('<3Q2I',source.cache,image_offset+32*i)
    name=source.cache[name_offset:source.cache.find(b'\0',name_offset)].decode()
    if name=='/usr/lib/system/libcorecrypto.dylib':
        crypto_symbols,sections,_,indirect=source.macho(source.cache,source.offset(image,28))
        stubs={address+j*width:indirect[index+j]for _,_,address,size,_,flags,index,width in sections if flags&255==8 for j in range(size//width)}
        break
else:raise AssertionError('missing original corecrypto image')
for address,name in {0x20451384:'_memcmp',0x20451394:'_memcpy',0x204513a4:'_memmove',0x204513b4:'_memset',0x204513c4:'_memset_s'}.items():
    assert stubs[address]==name
assert crypto_symbols['_ccder_decode_rsa_pub_n']['value']==0x2044d80c
assert crypto_symbols['_ccrsa_import_pub']['value']==0x2044db28
assert crypto_symbols['_ccrsa_pub_crypt']['value']==0x204407e4


import hashlib
FIXTURE=Path(__file__).with_name('synthetic-public-link.json')
assert FIXTURE.stat().st_size<=8192
assert hashlib.sha256(FIXTURE.read_bytes()).hexdigest()=='b62e73b2ffa0041bd1aa6bf0e3683d1d56d89023d0d18850cb01e2b5716dea35'
f=json.loads(FIXTURE.read_text())
PUB_DATA=bytes.fromhex(f['pkcs1_public_der_hex']);TBS_DATA=bytes.fromhex(f['tbs_hex']);SIGNATURE=bytes.fromhex(f['rsa_pkcs1v15_sha256_signature_hex'])
assert len(PUB_DATA)<256 and len(TBS_DATA)<256 and len(SIGNATURE)==128
CHILD,ISSUER,PATH,PUB,OID,PUBOID,PARAM,TBS,SIG=[OBJ+v for v in(0x1000,0x1800,0x2000,0x3000,0x4000,0x4100,0x4200,0x4300,0x4400)]

def run(corrupt=False,missing_params=False):
    u=source.context();calls=[];heap=OBJ+0x5000;allocated={};steps=0;digest_pc=None;tail=[]
    def put(p,n):u.mem_write(p,struct.pack('<I',n&0xffffffff))
    raw_sig=SIGNATURE[:-1]+bytes([SIGNATURE[-1]^1])if corrupt else SIGNATURE
    alg=bytes.fromhex('2a864886f70d01010b');pubalg=bytes.fromhex('2a864886f70d010101')
    for address,raw in((PUB,PUB_DATA),(OID,alg),(PUBOID,pubalg),(PARAM,b'\x05\x00'),(TBS,TBS_DATA),(SIG,raw_sig)):u.mem_write(address,raw)
    put(CHILD+0x3c,OID);put(CHILD+0x40,len(alg));put(CHILD+0x44,PARAM);put(CHILD+0x48,2)
    put(CHILD+0x10,TBS);put(CHILD+0x14,len(TBS_DATA));put(CHILD+0x28,SIG);put(CHILD+0x2c,len(raw_sig))
    put(ISSUER+0x74,PUBOID);put(ISSUER+0x78,len(pubalg));put(ISSUER+0x7c,PARAM if not missing_params else 0);put(ISSUER+0x80,2 if not missing_params else 0);put(ISSUER+0x84,PUB);put(ISSUER+0x88,len(PUB_DATA))
    put(PATH+8,2);put(PATH+0x10,0);put(PATH+0x14,0xffffffff);put(PATH+0x1c,CHILD);put(PATH+0x20,ISSUER)
    def read(address,n):
        return bytes(u.mem_read(address,n)) if OBJ<=address<OBJ+0x10000 or STACK<=address<STACK+0x10000 else source.cache_read(address,n)
    def hook(mc,pc,size,cookie):
        nonlocal heap,steps,digest_pc
        steps+=1;tail.append(hex(pc));tail[:]=tail[-20:]
        r0,r1,r2,r3=[mc.reg_read(r)for r in(UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3)]
        def ret(v):
            lr=mc.reg_read(UC_ARM_REG_LR);state=mc.reg_read(UC_ARM_REG_CPSR)
            mc.reg_write(UC_ARM_REG_R0,v&0xffffffff);mc.reg_write(UC_ARM_REG_PC,lr)
            mc.reg_write(UC_ARM_REG_CPSR,(state&~0x20)|((lr&1)<<5))
        if pc==0x20ca7d6c:ret(42);return
        if pc in(0x208090d4,0x20cc86c8):
            n=r2+8 if pc==0x208090d4 else r0*r1
            assert 0<n<=4096 and heap+n<OBJ+0xf000
            out=heap;heap+=(n+15)&~15;allocated[out]=n;mc.mem_write(out,b'\0'*n)
            calls.append({'synthetic_allocator':'CFInstance'if pc==0x208090d4 else'calloc','requested':n});ret(out);return
        if pc==0x20cc3658:calls.append({'synthetic_logging_boundary':True});ret(0);return
        if pc==source.exports['_CFRelease']:
            assert r0 in allocated;calls.append({'synthetic_release':True});ret(0);return
        if pc==0x204513c4:
            assert 0<=r3<=r1<=4096;mc.mem_write(r0,bytes([r2&255])*r3);ret(0);return
        if pc in(0x204513b4,0x20cc8f68):
            assert r2<=4096;mc.mem_write(r0,bytes([r1&255])*r2);ret(r0);return
        if pc in(0x20451394,0x204513a4,0x20cc8f48):
            assert r2<=4096;mc.mem_write(r0,read(r1,r2));ret(r0);return
        if pc in(0x20451384,0x20cc8f38):
            assert r2<=4096;x=read(r0,r2);y=read(r1,r2);ret(0 if x==y else 1);return
        if pc==0x20ca8e8a:
            digest_pc=r3&~1;assert r0==TBS and r1==len(TBS_DATA) and mc.reg_read(UC_ARM_REG_R8)==32
            calls.append({'python_sha256_digest_provider':hex(digest_pc),'digest_length':32})
        if digest_pc is not None and pc==digest_pc:
            assert r0==TBS and r1==len(TBS_DATA);mc.mem_write(r2,hashlib.sha256(TBS_DATA).digest());ret(0);return
        if pc==0x20ca84bc:
            alglen,algptr=struct.unpack('<2I',mc.mem_read(r1,8))
            plen,pptr=struct.unpack('<2I',mc.mem_read(r2,8))
            publen,pubptr=struct.unpack('<2I',mc.mem_read(r3,8))
            assert read(algptr,alglen)==pubalg and read(pubptr,publen)==PUB_DATA
            calls.append({'actual_factory_public_der_sha256':hashlib.sha256(read(pubptr,publen)).hexdigest(),'selected_parameter_length':plen,'parameter_present':bool(pptr)})
        if pc==0x20cb18e4:calls.append({'original_RSA_public_verify':True})
        if pc==crypto_symbols['_ccrsa_pub_crypt']['value']:calls.append({'original_corecrypto_RSA_public_modexp':True})
        if pc==0x20cb17d8:calls.append({'native_public_import_result':r0})
        if 0x20451164<=pc<=0x204514a4:raise AssertionError(('unprovided corecrypto stub',hex(pc),r0,r1,r2,r3))
        if pc==0x2051f69c:raise AssertionError('stackguard')
        if pc in(0x20c9e638,0x20c9e488,0x20c9a9d0,0x20ca8bcc):calls.append({'actual_entry':hex(pc)})
    u.hook_add(UC_HOOK_CODE,hook);u.reg_write(UC_ARM_REG_R0,PATH)
    try:u.emu_start(0x20c9e639,END,count=2000000)
    except Exception as e:raise RuntimeError(f'pc={u.reg_read(UC_ARM_REG_PC):x},calls={calls},tail={tail}')from e
    assert u.reg_read(UC_ARM_REG_PC)==END,(hex(u.reg_read(UC_ARM_REG_PC)),steps)
    result=u.reg_read(UC_ARM_REG_R0)
    assert result==int(corrupt),(corrupt,result,calls)
    assert any('original_RSA_public_verify'in c for c in calls)
    assert any('original_corecrypto_RSA_public_modexp'in c for c in calls),calls
    return {'synthetic_corrupt_signature':corrupt,'synthetic_missing_issuer_parameters':missing_params,'native_path_verify_result':result,'native_completed_links':struct.unpack('<I',u.mem_read(PATH+0x10,4))[0],'instruction_count':steps,'calls':calls}
rows=[run(),run(True),run(False,True)]
print(json.dumps({'cache_sha256':CACHE_SHA,'securityd_sha256':DAEMON_SHA,'scope':'actual native two-node RSA certificate-link crypto; synthetic certificate fields/CF/heap, Python real SHA256 provider; no native X509 parser/full trust','fixture_sha256':hashlib.sha256(FIXTURE.read_bytes()).hexdigest(),'certificate_chain_trusted':None,'authorization_granted':False,'cases':rows},indent=2,sort_keys=True))
