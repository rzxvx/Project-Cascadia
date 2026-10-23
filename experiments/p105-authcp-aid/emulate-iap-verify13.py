"""Original IAP verify control flow; CF/Security/crypto are explicitly synthetic."""
from pathlib import Path
import argparse,hashlib,json,mmap,struct
from unicorn import Uc,UC_ARCH_ARM,UC_MODE_THUMB,UC_HOOK_CODE,UC_HOOK_MEM_READ_UNMAPPED
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
OBJ,NUM,CERTDATA,NONCEDATA,SIGDATA=0x10000000,0x10001000,0x10002000,0x10003000,0x10004000
CERT,NONCE,SIG,LEAFCERT,CARRAY,POLICY,TRUST,ANCHORS,PUBKEY=0x10005000,0x10006000,0x10007000,0x10008000,0x10009000,0x1000a000,0x1000b000,0x1000c000,0x1000d000
STACK,END=0x20000000,0x30000ffc

def run(version=2,trust_result=4,trust_error=0,verify_error=0,public_key=True,parse_ok=True,case='success'):
 mu=Uc(UC_ARCH_ARM,UC_MODE_THUMB);mu.mem_map(BASE,0x57000);mu.mem_write(BASE,get(BASE,0x57000))
 mu.mem_map(OBJ,0x10000);mu.mem_map(STACK,0x10000);mu.mem_map(END&~4095,4096)
 def put(va,v):mu.mem_write(va,struct.pack('<I',v&0xffffffff))
 mapped=set()
 def ensure(va):
  page=va&~4095
  if page not in mapped:mu.mem_map(page,4096);mu.mem_write(page,get(page,4096));mapped.add(page)
 def unmapped(mc,typ,va,size,val,_):ensure(va);return True
 mu.hook_add(UC_HOOK_MEM_READ_UNMAPPED,unmapped)
 # Synthetic already-initialized native once-context, explicitly not real process state.
 once=0x31a0ea1c+0x05c985f0;flag=0x31a0ea30+0x05c985e0
 ensure(once);ensure(flag);put(once,0xffffffff);mu.mem_write(flag,b'\0')
 nonce_len=32 if version==3 else 20;sig_len=64 if version==3 else 128
 mu.mem_write(CERT,b'\x11'*16);mu.mem_write(NONCE,bytes(range(nonce_len)));mu.mem_write(SIG,bytes((i+0x80)&255 for i in range(sig_len)))
 calls=[];alloc=0;certn=0
 def hook(mc,pc,size,_):
  nonlocal alloc,certn
  r0,r1,r2,r3=[mc.reg_read(r)for r in(UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3)]
  symbol=stubs.get(pc)
  if pc==0x31a0ec28:
   calls.append({'mock':'source certificate/anchor parser boundary','input_length':r1});out=LEAFCERT if parse_ok else 0
  elif pc==0x31a0ec9c:calls.append({'mock':'diagnostic cert helper'});out=0
  elif symbol=='_CFArrayCreateMutable':alloc+=1;out=CARRAY if alloc==1 else ANCHORS;calls.append({'mock':symbol})
  elif symbol=='_CFArrayAppendValue':calls.append({'mock':symbol,'array_kind':'chain'if r0==CARRAY else'anchors'});out=0
  elif symbol=='_CFRelease':out=0
  elif symbol=='_SecPolicyCreateiAP':calls.append({'mock':symbol});out=POLICY
  elif symbol=='_SecTrustCreateWithCertificates':assert r0==CARRAY and r1==POLICY;put(r2,TRUST);calls.append({'mock':symbol});out=0
  elif symbol=='_SecCertificateCreateWithBytes':certn+=1;calls.append({'mock':symbol,'source_anchor_number':certn,'input_length':r2});out=LEAFCERT+4*certn
  elif symbol=='_SecTrustSetAnchorCertificates':assert r0==TRUST and r1==ANCHORS;calls.append({'mock':symbol});out=0
  elif symbol=='_SecTrustEvaluate':assert r0==TRUST;put(r1,trust_result);calls.append({'mock':symbol,'synthetic_error':trust_error,'synthetic_result':trust_result});out=trust_error
  elif symbol=='_SecTrustCopyPublicKey':assert r0==TRUST;calls.append({'mock':symbol});out=PUBKEY if public_key else 0
  elif symbol=='_SecKeyRawVerify':
   spv=mc.reg_read(UC_ARM_REG_SP);sigptr,sigsize=struct.unpack('<II',mu.mem_read(spv,8))
   assert r0==PUBKEY and r2==NONCE and r3==nonce_len and sigptr==SIG and sigsize==sig_len
   calls.append({'mock':symbol,'padding':hex(r1),'nonce_length':r3,'signature_length':sigsize,'synthetic_error':verify_error});out=verify_error
  elif symbol in('_asl_log','_syslog'):calls.append({'mock':'diagnostic log'});out=0
  else:return
  mc.reg_write(UC_ARM_REG_R0,out&0xffffffff);mc.reg_write(UC_ARM_REG_PC,mc.reg_read(UC_ARM_REG_LR))
 mu.hook_add(UC_HOOK_CODE,hook);spv=STACK+0xff00;put(spv,SIG);put(spv+4,sig_len);put(spv+8,version)
 for reg,val in((UC_ARM_REG_R0,CERT),(UC_ARM_REG_R1,16),(UC_ARM_REG_R2,NONCE),(UC_ARM_REG_R3,nonce_len),(UC_ARM_REG_SP,spv),(UC_ARM_REG_LR,END|1)):mu.reg_write(reg,val)
 try:mu.emu_start(0x31a0e871,END,count=6000)
 except Exception as e:raise RuntimeError(f'{version}/{case} PC={mu.reg_read(UC_ARM_REG_PC):#x}, calls={calls}')from e
 assert mu.reg_read(UC_ARM_REG_PC)==END
 return {'version':version,'case':case,'result':mu.reg_read(UC_ARM_REG_R0),'calls':calls,'scope':'actual verify control flow with synthetic certificate parse/CF/Security/crypto/once context; no real trust or signature verification'}
rows=[run(2),run(3),run(2,verify_error=-1,case='bad_signature'),run(3,verify_error=-1,case='bad_signature'),run(2,trust_result=1,case='proceed_not_native_four'),run(2,trust_result=2,case='untrusted_result'),run(3,trust_result=2,case='untrusted_result'),run(2,trust_error=-1,case='trust_API_error'),run(3,public_key=False,case='missing_public_key'),run(2,parse_ok=False,case='parse_failed'),run(4,case='unknown_version')]
assert [r['result']for r in rows]==[1,1,0,0,0,0,0,0,0,0,0]
assert [next(c['padding']for c in r['calls']if c.get('mock')=='_SecKeyRawVerify')for r in rows[:4]]==['0x8002','0x4000','0x8002','0x4000']
assert all(not any(c.get('mock')=='_SecKeyRawVerify'for c in r['calls'])for r in rows[4:])
print(json.dumps({'cache_sha256':SHA,'no_hardware':True,'no_real_crypto':True,'cases':rows},indent=2))
