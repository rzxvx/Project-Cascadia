#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Original THS abort/reset helpers: fake I2C/delay only, no hardware fence proof."""
from pathlib import Path
import argparse,hashlib,importlib.util,json,struct
from unicorn import Uc,UC_ARCH_ARM,UC_MODE_THUMB,UC_HOOK_CODE
from unicorn.arm_const import *
ap=argparse.ArgumentParser();ap.add_argument('kernelcache',type=Path);ap.add_argument('--source-root',type=Path,required=True);args=ap.parse_args()
sp=importlib.util.spec_from_file_location('m',args.source_root/'scripts/usb-phy-plan.py');m=importlib.util.module_from_spec(sp);sp.loader.exec_module(m)
K=args.kernelcache.read_bytes();SHA='8eae16f060a71d4f3790385951588f8198d0f21cdb74146f9750941a3d764125';assert hashlib.sha256(K).hexdigest()==SHA
OBJ,DEV,VT,STACK,TRAP,END=0x10000000,0x10001000,0x10002000,0x20000000,0x30000000,0x30000ffc
for slot,entry in((0x364,0x809eec60),(0x368,0x809eec80),(0x36c,0x809eecac)):
 assert struct.unpack_from('<I',K,m.address_offset(K,0x809f12cc+slot))[0]==entry|1

def run(entry,fail=-1):
 mu=Uc(UC_ARCH_ARM,UC_MODE_THUMB);base=0x809eb000;mu.mem_map(base,0xa000);off=m.address_offset(K,base);mu.mem_write(base,K[off:off+0xa000])
 for p,n in((OBJ,0x10000),(STACK,0x10000),(TRAP,0x1000)):mu.mem_map(p,n)
 def put(p,v):mu.mem_write(p,struct.pack('<I',v))
 def ptr(p):return struct.unpack('<I',mu.mem_read(p,4))[0]
 put(OBJ,0x809f12cc);put(OBJ+0x74,DEV);put(OBJ+0x84,32);put(DEV,VT);put(VT+0x40c,TRAP|1)
 writes=[];delays=[]
 def hook(mc,pc,size,_):
  r0,r1,r2,r3=[mc.reg_read(r)for r in(UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3)]
  if pc==TRAP:
   assert r0==DEV and r3==1
   error=-5 if len(writes)==fail else 0
   writes.append({'register':r1,'value':mc.mem_read(r2,1)[0],'error':error});out=error
  elif pc in(0x809ef59c,0x809ef5fc):
   delays.append({'boundary':hex(pc),'literal_argument':r0,'physical_time_unit':'not qualified here'});out=0
  else:return
  mc.reg_write(UC_ARM_REG_R0,out&0xffffffff);mc.reg_write(UC_ARM_REG_PC,mc.reg_read(UC_ARM_REG_LR))
 mu.hook_add(UC_HOOK_CODE,hook)
 for r,v in((UC_ARM_REG_R0,OBJ),(UC_ARM_REG_SP,STACK+0xff00),(UC_ARM_REG_LR,END|1)):mu.reg_write(r,v)
 mu.emu_start(entry|1,END,count=3000);assert mu.reg_read(UC_ARM_REG_PC)==END
 return {'entry':hex(entry),'fail_write':fail,'return':hex(mu.reg_read(UC_ARM_REG_R0)),'writes':writes,'delay_calls':delays,'pending_rx_after':ptr(OBJ+0x84),'no_source_read_or_idle_ack_observed':True}
rows=[run(0x809eec60),run(0x809eec60,0),run(0x809eec80),run(0x809eec80,0),run(0x809eecac),run(0x809eecac,0),run(0x809eecac,1)]
assert rows[0]['writes']==[{'register':0x41,'value':1,'error':0}]
assert rows[2]['writes']==[{'register':5,'value':4,'error':0}]and rows[2]['delay_calls'][0]['literal_argument']==20
assert rows[4]['writes']==[{'register':5,'value':8,'error':0},{'register':5,'value':0,'error':0}]and rows[4]['delay_calls'][0]['literal_argument']==500
assert all(r['pending_rx_after']==32 for r in rows)
assert [r['return']for r in rows]==['0x0','0xfffffffb','0x0','0xfffffffb','0x0','0xfffffffb','0xfffffffb']
print(json.dumps({'source_sha256':SHA,'hardware_access':False,'scope':'original write/delay helpers, I2C+delay fake. No physical FIFO drain/reset/late-reply fence proof','cases':rows},indent=2))
