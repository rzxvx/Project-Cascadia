#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Bounded actual 13G36 constructor, ADC getter dispatch and completion callback.
Allocation/context initialization and gate wakeup are explicit mocks. No I/O.
"""
import argparse, hashlib, json, struct
from pathlib import Path
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC
SHA='8eae16f060a71d4f3790385951588f8198d0f21cdb74146f9750941a3d764125'
OBJ=0x20001000; GATE=0x20003000; GVT=0x20004000; OUT=0x20005000; END=0x30000000; WAKE=END+4
R=(UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3)
class Dispatch:
 def __init__(self,data):
  assert hashlib.sha256(data).hexdigest()==SHA
  self.u=Uc(UC_ARCH_ARM,UC_MODE_THUMB);self.calls=[];self.arrivals=[];self.stop_adc=False
  o=28
  for _ in range(struct.unpack_from('<I',data,16)[0]):
   c,n=struct.unpack_from('<II',data,o)
   if c==1:
    va,vs,fo,fs=struct.unpack_from('<IIII',data,o+24)
    if vs:self.u.mem_map(va,(vs+4095)&~4095)
    if fs:self.u.mem_write(va,data[fo:fo+fs])
   o+=n
  self.u.mem_map(0x20000000,0x20000);self.u.mem_map(END,0x1000)
  self.u.hook_add(UC_HOOK_CODE,self.hook)
 def w(self,a,v):self.u.mem_write(a,struct.pack('<I',v))
 def r(self,a):return struct.unpack('<I',self.u.mem_read(a,4))[0]
 def hook(self,u,a,size,user):
  args=[u.reg_read(r)for r in R]
  if a in (0x8114be9c,0x8114bd2c,0x8114bf2c,WAKE):
   names={0x8114be9c:'constructor allocation',0x8114bd2c:'base/context initialization',0x8114bf2c:'constructor context cleanup',WAKE:'gate commandWakeup'}
   self.calls.append({'mocked':names[a],'arguments':args})
   u.reg_write(UC_ARM_REG_R0,OBJ if a==0x8114be9c else 0);u.reg_write(UC_ARM_REG_PC,u.reg_read(UC_ARM_REG_LR))
  elif a==0x811472ac and self.stop_adc:
   self.arrivals.append({'entry':hex(a),'arguments':args,'vptr':hex(self.r(OBJ))});u.emu_stop()
 def call(self,entry,args):
  for r,v in zip(R,args):self.u.reg_write(r,v)
  self.u.reg_write(UC_ARM_REG_SP,0x2001f000);self.u.reg_write(UC_ARM_REG_LR,END|1)
  self.u.emu_start(entry|1,END,count=10000)
  return self.u.reg_read(UC_ARM_REG_R0)
 def run(self):
  assert self.call(0x81144ecc,(0,0,0,0))==OBJ
  vp=self.r(OBJ);assert vp==0x8114d214 and self.r(vp+0x388)==0x811472ad
  self.stop_adc=True
  self.call(0x81147d58,(OBJ,6,OUT,0));assert self.arrivals[-1]['arguments'][:3]==[OBJ,6,0]
  assert self.u.reg_read(UC_ARM_REG_PC)==0x811472ac
  self.stop_adc=False
  stubs=[]
  for slot,result in ((0x38c,0),(0x390,0),(0x394,0xe00002c7),(0x398,1)):
   target=self.r(vp+slot);actual=self.call(target,(OBJ,6,OUT,0));assert actual==result
   stubs.append({'slot':hex(slot),'target':hex(target),'return':actual})
  self.w(OBJ+0x60,GATE);self.w(GATE,GVT);self.w(GVT+0x80,WAKE|1)
  callbacks=[]
  for state in (0,1,2,3):
   self.w(OBJ+0x68,state);count=len(self.calls)
   assert self.call(0x81141180,(OBJ,0,0,0))==0
   after=self.r(OBJ+0x68);wakes=self.calls[count:]
   assert after==(2 if state==1 else state) and len(wakes)==(1 if state==1 else 0)
   if wakes:assert wakes[0]['arguments'][:3]==[GATE,OBJ+0x68,0]
   callbacks.append({'before_state':state,'after_state':after,'wakeup':wakes})
  return {'format':'p105-actual-ADC-dispatch13-v1','kernelcache_sha256':SHA,'hardware_access':False,'physical_quiescence_granted':False,'actual_constructor_vptr':hex(vp),'bounded_getter_arrival':self.arrivals,'generic_slots_actual_stubs':stubs,'completion_callbacks':callbacks,'mocks':self.calls[:3],'limits':['constructor allocation/base init/context helper calls mocked','getter stopped at actual ADC entry; conversion not executed','callback gate wake mocked; no event origin/epoch or electrical completion tested','base generic acquisition not assumed active hardware path']}
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('kernelcache',type=Path);a=p.parse_args();print(json.dumps(Dispatch(a.kernelcache.read_bytes()).run(),indent=2))
