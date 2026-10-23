# SPDX-License-Identifier: GPL-2.0
"""Original IAP verify control flow; CF/Security/crypto are explicitly synthetic."""
from pathlib import Path
import argparse,hashlib,json,mmap,struct
from unicorn import Uc,UC_ARCH_ARM,UC_MODE_THUMB,UC_HOOK_CODE,UC_HOOK_MEM_READ_UNMAPPED,UC_HOOK_MEM_FETCH_UNMAPPED
from unicorn.arm_const import *
ap=argparse.ArgumentParser();ap.add_argument('cache',type=Path);args=ap.parse_args()
SHA='4f5b5d73244c53fd0bdc5b8012106b10d0dd7607cc850bf7c8f9e9ef3a3f733b'
h=hashlib.sha256()
with args.cache.open('rb')as f:
 while part:=f.read(2**20):h.update(part)
assert h.hexdigest()==SHA
f=args.cache.open('rb');D=mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ)
mo,mc,io,ic=struct.unpack_from('<4I',D,16)
MAP=[struct.unpack_from('<3Q2I',D,mo+32*i)for i in range(mc)]
def off(va):
 row=next((r for r in MAP if r[0]<=va<r[0]+r[1]),None)
 assert row is not None,hex(va)
 return row[2]+va-row[0]
def get(va,n):return D[off(va):off(va)+n]
images={}
for i in range(ic):
 va,_,_,po,_=struct.unpack_from('<3Q2I',D,io+32*i);images[D[po:D.find(b'\0',po)].decode()]=va
BASE=images['/System/Library/PrivateFrameworks/IAPAuthentication.framework/IAPAuthentication'];assert BASE==0x31a0d000
sections=[]
pos=off(BASE)+28
for _ in range(struct.unpack_from('<I',D,off(BASE)+16)[0]):
 cmd,z=struct.unpack_from('<II',D,pos)
 if cmd==2:so,n,st,ss=struct.unpack_from('<4I',D,pos+8)
 if cmd==11:it,itn=struct.unpack_from('<2I',D,pos+56)
 if cmd==1:
  for j in range(struct.unpack_from('<I',D,pos+48)[0]):
   q=pos+56+68*j;sections.append((D[q:q+16].split(b'\0')[0].decode(),*struct.unpack_from('<9I',D,q+32)))
 pos+=z
names={i:D[st+struct.unpack_from('<I',D,so+12*i)[0]:D.find(b'\0',st+struct.unpack_from('<I',D,so+12*i)[0])].decode()for i in range(n)}
stubs={}
for nm,va,sz,fo,_,_,_,flags,r1,r2 in sections:
 if flags&255==8:
  for j in range(sz//r2):stubs[va+j*r2]=names[struct.unpack_from('<I',D,it+4*(r1+j))[0]]
assert stubs[0x31a0fe20]=='_SecKeyRawVerify'
assert stubs[0x31a0fe30]=='_SecPolicyCreateiAP'
assert stubs[0x31a0fe50]=='_SecTrustCopyPublicKey'
assert stubs[0x31a0fe70]=='_SecTrustEvaluate'
assert stubs[0x31a0ff10]=='_arc4random_buf'
OBJ,STACK,END,ONCERET=0x10000000,0x20000000,0x30000ffc,0x30000f00
CERT,NONCE,SIG,CARRAY,ANCHORS,TRUST,PUBKEY,POLICY= [OBJ+0x1000*i for i in range(1,9)]
ONCE=0x376a700c;FLAG=0x376a7010
# Addresses below are derived from actual source PIC arithmetic, then checked.
assert ONCE==0x31a0ea18+4+0x05c985f0
assert FLAG==0x31a0ea2c+4+0x05c985e0
assert stubs[0x31a10150]=='_CFPreferencesGetAppBooleanValue'
assert stubs[0x31a0fdd0]=='_SecCMSCertificatesOnlyMessageCopyCertificates'
assert stubs[0x31a100e0]=='_CFDataCreateWithBytesNoCopy'
assert stubs[0x31a0ff30]=='_dispatch_once'
assert stubs[0x31a100a0]=='_CFArrayGetCount'
assert stubs[0x31a100b0]=='_CFArrayGetValueAtIndex'

def cfstring(va):
 isa,flags,pointer,size=struct.unpack('<4I',get(va,16));assert flags==0x7c8 and size<128
 return get(pointer,size).decode()

def run(version,supported=False,leaf_count=1,trust_result=4):
 mu=Uc(UC_ARCH_ARM,UC_MODE_THUMB);mu.mem_map(BASE,0x57000);mu.mem_write(BASE,get(BASE,0x57000));mu.mem_map(OBJ,0x20000);mu.mem_map(STACK,0x10000);mu.mem_map(END&~4095,4096)
 mapped=set(range(BASE,BASE+0x57000,4096));calls=[];objects={};arrays={};next_object=OBJ+0x10000;once_return=None
 def put(va,value):mu.mem_write(va,struct.pack('<I',value&0xffffffff))
 def ensure(va):
  page=va&~4095
  if page not in mapped:mu.mem_map(page,4096);mu.mem_write(page,get(page,4096));mapped.add(page)
 def unmapped(mc,typ,va,size,value,_):ensure(va);return True
 mu.hook_add(UC_HOOK_MEM_READ_UNMAPPED,unmapped);mu.hook_add(UC_HOOK_MEM_FETCH_UNMAPPED,unmapped)
 ensure(ONCE);ensure(FLAG);put(ONCE,0);mu.mem_write(FLAG,b'\0')
 nonce_size=32 if version==3 else 20;signature_size=64 if version==3 else 128
 mu.mem_write(CERT,b'X'*16);mu.mem_write(NONCE,bytes(range(nonce_size)));mu.mem_write(SIG,b'S'*signature_size)
 def object_of(kind,content):
  nonlocal next_object
  pointer=next_object;next_object+=0x40;objects[pointer]=(kind,content);return pointer
 def hook(mc,pc,size,_):
  nonlocal once_return
  r0,r1,r2,r3=[mc.reg_read(r)for r in (UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3)]
  symbol=stubs.get(pc);out=0
  if pc==ONCERET:
   assert once_return is not None;put(ONCE,0xffffffff);mc.reg_write(UC_ARM_REG_R0,0);mc.reg_write(UC_ARM_REG_PC,once_return);return
  if symbol=='_dispatch_once':
   assert r0==ONCE;assert struct.unpack('<I',get(r1+12,4))[0]==0x31a0f97d
   calls.append({'native_once_block':'0x31a0f97c'})
   once_return=mc.reg_read(UC_ARM_REG_LR);mc.reg_write(UC_ARM_REG_R0,r1);mc.reg_write(UC_ARM_REG_LR,ONCERET|1);mc.reg_write(UC_ARM_REG_PC,0x31a0f97d);return
  if symbol=='_CFPreferencesGetAppBooleanValue':
   key,domain=cfstring(r0),cfstring(r1);assert(key,domain,r2)==('AppleIDAuth30Supported','com.apple.iaptransportd',0)
   calls.append({'preferences':{'key':key,'domain':domain,'synthetic_value':supported}});out=int(supported)
  elif symbol=='_CFDataCreateWithBytesNoCopy':
   if r1==CERT:content={'kind':'synthetic-peer-container','length':r2};assert r2==16
   else:
    raw=get(r1,r2);content={'kind':'source-v3-container','address':hex(r1),'length':r2,'sha256':hashlib.sha256(raw).hexdigest()};assert(r1,r2)==(0x31a11a10,637)
   out=object_of('data',content)
  elif symbol=='_SecCMSCertificatesOnlyMessageCopyCertificates':
   kind,content=objects[r0];assert kind=='data';calls.append({'certificates_only_parser_boundary':content})
   count=leaf_count if content['kind']=='synthetic-peer-container'else 1
   values=[object_of('cert',{'from':content['kind'],'index':i})for i in range(count)]
   out=object_of('array',values);arrays[out]=values
  elif symbol=='_CFArrayGetCount':out=len(arrays[r0])
  elif symbol=='_CFArrayGetValueAtIndex':assert r1<len(arrays[r0]);out=arrays[r0][r1];calls.append({'select_certificate_index':r1,'source':objects[out][1]['from']})
  elif symbol=='_CFRetain' or pc==0x20808d4c:out=r0
  elif symbol=='_CFArrayCreateMutable':out=object_of('array',[]);arrays[out]=[]
  elif symbol=='_CFArrayAppendValue':assert r0 in arrays;arrays[r0].append(r1)
  elif symbol=='_CFRelease':out=0
  elif symbol=='_SecPolicyCreateiAP':out=POLICY
  elif symbol=='_SecTrustCreateWithCertificates':
   assert len(arrays[r0])==1 and objects[arrays[r0][0]][1]=={'from':'synthetic-peer-container','index':0};put(r2,TRUST)
   calls.append({'trust_peer_certificate_count':len(arrays[r0]),'whole_input_container_certificate_count':leaf_count});out=0
  elif symbol=='_SecCertificateCreateWithBytes':
   raw=get(r1,r2);calls.append({'source-v2-DER-anchor':{'address':hex(r1),'length':r2,'sha256':hashlib.sha256(raw).hexdigest()}})
   assert(r1,r2)in[(0x31a111c0,1091),(0x31a11608,1026)];out=object_of('cert',{'from':'source-v2-anchor'})
  elif symbol=='_SecTrustSetAnchorCertificates':assert r0==TRUST;calls.append({'supplied_anchor_count':len(arrays[r1]),'synthetic_API_return':0});out=0
  elif symbol=='_SecTrustEvaluate':assert r0==TRUST;put(r1,trust_result);calls.append({'synthetic_trust_result':trust_result});out=0
  elif symbol=='_SecTrustCopyPublicKey':assert r0==TRUST;out=PUBKEY
  elif symbol=='_SecKeyRawVerify':calls.append({'synthetic_raw_verify_success':True,'padding':hex(r1)});out=0
  elif symbol in('_asl_log','_syslog'):out=0
  elif pc==0x31a0ec9c:out=0 # diagnostic-only boundary
  else:return
  mc.reg_write(UC_ARM_REG_R0,out);mc.reg_write(UC_ARM_REG_PC,mc.reg_read(UC_ARM_REG_LR))
 mu.hook_add(UC_HOOK_CODE,hook);sp=STACK+0xff00;put(sp,SIG);put(sp+4,signature_size);put(sp+8,version)
 for reg,value in[(UC_ARM_REG_R0,CERT),(UC_ARM_REG_R1,16),(UC_ARM_REG_R2,NONCE),(UC_ARM_REG_R3,nonce_size),(UC_ARM_REG_SP,sp),(UC_ARM_REG_LR,END|1)]:mu.reg_write(reg,value)
 try:mu.emu_start(0x31a0e871,END,count=7000)
 except Exception as e:raise RuntimeError(f'pc={mu.reg_read(UC_ARM_REG_PC):x}, calls={calls}')from e
 assert mu.reg_read(UC_ARM_REG_PC)==END
 return {'version':version,'synthetic_supported_preference':supported,'synthetic_leaf_container_count':leaf_count,'synthetic_trust_result':trust_result,'caller_result':mu.reg_read(UC_ARM_REG_R0),'calls':calls}
rows=[run(2),run(3,True),run(3,False,trust_result=5),run(3,True,leaf_count=2),run(2,leaf_count=0)]
assert [r['caller_result']for r in rows]==[1,1,0,1,0]
assert [[c['supplied_anchor_count']for c in r['calls']if 'supplied_anchor_count'in c]for r in rows]==[[2],[1],[0],[1],[]]
assert not any('synthetic_raw_verify_success'in c for c in rows[2]['calls']+rows[4]['calls'])
print(json.dumps({'cache_sha256':SHA,'scope':'actual anchor selection/once/parser caller; CF/CMS/trust/crypto are synthetic boundaries; no actual trust result','cases':rows},indent=2))
