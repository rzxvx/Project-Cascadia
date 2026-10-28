"""Original iBoot13 ADC6 polling and actual clock/divider; all I/O fake."""
import argparse,hashlib,json,struct
from pathlib import Path
from unicorn import Uc,UC_ARCH_ARM,UC_MODE_THUMB,UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3,UC_ARM_REG_SP,UC_ARM_REG_LR,UC_ARM_REG_PC
SHA='7dc177231f52e7032cd10ddb255c3a8fd6f62c33c5e4d731eda7e5e0a8b8fdcd';B,S,E=0x9ff00000,0x20000000,0x30000ffc
class ADC:
 def __init__(self,data):
  if len(data)!=282624 or hashlib.sha256(data).hexdigest()!=SHA:raise ValueError('wrong iBoot13 input')
  self.m=Uc(UC_ARCH_ARM,UC_MODE_THUMB);self.m.mem_map(B,0x70000);self.m.mem_write(B,data);self.m.mem_map(S,0x10000);self.m.mem_map(0x30000000,0x1000);self.m.mem_map(0x3f200000,0x1000)
  self.transactions=[];self.clocks=[];self.logs=0;self.fake_us=0;self.step_us=1000;self.busy_reads=0;self.polls=0;self.control=6;self.ordinal=0;self.errors=set();self.raw=0xabc;self.highflag=0
  for off in (0xc3c4,0xc4e8,0x1be4,0x33540,0x1be40):self.m.hook_add(UC_HOOK_CODE,self.io,begin=B+off,end=B+off)
 def io(self,m,a,size,u):
  off=a-B;args=[m.reg_read(r) for r in (UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3)]
  if off==0x1be4:
   self.fake_us+=self.step_us;counter=self.fake_us*24
   m.mem_write(0x3f200020,struct.pack('<I',counter&0xffffffff));m.mem_write(0x3f200028,struct.pack('<I',counter>>32));self.clocks.append(counter);return # actual coherent MMIO reader/div64 execute
  if off==0x1be40:raise RuntimeError('original stack guard')
  if off==0x33540:self.logs+=1;ret=0
  else:
   self.ordinal+=1;ret=-5 if self.ordinal in self.errors else 0
   if off==0xc4e8:
    if args[:2]!=[0,0x78] or args[3]!=2:raise RuntimeError('unexpected original write')
    reg,value=bytes(m.mem_read(args[2],2))
    if reg!=0x43:raise RuntimeError('unexpected write register')
    if not ret:self.control=value
    self.transactions.append({'op':'write','reg':reg,'value':value,'result':ret,'ordinal':self.ordinal})
   elif off==0xc3c4:
    out,n,flag=struct.unpack('<III',m.mem_read(m.reg_read(UC_ARM_REG_SP),12));reg=m.mem_read(args[2],1)[0]
    if args[:2]!=[0,0x79] or args[3]!=1:raise RuntimeError('unexpected read')
    if reg==0x43 and n==1:
     self.polls+=1
     if self.polls>self.busy_reads:self.control&=~0x20
     value=bytes([self.control])
    elif reg==0x44 and n==2:value=bytes([(self.raw&15)|self.highflag,self.raw>>4])
    else:raise RuntimeError(('unexpected read register/length',reg,n))
    if not ret:m.mem_write(out,value)
    self.transactions.append({'op':'read','reg':reg,'bytes':list(value),'result':ret,'ordinal':self.ordinal,'output_written':not bool(ret)})
   else:raise RuntimeError('unreviewed callback')
  m.reg_write(UC_ARM_REG_R0,ret&0xffffffff);m.reg_write(UC_ARM_REG_PC,m.reg_read(UC_ARM_REG_LR))
 def run(self,raw=0xabc,busy_reads=0,errors=(),step_us=1000,highflag=0):
  if any(type(x)is not int for x in (raw,busy_reads,step_us,highflag)) or not 0<=raw<=4095 or not 0<=busy_reads<=100 or not 1<=step_us<=10000 or highflag not in (0,0x80):raise ValueError('bad fake input')
  self.raw=raw;self.busy_reads=busy_reads;self.errors=set(errors);self.step_us=step_us;self.highflag=highflag
  self.m.mem_write(0x30000000,b'\xa5'*4);self.m.reg_write(UC_ARM_REG_R0,6);self.m.reg_write(UC_ARM_REG_R1,0x30000000);self.m.reg_write(UC_ARM_REG_SP,S+0xf000);self.m.reg_write(UC_ARM_REG_LR,E|1)
  self.m.emu_start(B+0xc88c|1,E,count=200000)
  if self.m.reg_read(UC_ARM_REG_PC)!=E:raise RuntimeError('instruction bound')
  return {'return_code':self.m.reg_read(UC_ARM_REG_R0),'sample_output':struct.unpack('<I',self.m.mem_read(0x30000000,4))[0],'final_control':self.control,'transactions':self.transactions,'clock_ticks_fake':self.clocks,'logs':self.logs}
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('image',type=Path);a=p.parse_args();d=a.image.read_bytes();o={}
 for name,kw in [('success',{}),('one-busy',{'busy_reads':1}),('timeout',{'busy_reads':100}),('failed-start',{'errors':(1,)}),('failed-poll',{'busy_reads':100,'errors':(2,)}),('failed-sample',{'errors':(3,)}),('failed-cleanup',{'errors':(4,)})]:o[name]=ADC(d).run(**kw)
 print(json.dumps(o,indent=2))
