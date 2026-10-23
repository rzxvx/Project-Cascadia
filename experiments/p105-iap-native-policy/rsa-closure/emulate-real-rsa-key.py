# SPDX-License-Identifier: GPL-2.0
"""Original RSA public DER decoding and native weak-key test, fake CF/heap.

Synthetic certificate field objects only; no certificate DER parser or trust.
Public modulus/exponent fixture contains no Apple or private key material.
"""
import argparse,json,struct
from pathlib import Path
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import *
from native13 import Sources,OBJ,STACK,END,CACHE_SHA,DAEMON_SHA
p=argparse.ArgumentParser();p.add_argument('cache',type=Path);p.add_argument('daemon',type=Path);a=p.parse_args();source=Sources(a.cache,a.daemon)
CERT,OID,PARAM,PUB=OBJ+0x1000,OBJ+0x2000,OBJ+0x2100,OBJ+0x3000
# Independently qualify actual named API/descriptor and imported libc veneers.
for name,address in {'_SecCertificateIsWeak':0x20c9b510,'_SecKeyGetBlockSize':0x20ca8860,'_SecKeyGetAlgorithmIdentifier':0x20ca7ed8,'_SecKeyCreateRSAPublicKey':0x20cb1f9c}.items():
    assert source.exports[name]==address,(name,source.exports.get(name))
assert source.symbols['_SecPVCParentCertificateChecks']['value']==0x33980
assert struct.unpack('<I',source.cache_read(0x36b033e8+0x28,4))[0]==0x20cb1e39
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


def length(n):
    if n<128:return bytes([n])
    b=n.to_bytes((n.bit_length()+7)//8,'big');return bytes([0x80+len(b)])+b

def integer(n):
    b=n.to_bytes((n.bit_length()+7)//8,'big')
    if b[0]&128:b=b'\0'+b
    return b'\x02'+length(len(b))+b

def public_der(bits):
    # Source-owned odd public test integer; factorization/private key absent.
    body=integer((1<<(bits-1))|0x12345)+integer(65537)
    return b'\x30'+length(len(body))+body

def run(bits,malformed=False,parent=False):
    u=source.context();calls=[];heap=OBJ+0x5000;allocated={};steps=0;tail=[]
    def put(p,n):u.mem_write(p,struct.pack('<I',n))
    raw=public_der(bits)
    if malformed:raw=raw[:-2]
    oid=bytes.fromhex('2a864886f70d010101')
    u.mem_write(OID,oid);u.mem_write(PARAM,b'\x05\x00');u.mem_write(PUB,raw)
    policies,policy,options,path=[OBJ+v for v in(0xa000,0xa100,0xa200,0xa300)]
    put(OBJ+4,policies);put(OBJ+0x10,path);put(OBJ+0x24,0);u.mem_write(OBJ+0x35,b"\1");put(policy+0xc,options)
    weak_root_key=struct.unpack("<I",source.cache_read(source.exports["_kSecPolicyCheckWeakRoot"],4))[0]
    put(CERT+0x74,OID);put(CERT+0x78,len(oid));put(CERT+0x7c,PARAM);put(CERT+0x80,2);put(CERT+0x84,PUB);put(CERT+0x88,len(raw))
    def hook(mc,pc,size,cookie):
        nonlocal heap,steps
        steps+=1;tail.append(hex(pc));tail[:]=tail[-25:]
        r0,r1,r2,r3=[mc.reg_read(r)for r in(UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3)]
        def ret(v):
            lr=mc.reg_read(UC_ARM_REG_LR)
            mc.reg_write(UC_ARM_REG_R0,v)
            mc.reg_write(UC_ARM_REG_PC,lr)
            mc.reg_write(UC_ARM_REG_CPSR,(mc.reg_read(UC_ARM_REG_CPSR)&~0x20)|((lr&1)<<5))
        name=source.stubs.get(pc)
        if parent and name=='_SecCertificatePathGetCount':assert r0==path;ret(2);return
        if parent and name=='_SecCertificatePathGetCertificateAtIndex':assert r0==path and r1==1;ret(CERT);return
        if parent and name=='_SecCertificatePathIsAnchored':assert r0==path;ret(1);return
        if parent and name=='_CFArrayGetValueAtIndex':assert r0==policies and r1==0;ret(policy);return
        if parent and name=='_CFDictionaryContainsKey':assert r0==options;ret(int(r1==weak_root_key));return
        if parent and name=='_SecCertificateIsWeak':
            assert r0==CERT;calls.append({'original_weak_callback_in_parent':True});mc.reg_write(UC_ARM_REG_PC,0x20c9b511);return
        if parent and name=='_SecCertificateIsValid':assert r0==CERT;ret(1);return
        if parent and name=='_SecCertificateGetBasicConstraints':assert r0==CERT;ret(0);return
        if parent and name:raise AssertionError(('unprovided parent API',name,hex(pc)))
        if pc==0x20ca7d6c:ret(42);return
        if pc in(0x208090d4,0x20cc86c8):
            n=r2+8 if pc==0x208090d4 else r0*r1
            assert 0<n<=4096 and heap+n<OBJ+0xf000
            out=heap;heap+=(n+15)&~15;allocated[out]=n;mc.mem_write(out,b'\0'*n)
            calls.append({'synthetic_allocator':'CFInstance'if pc==0x208090d4 else'calloc','requested':n});ret(out);return
        if pc==0x20cc3658:
            calls.append({'synthetic_log_boundary':True,'argument_r3':mc.reg_read(UC_ARM_REG_R3)});ret(0);return
        if pc==0x20cb17d8:calls.append({'native_import_result':r0})
        if pc==0x20c9b524:calls.append({'native_blocksize':r0})
        if pc==0x20c9b52c:calls.append({'native_algorithm_identifier':r0})
        if pc==source.exports['_CFRelease']:
            assert r0 in allocated;calls.append({'synthetic_release':True});ret(0);return
        if pc==0x204513c4:
            assert 0<=r3<=r1<=4096;mc.mem_write(r0,bytes([r2&255])*r3);ret(0);return
        if pc==0x204513b4:
            assert r2<=4096;mc.mem_write(r0,bytes([r1&255])*r2);ret(r0);return
        if pc in(0x20451394,0x204513a4):
            assert r2<=4096;mc.mem_write(r0,bytes(mc.mem_read(r1,r2)));ret(r0);return
        if pc==0x20451384:
            assert r2<=4096;x=bytes(mc.mem_read(r0,r2));y=bytes(mc.mem_read(r1,r2));ret(0 if x==y else 1);return
        if pc==0x20cc8f68:
            assert r2<=4096;mc.mem_write(r0,bytes([r1&255])*r2);ret(r0);return
        if pc==0x20cc8f48:
            assert r2<=4096;mc.mem_write(r0,bytes(mc.mem_read(r1,r2)));ret(r0);return
        if 0x20451164<=pc<=0x204514a4:raise AssertionError(('unprovided corecrypto stub',hex(pc),r0,r1,r2,r3))
        if pc==0x2051f69c:raise AssertionError('stackguard')
        if pc in(0x20c9b510,0x20c9b4bc,0x20ca84bc,0x20cb1f9c,0x20ca8638,0x20cb1760,0x2044d80c,0x2044db28,0x20cb1e38,0x20ca7ed8):calls.append({'actual_entry':hex(pc)})
    u.hook_add(UC_HOOK_CODE,hook);u.reg_write(UC_ARM_REG_R0,OBJ if parent else CERT);u.reg_write(UC_ARM_REG_R1,1 if parent else 0)
    try:u.emu_start((0x33980 if parent else 0x20c9b510)|1,END,count=1000000)
    except Exception as e:raise RuntimeError(f'bits={bits},pc={u.reg_read(UC_ARM_REG_PC):x},regs={[hex(u.reg_read(r))for r in(UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3,UC_ARM_REG_R4,UC_ARM_REG_R5,UC_ARM_REG_R6,UC_ARM_REG_SP,UC_ARM_REG_LR,UC_ARM_REG_CPSR)]},calls={calls},tail={tail}')from e
    assert u.reg_read(UC_ARM_REG_PC)==END,(hex(u.reg_read(UC_ARM_REG_PC)),steps)
    result=not bool(u.mem_read(OBJ+0x35,1)[0]) if parent else bool(u.reg_read(UC_ARM_REG_R0))
    assert result==bool(malformed or(bits+7)//8<128),(bits,result,calls)
    return {'original_parent_processing':parent,'synthetic_truncated_der':malformed,'public_test_modulus_bits':bits,'public_test_exponent':65537,'native_weak':bool(result),'instruction_count':steps,'calls':calls}
rows=[run(bits)for bits in(512,1023,1024,2048)]+[run(1024,True)]+[run(bits,parent=True)for bits in(512,1024)]
print(json.dumps({'cache_sha256':CACHE_SHA,'securityd_sha256':DAEMON_SHA,'scope':'actual RSA DER-import/key descriptor/blocksize/algo/weak path; certificate fields/CF/heap synthetic','cases':rows},indent=2,sort_keys=True))
