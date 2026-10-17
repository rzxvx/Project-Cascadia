"""Original THS size/submission/completion methods; synthetic I2C callbacks only."""
import argparse,hashlib,importlib.util,json,struct
from pathlib import Path
from unicorn import Uc,UC_ARCH_ARM,UC_MODE_THUMB,UC_HOOK_CODE
from unicorn.arm_const import *
ap=argparse.ArgumentParser();ap.add_argument('kernelcache',type=Path);ap.add_argument('--source-root',type=Path,required=True);args=ap.parse_args()
sp=importlib.util.spec_from_file_location('m',args.source_root/'scripts/usb-phy-plan.py');m=importlib.util.module_from_spec(sp);sp.loader.exec_module(m)
K=args.kernelcache.read_bytes();SHA='8eae16f060a71d4f3790385951588f8198d0f21cdb74146f9750941a3d764125';assert hashlib.sha256(K).hexdigest()==SHA
OBJ,DEV,VT,TX,RAM,STACK,TRAP,END=0x10000000,0x10001000,0x10002000,0x10003000,0x10004000,0x20000000,0x30000000,0x30000ffc
assert struct.unpack_from('<I',K,m.address_offset(K,0x809f12cc+0x348))[0]==0x809eebd9
assert struct.unpack_from('<I',K,m.address_offset(K,0x809f12cc+0x370))[0]==0x809eeabd

def execute(entry,txlen=21,rxlen=3,fail_write=-1,status=0x10,fail_read=-1,direction=1):
 mu=Uc(UC_ARCH_ARM,UC_MODE_THUMB);base=0x809eb000;mu.mem_map(base,0xa000);o=m.address_offset(K,base);mu.mem_write(base,K[o:o+0xa000])
 for p,n in((OBJ,0x10000),(STACK,0x10000),(TRAP,0x1000)):mu.mem_map(p,n)
 def put(a,v):mu.mem_write(a,struct.pack('<I',v&0xffffffff))
 def ptr(a):return struct.unpack('<I',mu.mem_read(a,4))[0]
 put(OBJ,0x809f12cc);put(OBJ+0x74,DEV);put(DEV,VT);put(VT+0x40c,TRAP|1);put(VT+0x410,(TRAP+4)|1)
 put(OBJ+0x84,rxlen if entry==0x809eeabc else 0x55)
 mu.mem_write(TX,bytes(range(64)));transfers=[];events=[]
 # Native MOVW/MOVT+ADD stackguard import, synthetic loader double-indirection.
 put(0x809f00f4,RAM);put(RAM,0x12345678)
 def hook(mc,pc,size,_):
  r0,r1,r2,r3=[mc.reg_read(r)for r in(UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3)]
  if pc==TRAP:
   assert r0==DEV and 1<=r3<=32
   err=-5 if len(transfers)==fail_write else 0
   transfers.append({'operation':'I2C write fake','register':r1,'bytes':bytes(mu.mem_read(r2,r3)).hex(),'error':err});out=err
  elif pc==TRAP+4:
   assert r0==DEV and 1<=r3<=32
   err=-5 if len(transfers)==fail_read else 0
   val=bytes([status])if r1==0x0d else bytes(range(r3))
   if err:val=b'\xcc'*r3 # poisoned failed read, explicitly synthetic
   mu.mem_write(r2,val);transfers.append({'operation':'I2C read fake','register':r1,'bytes':val.hex(),'error':err});out=err
  elif pc==0x809ec0d4:
   assert r0==OBJ;events.append({'boundary':'IDbus event callback fake','code':r1,'payload':bytes(mu.mem_read(r2,rxlen)).hex()if r2 else None});out=0
  elif pc==0x809ec390:events.append({'boundary':'secondary event handler fake'});out=0
  elif pc==0x809ef7dc:raise AssertionError('stack guard failure')
  else:return
  mc.reg_write(UC_ARM_REG_R0,out&0xffffffff);mc.reg_write(UC_ARM_REG_PC,mc.reg_read(UC_ARM_REG_LR))
 mu.hook_add(UC_HOOK_CODE,hook)
 for reg,val in((UC_ARM_REG_R0,OBJ),(UC_ARM_REG_R1,direction if entry==0x809eebd8 else TX),(UC_ARM_REG_R2,txlen),(UC_ARM_REG_R3,rxlen),(UC_ARM_REG_SP,STACK+0xff00),(UC_ARM_REG_LR,END|1)):mu.reg_write(reg,val)
 try:mu.emu_start(entry|1,END,count=3000)
 except Exception as e:raise RuntimeError(f'entry{entry:#x} len{txlen}/{rxlen} PC{mu.reg_read(UC_ARM_REG_PC):#x} transfers{transfers}')from e
 assert mu.reg_read(UC_ARM_REG_PC)==END
 return {'entry':hex(entry),'txlen':txlen,'rxlen':rxlen,'direction':direction,'fail_write':fail_write,'status':status,'fail_read':fail_read,'return':hex(mu.reg_read(UC_ARM_REG_R0)),'pending_rx':ptr(OBJ+0x84),'transfers':transfers,'events':events}
size=[execute(0x809eebd8,direction=d)for d in(0,1,2,7)]
submit=[execute(0x809eebdc,t,r)for t,r in((1,1),(21,3),(32,32),(5,32))]
submit +=[execute(0x809eebdc,t,r)for t,r in((0,1),(33,1),(1,0),(1,33))]
submit +=[execute(0x809eebdc,fail_write=n)for n in range(3)]
complete=[execute(0x809eeabc,rxlen=n)for n in(1,3,32)]
complete +=[execute(0x809eeabc,status=0),execute(0x809eeabc,rxlen=3,fail_read=0),execute(0x809eeabc,rxlen=3,fail_read=1)]
assert all(r['return']=='0x20'and not r['transfers']for r in size)
for r in submit[:4]:
 assert r['return']=='0x0' and r['pending_rx']==r['rxlen']
 assert [t['register']for t in r['transfers']]==[0x20,0x41,0x40]
 assert [t['bytes']for t in r['transfers']]==[bytes(range(r['txlen'])).hex(),bytes([2*(r['rxlen']-1)]).hex(),bytes([2*r['txlen']-1]).hex()]
for r in submit[4:8]:assert r['return']=='0xe00002c2'and not r['transfers']and r['pending_rx']==0x55
for i,r in enumerate(submit[8:]):assert r['return']=='0xfffffffb' and len(r['transfers'])==i+1 and r['pending_rx']==3
for r in complete[:3]:
 assert [t['register']for t in r['transfers']]==[0x0d,0x40-r['rxlen']]
 assert r['pending_rx']==0 and r['events'][0]['payload']==bytes(reversed(range(r['rxlen']))).hex()
assert not complete[3]['events']and complete[3]['pending_rx']==3
assert not complete[4]['events']and complete[4]['pending_rx']==3
# Diagnostic native gap: bulk read error is not checked before callback publication.
assert complete[5]['events'][0]['payload']=='cccccc'and complete[5]['pending_rx']==0
print(json.dumps({'source_sha256':SHA,'no_hardware':True,'scope':'actual THS capability/submission/status-read/copy branches; fake I2C/event APIs. No IRQ ownership or electrical validation','size_cases':size,'submit_cases':submit,'complete_cases':complete},indent=2))
