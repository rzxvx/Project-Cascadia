#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""New actual IRQ/event-source paths with fake provider and PMU memory.
No hardware accesses, positive quiescence producer or charge authorization.
"""
import argparse,importlib.util,json,struct,hashlib
from pathlib import Path
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3,UC_ARM_REG_LR,UC_ARM_REG_PC,UC_ARM_REG_SP

def load(root):
 spec=importlib.util.spec_from_file_location('adc',root/'tools/power/emulate-d1946-adc-lifecycle.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m

class Drain:
 def __init__(self,module,data,poison=False,fail=False,cached=False,provider_error=0):
  self.m=module;self.d=module.d;self.a=module.Acquisition(data,fail_transfer=0 if fail else None);a=self.a;d=self.d
  self.provider_error=provider_error;self.poison=poison;self.provider_calls=[];self.actions=[];self.injections=[]
  # Actual kernel event-source code, metadata/vtable and fake immutable frames.
  lo,hi=0x80304000,0x80306000;a.mu.mem_map(lo,hi-lo)
  for va,_,fo,fs in d.segments(data):
   b,e=max(va,lo),min(va+fs,hi)
   if b<e:a.mu.mem_write(b,data[fo+b-va:fo+e-va])
  self.E=d.SOURCE+0xc000;self.P=d.SOURCE+0xc100;self.PVT=d.SOURCE+0xc200;self.EVT=d.SOURCE+0xc300;self.EX=d.SOURCE+0xc500;self.DIS=d.TRAPS+0x200;self.EN=d.TRAPS+0x204
  a.w32(self.E,self.EVT);a.w32(self.E+0xc,d.PMU);a.w32(self.E+0x10,0x80ecbca5);a.w32(self.E+0x24,self.P);a.w32(self.E+0x28,0);a.w32(self.E+0x2c,1);a.w32(self.E+0x30,0);a.w32(self.E+0x38,self.EX);a.w32(self.EX,0)
  a.mu.mem_write(self.E+0x14,b'\x01');a.mu.mem_write(self.E+0x34,b'\x01\x00');a.w32(self.P,self.PVT);a.w32(self.PVT+0x214,self.EN|1);a.w32(self.PVT+0x218,self.DIS|1);a.w32(self.EVT+0x54,0x803052cd)
  a.w32(d.PMU+0x94,1);a.registers[1:10]=bytes(9)
  if cached:a.mu.mem_write(d.PMU+0x9a,b'\x01');a.mu.mem_write(d.PMU+0x9c,bytes.fromhex('002000000000000000'))
  a.mu.hook_add(UC_HOOK_CODE,self.hook,begin=0x80ecbca4,end=0x80ecbca4)
  a.mu.hook_add(UC_HOOK_CODE,self.hook,begin=self.DIS,end=self.EN)
 def hook(self,u,address,size,user):
  a=self.a;d=self.d
  if address==0x80ecbca4:
   self.actions.append({'action':'actual PMU event dispatcher','before_state':a.state()})
   if self.poison:
    # Actual prologue reserves0x3c bytes; eventbuf is currentSP-0x30.
    buf=u.reg_read(UC_ARM_REG_SP)-0x30;u.mem_write(buf,bytes.fromhex('002000000000000000'));self.injections.append({'mocked':'stale preexisting IRQ stack event buffer','address':hex(buf)})
  elif address in (self.DIS,self.EN):
   args=[u.reg_read(r)for r in (UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2)]
   assert args[0]==self.P and args[1]==0
   self.provider_calls.append({'mocked':'provider disableInterrupt' if address==self.DIS else 'provider enableInterrupt','arguments':args,'synthetic_return':self.provider_error})
   u.reg_write(UC_ARM_REG_R0,self.provider_error);u.reg_write(UC_ARM_REG_PC,u.reg_read(UC_ARM_REG_LR))
 def invoke(self,address,args):return self.a.invoke(address,args)
 def word(self,offset):return struct.unpack('<I',self.a.mu.mem_read(self.E+offset,4))[0]
 def run(self,disable=True):
  a=self.a;d=self.d
  if disable:self.invoke(0x803052f4,(self.E,0,0,0))
  before={'producer':self.word(0x2c),'consumer':self.word(0x30),'enabled':a.mu.mem_read(self.E+0x14,1)[0]}
  ret=self.invoke(0x80305398,(self.E,0,0,0))
  return {'return_code':ret,'before':before,'after':{'producer':self.word(0x2c),'consumer':self.word(0x30),'enabled':a.mu.mem_read(self.E+0x14,1)[0]},'ADC_state':a.state(),'actions':self.actions,'provider_calls':self.provider_calls,'transactions':a.transactions,'gate_calls':a.calls,'injections':self.injections,'hardware_access':False,'physical_quiescence_granted':False}

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('source_root',type=Path);p.add_argument('kernelcache',type=Path);a=p.parse_args();m=load(a.source_root);data=m.events.load_input(a.kernelcache)
 cases={name:Drain(m,data,**kw).run(disable=disable)for name,kw,disable in [('disabled_queued_event',{},True),('failed_read_stale_buffer',{'poison':True,'fail':True},True),('cached_resume_completion',{'cached':True},True),('auto_reenable_without_explicit_disable',{},False),('provider_disable_error_ignored',{'provider_error':0xe00002c0},True)]}
 assert cases['disabled_queued_event']['before']['enabled']==0 and len(cases['disabled_queued_event']['actions'])==1 and cases['disabled_queued_event']['after']['consumer']==1
 assert cases['failed_read_stale_buffer']['ADC_state']==2 and 'error' in cases['failed_read_stale_buffer']['transactions'][0]
 assert cases['cached_resume_completion']['ADC_state']==2 and cases['cached_resume_completion']['transactions'][0]['bytes']=='000000000000000000'
 assert cases['provider_disable_error_ignored']['before']['enabled']==0 and cases['provider_disable_error_ignored']['provider_calls'][0]['synthetic_return']==0xe00002c0
 assert cases['auto_reenable_without_explicit_disable']['provider_calls'][0]['mocked']=='provider enableInterrupt'
 print(json.dumps({'format':'p105-actual-IRQ-drain-boundaries-v1','kernelcache_sha256':m.d.EXPECTED_SHA256,'hardware_access':False,'premises':['provider disable/enable interrupts mocked; no electrical line modeled','command gates/PMU byte reads synthetic','one queued software event before disable','failed read leaves deliberately stale stack bytes in selected counterexample','cached event flag+bitmap set explicitly in resume counterexample'],'cases':cases},indent=2))
if __name__=='__main__':main()
