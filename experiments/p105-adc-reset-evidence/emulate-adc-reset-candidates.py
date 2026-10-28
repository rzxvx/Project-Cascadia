#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Bounded native ADC config/alternate-engine paths in fake RAM only."""
import argparse,hashlib,json,struct
from pathlib import Path
from unicorn import Uc,UC_ARCH_ARM,UC_MODE_THUMB,UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3,UC_ARM_REG_PC,UC_ARM_REG_SP,UC_ARM_REG_LR
PINS={'12':{'sha':'db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c','alternate':0x80ecdc24,'delay':0x80ed1d6c},'13':{'sha':'8eae16f060a71d4f3790385951588f8198d0f21cdb74146f9750941a3d764125','alternate':0x81147508,'delay':0x8114bbfc}}
OBJ,VT,GATE,GVT,OUT,STACK,TRAP,END=0x20001000,0x20002000,0x20003000,0x20004000,0x20005000,0x20010000,0x30000000,0x30000ffc
READ,WRITE,SLEEP=TRAP,TRAP+4,TRAP+8
REGS=(UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3)
class Candidate:
 def __init__(self,build,data,fail=None,busy=False):
  self.info=PINS[build];self.build=build
  if hashlib.sha256(data).hexdigest()!=self.info['sha']:raise ValueError('pinned image mismatch')
  self.u=Uc(UC_ARCH_ARM,UC_MODE_THUMB);self.transactions=[];self.helpers=[];self.fail=fail;self.busy=busy;self.regs=bytearray(512);self.regs[0x15e:0x160]=bytes.fromhex('abcd');self.regs[0x43]=0xa5;self.used=False
  o=28
  for _ in range(struct.unpack_from('<I',data,16)[0]):
   c,n=struct.unpack_from('<II',data,o)
   if c==1:
    va,vs,fo,fs=struct.unpack_from('<IIII',data,o+24)
    if vs:self.u.mem_map(va,(vs+4095)&~4095)
    if fs:self.u.mem_write(va,data[fo:fo+fs])
   o+=n
  self.u.mem_map(0x20000000,0x20000);self.u.mem_map(TRAP,0x1000)
  for a,v in ((OBJ,VT),(VT+0x378,READ|1),(VT+0x37c,WRITE|1),(OBJ+0x60,GATE),(GATE,GVT),(GVT+0x7c,SLEEP|1),(OUT,0xdeadbeef)):self.w(a,v)
  self.u.hook_add(UC_HOOK_CODE,self.hook)
 def w(self,a,v):self.u.mem_write(a,struct.pack('<I',v))
 def word(self,a):return struct.unpack('<I',self.u.mem_read(a,4))[0]
 def hook(self,u,address,size,user):
  args=[u.reg_read(r)for r in REGS]
  if address in (READ,WRITE):
   owner,reg,buf,count=args
   if owner!=OBJ or not 0<=reg<512 or not 1<=count<=512-reg:raise RuntimeError('unexpected fake PMU transaction')
   if reg not in (0x43,0x15d,0x15e):raise RuntimeError('unexpected PMU candidate address')
   row={'direction':'read'if address==READ else'write','logical_register':hex(reg),'count':count}
   error=0xe00002c0 if len(self.transactions)==self.fail else 0
   if error:row['error']=error
   elif address==WRITE:
    b=bytes(u.mem_read(buf,count));self.regs[reg:reg+count]=b;row['bytes']=b.hex()
   else:
    if reg==0x15d:self.regs[reg]=0x86 if self.busy else 6
    b=bytes(self.regs[reg:reg+count]);u.mem_write(buf,b);row['bytes']=b.hex()
   self.transactions.append(row);u.reg_write(UC_ARM_REG_R0,error);u.reg_write(UC_ARM_REG_PC,u.reg_read(UC_ARM_REG_LR))
  elif address==self.info['delay']:
   self.helpers.append({'mocked':'delay helper','arguments':args});u.reg_write(UC_ARM_REG_R0,0);u.reg_write(UC_ARM_REG_PC,u.reg_read(UC_ARM_REG_LR))
  elif address==SLEEP:
   self.helpers.append({'mocked':'commandSleep - stop before indefinite wait','arguments':args});u.emu_stop()
 def call(self,entry,args):
  if self.used:raise RuntimeError('fresh context required')
  self.used=True
  for r,v in zip(REGS,args):self.u.reg_write(r,v)
  self.u.reg_write(UC_ARM_REG_SP,STACK+0xf000);self.u.reg_write(UC_ARM_REG_LR,END|1);self.u.emu_start(entry|1,END,count=10000)
  pc=self.u.reg_read(UC_ARM_REG_PC)
  if pc not in (END,SLEEP):raise RuntimeError('instruction budget exhausted')
  return {'return_code':self.u.reg_read(UC_ARM_REG_R0),'completed':pc==END,'transactions':self.transactions,'helpers':self.helpers,'output_word':self.word(OUT),'config_cache':self.u.mem_read(OBJ+0x98,1)[0],'fake43':self.regs[0x43],'fake15d':self.regs[0x15d],'physical_quiescence_granted':False,'hardware_access':False}
 def config(self,state):
  self.w(OBJ+0x94,state);self.u.mem_write(OBJ+0x98,b'\xa5');return self.call(0x80ecdcb8,(OBJ,0,0x20,0))
 def alternate(self):return self.call(self.info['alternate'],(OBJ,6,OUT,0))
def run(build,data):
 alt={n:Candidate(build,data,fail=f,busy=b).alternate()for n,f,b in [('complete',None,False),('start_error',0,False),('status_error',1,False),('sample_error',2,False),('busy_abort',None,True),('busy_abort_error',2,True)]}
 assert [x['logical_register']for x in alt['complete']['transactions']]==['0x15d','0x15d','0x15e']
 assert alt['complete']['return_code']==0 and alt['complete']['output_word']==0xcdab
 for n in ('start_error','status_error','sample_error'):assert alt[n]['return_code']==0xe00002c0 and alt[n]['output_word']==0xdeadbeef
 assert alt['busy_abort']['return_code']==alt['busy_abort_error']['return_code']!=0 and alt['busy_abort_error']['transactions'][-1]['error']==0xe00002c0
 configs={}
 if build=='12':
  configs={n:Candidate(build,data,fail=f).config(s)for n,s,f in [('idle',0,None),('sleep_state',3,None),('failed_config_write',0,0),('busy_wait',1,None)]}
  for n in ('idle','sleep_state','failed_config_write'):assert configs[n]['return_code']==0 and configs[n]['config_cache']==0x85
  assert configs['failed_config_write']['fake43']==0xa5 and configs['busy_wait']['transactions']==[] and not configs['busy_wait']['completed']
 return {'format':'p105-native-ADC-reset-candidates-v1','build':build,'sha256':PINS[build]['sha'],'hardware_access':False,'physical_quiescence_granted':False,'alternate':alt,'config_setter':configs,'limits':['15d/15e full logical native addresses; no bank remapping or register meaning inferred','all bus I/O/sample/status/delay/gate waits mocked','normal43 ADC engine not declared reset by alternate engine','config setter successful return can hide failed write','delay argument1 is not physical settle proof']}
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('build',choices=PINS);p.add_argument('image',type=Path);a=p.parse_args();print(json.dumps(run(a.build,a.image.read_bytes()),indent=2))
