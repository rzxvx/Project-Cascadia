# SPDX-License-Identifier: GPL-2.0
"""Actual13G36 anchor-only and weak-key leaves; synthetic CF/public-key providers."""
import argparse,hashlib,json,mmap,struct
from pathlib import Path
from unicorn import Uc,UC_ARCH_ARM,UC_MODE_THUMB,UC_HOOK_CODE,UC_HOOK_MEM_READ_UNMAPPED,UC_HOOK_MEM_FETCH_UNMAPPED
from unicorn.arm_const import *
ap=argparse.ArgumentParser();ap.add_argument('cache',type=Path);args=ap.parse_args();h=hashlib.sha256();f=args.cache.open('rb')
while b:=f.read(2**20):h.update(b)
assert h.hexdigest()=='4f5b5d73244c53fd0bdc5b8012106b10d0dd7607cc850bf7c8f9e9ef3a3f733b'
D=mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ);mo,mc,io,ic=struct.unpack_from('<4I',D,16);M=[struct.unpack_from('<3Q2I',D,mo+32*i)for i in range(mc)]
def get(va,n):
 r=next(r for r in M if r[0]<=va and va+n<=r[0]+r[1]);o=r[2]+va-r[0];return D[o:o+n]
SYMS={}
for i in range(ic):
 va,_,_,po,_=struct.unpack_from('<3Q2I',D,io+32*i);name=D[po:D.find(b'\0',po)].decode()
 if not any(v in name for v in ['Security.framework/Security','CoreFoundation.framework/CoreFoundation']):continue
 r=next(r for r in M if r[0]<=va<r[0]+r[1]);o=r[2]+va-r[0];pos=o+28
 for _ in range(struct.unpack_from('<I',D,o+16)[0]):
  cmd,z=struct.unpack_from('<II',D,pos)
  if cmd==2:so,n,st,ss=struct.unpack_from('<4I',D,pos+8)
  pos+=z
 for i in range(n):
  si,nt,ns,nd,value=struct.unpack_from('<IBBHI',D,so+12*i)
  if value:SYMS[D[st+si:D.find(b'\0',st+si)].decode()]=value
PINS={'_SecTrustSetAnchorCertificates':0x20cb44a0,'_SecTrustSetAnchorCertificatesOnly':0x20cb4484,'_SecCertificateIsWeak':0x20c9b510,'_SecCertificateCopyPublicKey':0x20c9b4bc,'_SecKeyGetBlockSize':0x20ca8860,'_SecKeyGetAlgorithmIdentifier':0x20ca7ed8,'_CFRetain':0x20808d4c,'_CFRelease':0x2080b5ec}
for name,a in PINS.items():assert SYMS.get(name)==a,(name,SYMS.get(name),a)
OBJ,STACK,END,OLD,NEW,KEY,CERT=0x10000000,0x20000000,0x30000ffc,0x10001000,0x10002000,0x10003000,0x10004000

def context(callback):
 mu=Uc(UC_ARCH_ARM,UC_MODE_THUMB);mu.mem_map(OBJ,0x10000);mu.mem_map(STACK,0x10000);mu.mem_map(END&~4095,4096)
 mapped=set()
 def unmapped(mc,kind,va,size,value,cookie):
  page=va&~4095
  if page not in mapped:mu.mem_map(page,4096);mu.mem_write(page,get(page,4096));mapped.add(page)
  return True
 mu.hook_add(UC_HOOK_MEM_READ_UNMAPPED,unmapped);mu.hook_add(UC_HOOK_MEM_FETCH_UNMAPPED,unmapped);mu.hook_add(UC_HOOK_CODE,callback)
 mu.reg_write(UC_ARM_REG_SP,STACK+0xff00);mu.reg_write(UC_ARM_REG_LR,END|1)
 return mu

def run_anchor(supplied=True,old=True,null_trust=False,anchor_count=2):
 calls=[]
 def callback(mc,pc,size,cookie):
  if pc==PINS['_CFRetain']:calls.append({'retain':'new'if mc.reg_read(UC_ARM_REG_R0)==NEW else'bad'});out=mc.reg_read(UC_ARM_REG_R0)
  elif pc==PINS['_CFRelease']:calls.append({'release':'old'if mc.reg_read(UC_ARM_REG_R0)==OLD else'bad'});out=0
  else:return
  mc.reg_write(UC_ARM_REG_R0,out);mc.reg_write(UC_ARM_REG_PC,mc.reg_read(UC_ARM_REG_LR))
 mu=context(callback);mu.mem_write(OBJ+0xc,struct.pack('<I',OLD if old else 0));mu.mem_write(OBJ+0x38,struct.pack('<I',4));mu.mem_write(OBJ+0x3c,b'\0')
 mu.reg_write(UC_ARM_REG_R0,0 if null_trust else OBJ);mu.reg_write(UC_ARM_REG_R1,NEW if supplied else 0)
 mu.emu_start(PINS['_SecTrustSetAnchorCertificates']|1,END,count=250);assert mu.reg_read(UC_ARM_REG_PC)==END
 rc=mu.reg_read(UC_ARM_REG_R0);rc=rc if rc<2**31 else rc-2**32
 row={'supplied_array':supplied,'synthetic_array_count':anchor_count,'old_array':old,'null_trust':null_trust,'return':rc,'anchors_only':bool(mu.mem_read(OBJ+0x3c,1)[0]),'trust_result':struct.unpack('<I',mu.mem_read(OBJ+0x38,4))[0],'stored_array':'new'if struct.unpack('<I',mu.mem_read(OBJ+0xc,4))[0]==NEW else'old'if struct.unpack('<I',mu.mem_read(OBJ+0xc,4))[0]==OLD else'null','calls':calls}
 assert rc==(-50 if null_trust else 0)
 if null_trust:assert calls==[] and row['trust_result']==4 and row['stored_array']==('old'if old else'null')
 else:
  assert row['anchors_only'] is supplied and row['trust_result']==0 and row['stored_array']==('new'if supplied else'null')
  assert calls==([{'retain':'new'}]if supplied else[])+([{'release':'old'}]if old else[])
 return row

def run_weak(algorithm,block_size,key_present=True):
 calls=[]
 def callback(mc,pc,size,cookie):
  if pc==PINS['_SecCertificateCopyPublicKey']:assert mc.reg_read(UC_ARM_REG_R0)==CERT;out=KEY if key_present else 0;calls.append('copy_public_key')
  elif pc==PINS['_SecKeyGetBlockSize']:assert mc.reg_read(UC_ARM_REG_R0)==KEY;out=block_size;calls.append('block_size')
  elif pc==PINS['_SecKeyGetAlgorithmIdentifier']:assert mc.reg_read(UC_ARM_REG_R0)==KEY;out=algorithm;calls.append('algorithm_id')
  elif pc==PINS['_CFRelease']:assert mc.reg_read(UC_ARM_REG_R0)==KEY;out=0;calls.append('release_public_key')
  else:return
  mc.reg_write(UC_ARM_REG_R0,out);mc.reg_write(UC_ARM_REG_PC,mc.reg_read(UC_ARM_REG_LR))
 mu=context(callback);mu.reg_write(UC_ARM_REG_R0,CERT);mu.emu_start(PINS['_SecCertificateIsWeak']|1,END,count=250);assert mu.reg_read(UC_ARM_REG_PC)==END
 weak=bool(mu.reg_read(UC_ARM_REG_R0));expected=not key_present or algorithm not in(1,3)or block_size<(128 if algorithm==1 else 20)
 assert weak is expected
 assert calls==(['copy_public_key','block_size','algorithm_id','release_public_key']if key_present else['copy_public_key'])
 return {'algorithm_id':algorithm,'block_size':block_size,'key_present':key_present,'weak':weak,'calls':calls}
anchor=[run_anchor(),run_anchor(anchor_count=0),run_anchor(supplied=False),run_anchor(old=False),run_anchor(null_trust=True)]
weak=[run_weak(a,size)for a,sizes in[(1,[0,127,128,129,256]),(3,[0,19,20,21,32]),(0,[128]),(2,[128])]for size in sizes]+[run_weak(1,128,False)]
print(json.dumps({'cache_sha256':h.hexdigest(),'scope':'original API leaves; synthetic CF/public-key providers, no cert trust or parsing','function_pins':{k:hex(v)for k,v in PINS.items()},'anchor_cases':anchor,'weak_cases':weak},indent=2))
