#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Actual PMU getProvider/IOService/GPIO IRQ type plus registration prefix.
Device-tree controller resolution and OSData bytes are explicit fake objects.
"""
import argparse,hashlib,importlib.util,json,struct
from pathlib import Path
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3,UC_ARM_REG_R4,UC_ARM_REG_R6,UC_ARM_REG_PC,UC_ARM_REG_SP,UC_ARM_REG_LR

def run(data):
 spec=importlib.util.spec_from_file_location('gpio',Path(__file__).with_name('gpio-fixture.py'));m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);g=m.GPIO(data)
 # Full initialized native vptr data, common-core methods and generation cells.
 segments=[];o=28
 for _ in range(struct.unpack_from('<I',data,16)[0]):
  c,n=struct.unpack_from('<II',data,o)
  if c==1:segments.append(struct.unpack_from('<IIII',data,o+24))
  o+=n
 def copy(lo,size):
  g.u.mem_map(lo,size)
  for v,vs,f,fs in segments:
   b,e=max(v,lo),min(v+fs,lo+size)
   if b<e:g.u.mem_write(b,data[f+b-v:f+e-v])
 copy(0x802ed000,0x1000);copy(0x802f3000,0x1000);copy(0x8031d000,0x2000);copy(0x803f4000,0x2000)
 # Old fixture already maps GPIO text80dec000..80df6000 including native vptr.
 SELF=m.OBJ+0x8000;NVT=m.OBJ+0x8100;RESOLVE=m.TRAP+0x300;VECTORS=m.OBJ+0x9000;GEN=0x803f5d90
 global_gen=struct.unpack('<I',g.u.mem_read(GEN,4))[0]
 g.w32(SELF+0x18,m.NUB);g.w32(SELF+0x1c,global_gen);g.w32(m.NUB,NVT);g.w32(NVT+0x288,RESOLVE|1);g.w32(m.OBJ+0x50,VECTORS)
 calls=[];prefix={}
 def hook(u,address,size,user):
  if address==RESOLVE:
   args=[u.reg_read(r)for r in (UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3)]
   assert args[:3]==[m.NUB,0,1]
   u.mem_write(args[3],struct.pack('<I',m.OBJ));calls.append({'mocked':'IOService.resolveInterrupt','source':0,'controller':m.OBJ,'binding':'pinned PMU interrupt-parent phandle0f => GPIO'})
   u.reg_write(UC_ARM_REG_R0,0);u.reg_write(UC_ARM_REG_PC,u.reg_read(UC_ARM_REG_LR))
  elif address==0x8031dfba:
   prefix.update(vector_number=u.reg_read(UC_ARM_REG_R4),vector_pointer=u.reg_read(UC_ARM_REG_R6));u.emu_stop()
 g.u.hook_add(UC_HOOK_CODE,hook,begin=RESOLVE,end=RESOLVE);g.u.hook_add(UC_HOOK_CODE,hook,begin=0x8031dfba,end=0x8031dfba)
 g.call(0x802ed368,(SELF,0,0,0));assert g.u.reg_read(UC_ARM_REG_R0)==m.NUB
 g.call(0x802f34b4,(m.NUB,0,m.OUT,0));kind=struct.unpack('<I',g.u.mem_read(m.OUT,4))[0];assert kind==1
 # Direct full native register prefix stops before lock/share/init side effects.
 for reg,val in zip((UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3),(m.OBJ,m.NUB,0,0)):g.u.reg_write(reg,val)
 g.u.reg_write(UC_ARM_REG_SP,m.STACK+0xff00);g.u.reg_write(UC_ARM_REG_LR,m.END|1);g.u.emu_start(0x8031df89,m.END,count=1000)
 assert g.u.reg_read(UC_ARM_REG_PC)==0x8031dfba and prefix=={'vector_number':19,'vector_pointer':VECTORS+19*32}
 return {'hardware_access':False,'physical_quiescence_granted':False,'raw_specifier_hex':'1300000001000000','actual_get_provider_result':m.NUB,'IOService_type':kind,'GPIO_type_method':'0x80ded634','GPIO_virtual_slot':'0x348','actual_register_prefix':prefix,'resolver_calls':calls,'complete_registration_executed':False,'limits':['provider relation supplied in coherent registry cache fixture','DT controller resolver mocked from exact pinned phandle match','native register prefix only; no interrupt enable/disable or physical line tested']}

if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('kernelcache',type=Path);a=p.parse_args();d=a.kernelcache.read_bytes();assert hashlib.sha256(d).hexdigest()=='db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c';print(json.dumps(run(d),indent=2))
