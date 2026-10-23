"""Original13G36 source classification, original routing/scaling; fake I/O only."""
import argparse,hashlib,json,struct
from pathlib import Path
from unicorn import Uc,UC_ARCH_ARM,UC_MODE_THUMB,UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3,UC_ARM_REG_SP,UC_ARM_REG_LR,UC_ARM_REG_PC
SHA='7dc177231f52e7032cd10ddb255c3a8fd6f62c33c5e4d731eda7e5e0a8b8fdcd'
B,S,E=0x9ff00000,0x20000000,0x30000ffc
class Source:
 def __init__(self,data):
  if len(data)!=282624 or hashlib.sha256(data).hexdigest()!=SHA:raise ValueError('wrong pinned iBoot13')
  self.m=Uc(UC_ARCH_ARM,UC_MODE_THUMB);self.m.mem_map(B,0x70000);self.m.mem_write(B,data);self.m.mem_map(S,0x10000);self.m.mem_map(0x30000000,0x1000)
  self.transactions=[];self.samples=[];self.delays=[];self.routing=[];self.logs=[];self.read_errors=set();self.write_errors=set();self.adc_errors=set();self.pmubytes=bytes(6);self.tristar={12:0x80,6:4};self.raw_samples=[];self.readordinal=self.writeordinal=self.adcordinal=0
  for off in (0xc3c4,0xc4e8,0xc88c,0x1cae0,0x1c88c,0x33540,0x1be40,0x17678):self.m.hook_add(UC_HOOK_CODE,self.io,begin=B+off,end=B+off)
 def io(self,m,addr,size,_):
  args=[m.reg_read(r) for r in (UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3)];off=addr-B;ret=0
  if off==0x1be40:raise RuntimeError('original stack guard')
  if off==0x17678:self.routing.append(args[0]);return # original routing instructions execute
  if off==0xc3c4:
   self.readordinal+=1;out,n,flag=struct.unpack('<III',m.mem_read(m.reg_read(UC_ARM_REG_SP),12));reg=m.mem_read(args[2],1)[0]
   if args[:2]==[0,0x79] and reg==10 and n==6:value=self.pmubytes
   elif args[:2]==[0,0x35] and n==1:value=bytes([self.tristar.get(reg,0)])
   else:raise RuntimeError(('unreviewed read',args,reg,n))
   ret=-5 if self.readordinal in self.read_errors else 0
   if not ret:m.mem_write(out,value)
   self.transactions.append({'op':'read','address':args[1],'reg':reg,'length':n,'result':ret,'ordinal':self.readordinal,'written':not bool(ret)})
  elif off==0xc4e8:
   self.writeordinal+=1
   if args[:2]!=[0,0x34] or args[3]!=2:raise RuntimeError(('unreviewed write',args))
   reg,val=bytes(m.mem_read(args[2],2));ret=-5 if self.writeordinal in self.write_errors else 0
   self.transactions.append({'op':'write','address':args[1],'reg':reg,'value':val,'result':ret,'ordinal':self.writeordinal})
  elif off==0xc88c:
   self.adcordinal+=1
   if args[0]!=6 or not self.raw_samples:raise RuntimeError(('unreviewed ADC request',args,self.routing,self.samples))
   raw=self.raw_samples.pop(0);ret=-1 if self.adcordinal in self.adc_errors else 0
   if not ret:m.mem_write(args[1],struct.pack('<I',raw))
   self.samples.append({'channel':6,'encoded_sample':raw,'result':ret,'output_written':not bool(ret)})
  elif off in (0x1cae0,0x1c88c):self.delays.append({'entry':hex(addr),'arg0':args[0],'arg1':args[1]})
  elif off==0x33540:self.logs.append(args[:2])
  else:raise RuntimeError('unknown mock')
  m.reg_write(UC_ARM_REG_R0,ret & 0xffffffff);m.reg_write(UC_ARM_REG_PC,m.reg_read(UC_ARM_REG_LR))
 def run(self,status=8,raw_samples=(1802,2457),read_errors=(),write_errors=(),adc_errors=(),tristar=None):
  if type(status)is not int or not 0<=status<=255:raise ValueError('bad raw status')
  if len(raw_samples)>3 or any(type(x)is not int or not 0<=x<=4095 for x in raw_samples):raise ValueError('bad raw ADC fixture')
  if any(type(x)is not int or not 1<=x<=100 for xs in (read_errors,write_errors,adc_errors) for x in xs):raise ValueError('bad ordinal')
  self.transactions=[];self.samples=[];self.routing=[];self.logs=[];self.delays=[];self.readordinal=self.writeordinal=self.adcordinal=0
  self.pmubytes=bytes([status,0,0,0,0,0]);self.raw_samples=list(raw_samples);self.read_errors=set(read_errors);self.write_errors=set(write_errors);self.adc_errors=set(adc_errors)
  if tristar is not None:self.tristar=tristar
  self.m.mem_write(S,bytes(0x10000));self.m.reg_write(UC_ARM_REG_SP,S+0xf000);self.m.reg_write(UC_ARM_REG_LR,E|1)
  self.m.emu_start(B+0x20f3c|1,E,count=20000)
  if self.m.reg_read(UC_ARM_REG_PC)!=E:raise RuntimeError('instruction bound')
  source,id_=struct.unpack('<II',self.m.mem_read(B+0x4818c,8));cache=self.m.mem_read(B+0x46a9c,3)
  return {'source_type':source,'brick_id':id_,'routing':self.routing,'samples':self.samples,'transactions':self.transactions,'delays':self.delays,'cache_bytes':list(cache)}
 def input_policy(self):
  """Execute original budget policy with r3=1 (zero-charge path), mock final setter."""
  captured=[]
  def setter(m,a,size,u):
   captured.append({'port':m.reg_read(UC_ARM_REG_R0),'input_request':m.reg_read(UC_ARM_REG_R1),'charge_request':struct.unpack('<I',m.mem_read(m.reg_read(UC_ARM_REG_R2),4))[0]})
   m.reg_write(UC_ARM_REG_R0,0);m.reg_write(UC_ARM_REG_PC,m.reg_read(UC_ARM_REG_LR))
  hook=self.m.hook_add(UC_HOOK_CODE,setter,begin=B+0xcfb4,end=B+0xcfb4)
  for reg,v in zip((UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3),(0,0,0,1)):self.m.reg_write(reg,v)
  self.m.reg_write(UC_ARM_REG_SP,S+0xf000);self.m.reg_write(UC_ARM_REG_LR,E|1)
  self.m.emu_start(B+0x20cb8|1,E,count=10000);self.m.hook_del(hook)
  if self.m.reg_read(UC_ARM_REG_PC)!=E:raise RuntimeError('policy instruction bound')
  return captured
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--image',type=Path,required=True);p.add_argument('--oracle',type=Path,default=Path(__file__).with_name('original-classifier-oracle.json'));a=p.parse_args()
 data=a.image.read_bytes();o=json.loads(a.oracle.read_text())
 if o['image_sha256']!=SHA or o['image_size']!=282624 or o['case_count']!=72 or len(o['cases'])!=72:raise ValueError('oracle provenance/size mismatch')
 for row in o['cases']:
  r=Source(data).run(status=o['mock_status'],raw_samples=tuple(row['encoded_samples']))
  if (r['source_type'],r['brick_id'])!=(row['source_type'],row['divider_id']):raise RuntimeError(('original oracle mismatch',row['case']))
 # Original behavior counterexamples are separate from the safer Linux candidate.
 for n in range(1,9):
  r=Source(data).run(write_errors=(n,))
  if (r['source_type'],r['brick_id'])!=(4,4):raise RuntimeError('original route-write failure behavior changed')
 x=Source(data);x.run();r=x.run(raw_samples=())
 if r['source_type']!=4 or r['samples'] or r['routing']:raise RuntimeError('original cached-ID path mismatch')
 r=x.run(status=0,raw_samples=())
 if (r['source_type'],r['brick_id'])!=(1,0):raise RuntimeError('original detach did not clear ID')
 r=x.run(raw_samples=(2457,1802))
 if (r['source_type'],r['brick_id'])!=(4,2):raise RuntimeError('original reattach mismatch')
 for status,type_ in ((0,1),(16,2)):
  if Source(data).run(status=status,raw_samples=())['source_type']!=type_:raise RuntimeError('original raw status mismatch')
 if Source(data).run(read_errors=(1,2),raw_samples=())['source_type']!=1:raise RuntimeError('original status-error path mismatch')
 for samples,id_,budget in (((1802,1802),1,500),((2457,1802),2,1000),((1802,2457),4,2100),((2457,2457),9,1000)):
  x=Source(data);x.run(raw_samples=samples)
  if x.input_policy()!=[{'port':0,'input_request':budget,'charge_request':0}]:raise RuntimeError('original selected zero-charge input profile mismatch')
 print('72 original classifier cases plus transport/cache/detach/profile controls PASS; all I/O fake; no hardware/current grant')
