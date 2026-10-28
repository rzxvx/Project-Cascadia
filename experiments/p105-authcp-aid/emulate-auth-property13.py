"""Bounded original sender/kernel property consumer; all IOKit interfaces fakeRAM."""
from pathlib import Path
import argparse
ap=argparse.ArgumentParser();ap.add_argument('firmware_directory',type=Path);ap.add_argument('--source-root',type=Path,required=True);args=ap.parse_args()
import struct,hashlib,json,importlib.util
from unicorn import Uc,UC_ARCH_ARM,UC_MODE_THUMB,UC_HOOK_CODE
from unicorn.arm_const import *
sp=importlib.util.spec_from_file_location('m',str(args.source_root/'scripts/usb-phy-plan.py'));m=importlib.util.module_from_spec(sp);sp.loader.exec_module(m)
U=(args.firmware_directory/'userland/extracted/iaptransportd').read_bytes();K=(args.firmware_directory/'kernelcache.13G36.macho').read_bytes()
assert hashlib.sha256(U).hexdigest()=='0eabef460b814484590b1337e7349ddeb37385d1fd80c83332dd562c3410ad1e'
assert hashlib.sha256(K).hexdigest()=='8eae16f060a71d4f3790385951588f8198d0f21cdb74146f9750941a3d764125'
OBJ,IFACE,VT,DICT,DVT,TRUE,FALSE,TASK,RAM=0x10000000,0x10001000,0x10002000,0x10003000,0x10004000,0x10005000,0x10005010,0x10006000,0x10007000
STACK,TRAP,END=0x20000000,0x30000000,0x30000ffc

# Source nlist, indirect table and nonlazy bindings checked before any fake loader.
pos=28;sections=[];segments=[]
for _ in range(struct.unpack_from('<I',U,16)[0]):
 cmd,size=struct.unpack_from('<II',U,pos)
 if cmd==2:so,n,st,ss=struct.unpack_from('<4I',U,pos+8)
 if cmd==11:indirect,count=struct.unpack_from('<2I',U,pos+56)
 if cmd==1:
  vm,vsz,fo,fs=struct.unpack_from('<4I',U,pos+24)
  if fs:segments.append((vm,vsz,fo,fs))
  for j in range(struct.unpack_from('<I',U,pos+48)[0]):
   q=pos+56+j*68;sections.append((U[q:q+16].split(b'\0')[0].decode(),*struct.unpack_from('<9I',U,q+32)))
 pos+=size
names={i:U[st+struct.unpack_from('<I',U,so+12*i)[0]:U.find(b'\0',st+struct.unpack_from('<I',U,so+12*i)[0])].decode()for i in range(n)}
bindings={};stubs={}
for name,a,n,o,_,_,_,flags,r1,r2 in sections:
 if flags&255==6:
  for j in range(n//4):bindings[a+4*j]=names.get(struct.unpack_from('<I',U,indirect+4*(r1+j))[0],'LOCAL')
 if flags&255==8:
  for j in range(n//r2):stubs[a+j*r2]=names.get(struct.unpack_from('<I',U,indirect+4*(r1+j))[0],'LOCAL')
assert bindings[0x284bc]=='_kCFBooleanTrue' and bindings[0x284b8]=='_kCFBooleanFalse'
assert stubs[0x27d0c]=='_IORegistryEntrySetCFProperty'
assert K[m.address_offset(K,0x809dd674+0xac):m.address_offset(K,0x809dd674+0xac)+4]==struct.pack('<I',0x809d9eb5)
assert m.symbol_address(K,'_kOSBooleanTrue')==0x803ec4ec and m.symbol_address(K,'_kOSBooleanFalse')==0x803ec4f0
assert m.pic_literal_pointer(K,0x809d9f66,0x809d9f6e,1)==0x809dbd05

def sender(entry,get_error=0,device=0x8877,set_error=0):
 mu=Uc(UC_ARCH_ARM,UC_MODE_THUMB)
 for vm,vs,off,fs in segments:
  assert not(vm&4095);mu.mem_map(vm,(vs+4095)&~4095);mu.mem_write(vm,U[off:off+fs])
 for p,n in((OBJ,0x10000),(STACK,0x10000),(TRAP,0x1000)):mu.mem_map(p,n)
 def put(a,v):mu.mem_write(a,struct.pack('<I',v))
 put(0x284bc,RAM);put(RAM,TRUE);put(0x284b8,RAM+4);put(RAM+4,FALSE)
 # Fake dynamic loader binds the actual guard import to synthetic RAM.
 for va,name in bindings.items():
  if name=='___stack_chk_guard':put(va,RAM+8);put(RAM+8,0x12345678)
 put(OBJ+0x20,IFACE);put(IFACE,VT);put(VT+0x54,TRAP|1)
 props=[];calls=[]
 def hook(mc,pc,size,_):
  r0,r1,r2,r3=[mc.reg_read(r)for r in(UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3)]
  if pc==TRAP:
   assert r0==IFACE;calls.append('GetDevice mock')
   if not get_error:mc.mem_write(r1,struct.pack('<I',device))
   out=get_error
  elif pc==0x27d0c:
   assert r0==device and r1==0x299d0 and r2 in(TRUE,FALSE)
   props.append({'mock':'IORegistryEntrySetCFProperty','device_handle':r0,'key':'Authenticated','value':r2==TRUE,'error':set_error});out=set_error
  elif pc==0x27d54:calls.append('GetDevice diagnostic only');out=0
  else:return
  mc.reg_write(UC_ARM_REG_R0,out&0xffffffff);mc.reg_write(UC_ARM_REG_PC,mc.reg_read(UC_ARM_REG_LR))
 mu.hook_add(UC_HOOK_CODE,hook)
 for r,v in((UC_ARM_REG_R0,OBJ),(UC_ARM_REG_SP,STACK+0xff00),(UC_ARM_REG_LR,END|1)):mu.reg_write(r,v)
 mu.emu_start(entry|1,END,count=3000);assert mu.reg_read(UC_ARM_REG_PC)==END
 return {'entry':hex(entry),'get_error':get_error,'get_device':device,'set_error':set_error,'properties':props,'calls':calls,'scope':'actual CFBoolean wrapper and direct property sender; no authentication executed, fake COM/device/property APIs'}

def consumer(value,previous=False,entitled=True,ent_error=0,task=True,dictionary=True):
 mu=Uc(UC_ARCH_ARM,UC_MODE_THUMB)
 lo,hi=0x809d8000,0x809e4000;a,b=[m.address_offset(K,z)for z in(lo,hi-1)];assert b-a==hi-lo-1
 mu.mem_map(lo,hi-lo);mu.mem_write(lo,K[a:b+1])
 for p,n in((OBJ,0x10000),(STACK,0x10000),(TRAP,0x1000),(0x803ec000,0x1000),(0x80412000,0x1000)):mu.mem_map(p,n)
 def put(a,v):mu.mem_write(a,struct.pack('<I',v))
 def ptr(a):return struct.unpack('<I',mu.mem_read(a,4))[0]
 # Native kOSBoolean globals carry one extra pointer indirection.
 put(0x803ec4ec,RAM);put(RAM,TRUE);put(0x803ec4f0,RAM+4);put(RAM+4,FALSE)
 put(OBJ,0x809dd674);put(DICT,DVT);put(DVT+0x98,TRAP|1)
 mu.mem_write(OBJ+0xd8,bytes([int(previous)]));calls=[]
 def hook(mc,pc,size,_):
  r0,r1,r2,r3=[mc.reg_read(r)for r in(UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3)]
  if pc==0x809db76c:out=0 # interrupt-context boundary fake
  elif pc==0x809db65c:out=DICT if dictionary and r0==DICT else 0 # dynamic metacast boundary
  elif pc==TRAP:
   assert r0==DICT
   off=m.address_offset(K,r1);key=K[off:K.find(b'\0',off)].decode();calls.append({'mock':'dictionary lookup','key':key})
   out={'true':TRUE,'false':FALSE,'foreign':RAM+0x100}.get(value,0)if key=='Authenticated'else 0
  elif pc==0x809db83c:out=TASK if task else 0
  elif pc==0x809db82c:assert r0==TASK;out=0
  elif pc==0x809db6cc:
   assert r0==TASK and r1==0x809dbd05
   calls.append({'mock':'entitlement lookup','key':'com.apple.usb.authentication','error':ent_error,'entitled':entitled})
   if not ent_error:mc.mem_write(r2,bytes([int(entitled)]))
   out=ent_error
  elif pc in(0x809d9c8c,0x809d9d88):calls.append({'mock':'authentication metadata/interface-property helper','address':hex(pc)});out=0
  elif pc==0x809d9b00: calls.append({'mock':'interface-publication boundary','cached_authenticated':int(mu.mem_read(OBJ+0xd8,1)[0])});out=0
  elif pc==0x809d9fb8:
   calls.append({'excluded':'denied entitlement diagnostics; no state transition'})
   mc.reg_write(UC_ARM_REG_PC,0x809da1df);return
  elif pc==0x809da038:
   # Only failure diagnostics follow until completion; explicit bounded cut,
   # not execution of revocation/termination or interface publication.
   calls.append({'excluded':'failure diagnostics after cached state became false'})
   mc.reg_write(UC_ARM_REG_PC,0x809da1df);return
  else:return
  mc.reg_write(UC_ARM_REG_R0,out&0xffffffff);mc.reg_write(UC_ARM_REG_PC,mc.reg_read(UC_ARM_REG_LR))
 mu.hook_add(UC_HOOK_CODE,hook)
 for r,v in((UC_ARM_REG_R0,OBJ),(UC_ARM_REG_R1,DICT),(UC_ARM_REG_SP,STACK+0xff00),(UC_ARM_REG_LR,END|1)):mu.reg_write(r,v)
 try:mu.emu_start(0x809d9eb5,END,count=3000)
 except Exception as error:raise RuntimeError(f'consumer{value}/ent{entitled}/{ent_error} PC={mu.reg_read(UC_ARM_REG_PC):#x},calls={calls}')from error
 assert mu.reg_read(UC_ARM_REG_PC)==END
 return {'input':value,'previous':previous,'entitled':entitled,'entitlement_error':ent_error,'task_present':task,'dictionary_type':dictionary,'final_cached_state':int(mu.mem_read(OBJ+0xd8,1)[0]),'return':hex(mu.reg_read(UC_ARM_REG_R0)),'calls':calls,'scope':'actual property identity/entitlement/changed-state branches; metadata+publication callbacks fake; false diagnostics cut'}

ss=[sender(0x13cfc),sender(0x13d10),sender(0x13cfc,get_error=-5),sender(0x13cfc,device=0),sender(0x13cfc,set_error=-5)]
cs=[consumer('true'),consumer('false',previous=True),consumer('true',previous=True),consumer('true',entitled=False),consumer('true',ent_error=-5),consumer('true',task=False),consumer('foreign'),consumer('missing'),consumer('true',dictionary=False)]
assert [e['properties'][0]['value']for e in ss[:2]]==[True,False]
assert not ss[2]['properties'] and not ss[3]['properties']
assert [e['final_cached_state']for e in cs]==[1,0,1,0,0,0,0,0,0]
assert sum(any('interface-publication boundary'==c.get('mock')for c in e['calls'])for e in cs)==1
print(json.dumps({'sources':{'iaptransportd_sha256':hashlib.sha256(U).hexdigest(),'kernelcache_sha256':hashlib.sha256(K).hexdigest()},'senders':ss,'consumers':cs,'no_hardware':True,'no_signature_verification':True,'no_USBd_0x20_bridge_established':True,'no_Camera_identity_or_power_grant':True},indent=2))
