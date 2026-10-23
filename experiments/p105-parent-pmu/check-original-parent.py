"""Full actual12H D1946 wrappers for normal parent logical registers; fake lower bus."""
import hashlib,struct,json,argparse
from pathlib import Path
from unicorn import Uc,UC_ARCH_ARM,UC_MODE_THUMB,UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3,UC_ARM_REG_SP,UC_ARM_REG_PC,UC_ARM_REG_LR
SHA='db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c';P,D,V,S,T,E=0x10000000,0x10001000,0x10002000,0x20000000,0x30000000,0x30000ffc
class Transfer12:
 def __init__(self,data):
  if hashlib.sha256(data).hexdigest()!=SHA:raise ValueError('wrong12H321 input')
  self.m=Uc(UC_ARCH_ARM,UC_MODE_THUMB);self.m.mem_map(0x80000000,0x1400000);o=28
  for _ in range(struct.unpack_from('<I',data,16)[0]):
   c,n=struct.unpack_from('<II',data,o)
   if c==1:
    va,vs,fo,fs=struct.unpack_from('<4I',data,o+24)
    if fs and 0x80000000<=va and va+fs<=0x81400000:self.m.mem_write(va,data[fo:fo+fs])
   o+=n
  for a,n in ((P,0x10000),(S,0x10000),(T,0x1000)):self.m.mem_map(a,n)
  self.w(P,0x80ed3208);self.w(P+0x68,0x80ed44f0);self.w(P+0x70,D);self.w(P+0x78,1);self.w(D,V);self.w(V+0x350,T|1)
  if self.r(0x80ed3208+0x378)!=0x80ec8741 or self.r(0x80ed3208+0x37c)!=0x80ec85e5 or self.m.mem_read(0x80ed44f0,5)!=bytes([1,3,0x57,0x1e,1]):raise ValueError('actualwrapper/descriptor mismatch')
  self.m.mem_write(P+0x7c,b'\2');self.bank=2;self.calls=[];self.used=False
  self.m.hook_add(UC_HOOK_CODE,self.hook)
 def w(self,a,v):self.m.mem_write(a,struct.pack('<I',v))
 def r(self,a):return struct.unpack('<I',self.m.mem_read(a,4))[0]
 def hook(self,m,a,size,ctx):
  if a==0x80ec8eb0:raise RuntimeError('original stackguard')
  if a in (0x80ec8ed0,0x80ec8dd0):m.reg_write(UC_ARM_REG_R0,1 if a==0x80ec8ed0 else 0);m.reg_write(UC_ARM_REG_PC,m.reg_read(UC_ARM_REG_LR));return
  if a!=T:return
  args=[m.reg_read(x)for x in(UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3)];n,p=struct.unpack('<II',m.mem_read(m.reg_read(UC_ARM_REG_SP),8));reg=m.mem_read(args[3],1)[0]
  if args[0]!=D or args[1]not in(1,2)or args[2]!=1 or not 1<=n<=18:raise RuntimeError('unexpected original transfer')
  if args[1]==2:
   payload=bytes(m.mem_read(p,n));op='write'
   if reg==0x1e:
    if payload!=b'\0':raise RuntimeError('notpage0')
    self.bank=0
  else:payload=bytes((self.bank*64+reg+i)&255 for i in range(n));m.mem_write(p,payload);op='read'
  self.calls.append({'op':op,'prefix':[reg],'data':list(payload),'bank':self.bank});m.reg_write(UC_ARM_REG_R0,0);m.reg_write(UC_ARM_REG_PC,m.reg_read(UC_ARM_REG_LR))
 def run(self,reg,count=1,write=False):
  if self.used or type(reg)is not int or not 0<=reg<=255 or type(count)is not int or not 1<=count<=18:raise ValueError('bounded fresh instance required')
  self.used=True;buf=P+0x4000;self.m.mem_write(buf,bytes([0xa5])*32)
  for r,v in zip((UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3),(P,reg,buf,count)):self.m.reg_write(r,v)
  self.m.reg_write(UC_ARM_REG_SP,S+0xff00);self.m.reg_write(UC_ARM_REG_LR,E|1);self.m.emu_start((0x80ec85e4 if write else 0x80ec8740)|1,E,count=10000)
  if self.m.reg_read(UC_ARM_REG_PC)!=E or self.m.reg_read(UC_ARM_REG_R0)!=0:raise RuntimeError('original completion')
  if len(self.calls)!=2 or self.calls[0]!={'op':'write','prefix':[0x1e],'data':[0],'bank':0}or self.calls[1]['prefix']!=[reg]or self.calls[1]['bank']!=0:raise RuntimeError('source did not select0')
  return {'logical_register':reg,'count':count,'write':write,'calls':self.calls,'output':list(self.m.mem_read(buf,count))}
if __name__=='__main__':
 a=argparse.ArgumentParser(description=__doc__);a.add_argument('image',type=Path);a.add_argument('--output',type=Path);args=a.parse_args();data=args.image.read_bytes();rows=[]
 for reg in [*range(1,10),*range(0x12,0x1b),0x1b,0x22,0x23,0x5b,*range(0x61,0x73),0xa1,0xaf,0xd0,0xd1,0xd2]:
  rows.append(Transfer12(data).run(reg));
  if reg>9:rows.append(Transfer12(data).run(reg,write=True))
 rows.append(Transfer12(data).run(1,9));rows.append(Transfer12(data).run(0x12,9,True));rows.append(Transfer12(data).run(0x61,18));result=json.dumps({'input_sha256':SHA,'actualread':'80ec8740','actualwrite':'80ec85e4','bankhelper':'80ec86d8','hardware_access':False,'case_count':len(rows),'cases':rows},indent=2)+'\n'
 if args.output:args.output.write_text(result);print(f'{len(rows)} native wrapper cases PASS; normalized evidence saved')
 else:print(result)
