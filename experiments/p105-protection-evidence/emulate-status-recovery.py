"""Execute actual12H status consumer WITH actual recovery; all bus/OS inputs fake."""
import importlib.util,os
from pathlib import Path
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0,UC_ARM_REG_LR,UC_ARM_REG_PC
ROOT=Path(__file__).resolve().parents[2]
s=importlib.util.spec_from_file_location('status',ROOT/'tools/power/emulate-charge-status.py');b=importlib.util.module_from_spec(s);s.loader.exec_module(b)
class Full(b.Status):
 def __init__(self,data,fail_transfer=None):
  super().__init__(data,fail_transfer);self.mu.hook_add(UC_HOOK_CODE,self.notify,begin=0x80ed1f5c,end=0x80ed1f5c);self.registers[0x41]=0xaa
 def helper(self,mu,address,size,user):
  if address in (0x80ed0c94,0x80ed0ce0):self.calls.append({'actual_recovery_entry':hex(address)});return
  super().helper(mu,address,size,user)
 def notify(self,mu,address,size,user):
  self.calls.append({'mocked':'post-mode generic notification; no physical meaning inferred'});mu.reg_write(UC_ARM_REG_R0,0);mu.reg_write(UC_ARM_REG_PC,mu.reg_read(UC_ARM_REG_LR))
 def full(self,status,fail=None):
  r=self.consume(status,401,first_timeout=100,attached_at=1);r['final41']=self.registers[0x41];r['final1b1c']=list(self.registers[0x1b:0x1d]);return r
if __name__=='__main__':
 import json
 data=b.events.load_input(ROOT/'build/firmware/kernelcache.12H321.macho');o={}
 for status in (8,32):
  for fail in (None,1,2,3):o[f'{status:02x}-fail{fail}']=Full(data,fail).full(status)
 print(json.dumps(o,indent=2))
