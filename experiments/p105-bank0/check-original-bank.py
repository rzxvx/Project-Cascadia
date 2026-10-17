"""Actual D1946 bank descriptor/common selector; fake lower bus only."""
import argparse,hashlib,json,struct
from pathlib import Path
from unicorn import Uc,UC_ARCH_ARM,UC_MODE_THUMB,UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3,UC_ARM_REG_SP,UC_ARM_REG_LR,UC_ARM_REG_PC
SHA='8eae16f060a71d4f3790385951588f8198d0f21cdb74146f9750941a3d764125';P,D,V,S,T,E=0x10000000,0x10001000,0x10002000,0x20000000,0x30000000,0x30000ffc
class Bank:
 def __init__(self,data):
  if len(data)!=18968576 or hashlib.sha256(data).hexdigest()!=SHA:raise ValueError('wrong13G36 KC')
  self.m=Uc(UC_ARCH_ARM,UC_MODE_THUMB);self.m.mem_map(0x81140000,0x13000);self.m.mem_write(0x81140000,data[0x46d000+0x81140000-0x804b7000:0x46d000+0x81153000-0x804b7000]);self.m.mem_map(P,0x10000);self.m.mem_map(S,0x10000);self.m.mem_map(T,0x1000)
  self.calls=[];self.used=False;self.w32(D,V);self.w32(V+0x350,T|1);self.w32(P+0x78,D)
  # Execute actual descriptor method to discover its pointer, verify stored vptr slot.
  if self.u32(0x8114d214+0x3a0)!=0x81145cf5:raise ValueError('actual descriptor slot mismatch')
  self.invoke(0x81145cf4,(P,0,0,0));self.descriptor=self.m.reg_read(UC_ARM_REG_R0)
  if self.descriptor!=0x8114e458 or bytes(self.m.mem_read(self.descriptor,5))!=bytes([1,3,0x57,0x1e,1]):raise ValueError('actual descriptor bytes mismatch')
  self.w32(P+0x70,self.descriptor);self.m.hook_add(UC_HOOK_CODE,self.bus,begin=T,end=T)
 def u32(self,p):return struct.unpack('<I',self.m.mem_read(p,4))[0]
 def w32(self,p,n):self.m.mem_write(p,struct.pack('<I',n))
 def invoke(self,entry,args):
  for r,v in zip((UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3),args):self.m.reg_write(r,v)
  self.m.reg_write(UC_ARM_REG_SP,S+0xff00);self.m.reg_write(UC_ARM_REG_LR,E|1);self.m.emu_start(entry|1,E,count=10000)
  if self.m.reg_read(UC_ARM_REG_PC)!=E:raise RuntimeError('originalinstructionbound')
 def bus(self,m,a,size,u):
  args=[m.reg_read(r)for r in (UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3)];n,ptr=struct.unpack('<II',m.mem_read(m.reg_read(UC_ARM_REG_SP),8))
  if args[:3]!=[D,2,1] or n!=1 or bytes(m.mem_read(args[3],1))!=b'\x1e':raise RuntimeError('unreviewed bank transfer')
  page=m.mem_read(ptr,1)[0];ret=self.error
  if not ret:self.hardware_bank=page
  self.calls.append({'command':args[1],'prefix':[0x1e],'payload':[page],'result':ret})
  m.reg_write(UC_ARM_REG_R0,ret);m.reg_write(UC_ARM_REG_PC,m.reg_read(UC_ARM_REG_LR))
 def run(self,logical=0x3d,cached=2,length=1,error=0,hardware=None):
  if self.used:raise RuntimeError('fresh instance required')
  if any(type(x)is not int for x in (logical,cached,length,error)) or not 0<=logical<=0xffff or not 0<=cached<=255 or not 1<=length<=32 or not 0<=error<=0xffffffff:raise ValueError('bad bounded input')
  self.used=True;self.error=error;self.hardware_bank=cached if hardware is None else hardware;self.m.mem_write(P+0x84,bytes([cached]));self.invoke(0x81140c48,(P,logical,length,0))
  return {'return_code':self.m.reg_read(UC_ARM_REG_R0),'original_descriptor':[1,3,87,30,1],'logical':logical,'length':length,'initial_cached_bank':cached,'final_cached_bank':self.m.mem_read(P+0x84,1)[0],'fake_hardware_bank':self.hardware_bank,'calls':self.calls,'hardware_access':False}

from types import SimpleNamespace
b=SimpleNamespace(Bank=Bank,P=P,D=D,V=V,S=S,T=T,E=E,SHA=SHA)
class Transfer(b.Bank):
 BUFFER=b.P+0x4000
 def __init__(self,data):
  super().__init__(data)
  # Additional image globals/import cells are fake mapped memory, not real pointers.
  self.m.mem_map(0x80000000,0x1140000);self.m.mem_map(0x81153000,0x1ad000)
  o=28
  for _ in range(struct.unpack_from('<I',data,16)[0]):
   cmd,n=struct.unpack_from('<II',data,o)
   if cmd==1:
    va,_,off,size=struct.unpack_from('<4I',data,o+24)
    if size and 0x80000000<=va and va+size<=0x81300000:self.m.mem_write(va,data[off:off+size])
   o+=n
  self.w32(b.P+0x80,1);self.logs=0;self.calls=[];self.error_at=0;self.page_values=[bytearray(256) for _ in range(3)]
  for page,values in enumerate(self.page_values):
   for i in range(256):values[i]=(page*64+i)&255
  for a in (0x81141d7c,0x81141c2c,0x81141d2c):self.m.hook_add(UC_HOOK_CODE,self.helper,begin=a,end=a)
 def helper(self,m,a,size,u):
  if a==0x81141d2c:raise RuntimeError('original stack guard')
  if a==0x81141c2c:self.logs+=1;ret=0
  else:ret=1 # explicit held bus-lock/current-thread fixture
  m.reg_write(UC_ARM_REG_R0,ret);m.reg_write(UC_ARM_REG_PC,m.reg_read(UC_ARM_REG_LR))
 def bus(self,m,a,size,u):
  args=[m.reg_read(r)for r in (UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3)];n,ptr=struct.unpack('<II',m.mem_read(m.reg_read(UC_ARM_REG_SP),8))
  if args[0]!=b.D or args[1]not in (1,2) or args[2]!=1 or not 1<=n<=9:raise RuntimeError('unreviewed lowertransfer')
  reg=m.mem_read(args[3],1)[0];ret=0xe00002c0 if self.error_at==len(self.calls)+1 else 0
  if args[1]==2:
   payload=bytes(m.mem_read(ptr,n))
   if not ret:
    if reg==0x1e and n==1:self.hardware_bank=payload[0]
    else:self.page_values[self.hardware_bank][reg:reg+n]=payload
   op='write'
  else:
   payload=bytes(self.page_values[self.hardware_bank][reg:reg+n]);op='read'
   if not ret:m.mem_write(ptr,payload)
  self.calls.append({'op':op,'prefix':[reg],'data':list(payload),'result':ret,'hardware_bank_after':self.hardware_bank})
  m.reg_write(UC_ARM_REG_R0,ret);m.reg_write(UC_ARM_REG_PC,m.reg_read(UC_ARM_REG_LR))
 def run_transfer(self,logical=0x3d,cached=2,count=1,write=False,error_at=0,hardware=None):
  if self.used:raise RuntimeError('fresh instance required')
  self.used=True;self.error_at=error_at;self.hardware_bank=cached if hardware is None else hardware;self.m.mem_write(b.P+0x84,bytes([cached]));self.m.mem_write(self.BUFFER,bytes([0xa5])*16)
  self.invoke(0x81140b4c if write else 0x81140cb0,(b.P,logical,self.BUFFER,count))
  return {'return_code':self.m.reg_read(UC_ARM_REG_R0),'logical':logical,'initial_cached':cached,'write':write,'calls':self.calls,'final_cached':self.m.mem_read(b.P+0x84,1)[0],'output_bytes':list(self.m.mem_read(self.BUFFER,count)),'logs':self.logs}
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('kernelcache',type=Path);p.add_argument('--oracle',type=Path,default=Path(__file__).with_name('native-page0-oracle.json'));a=p.parse_args();data=a.kernelcache.read_bytes();oracle=json.loads(a.oracle.read_text())
 if oracle['kernelcache_sha256']!=SHA or oracle['actual_descriptor']!=[1,3,87,30,1] or oracle['case_count']!=16 or len(oracle['cases'])!=16:raise ValueError('oracle provenance mismatch')
 for row in oracle['cases']:
  r=Transfer(data).run_transfer(logical=row['reg'],cached=row['cached_bank'],count=row['count'],write=row['write'])
  actual={'return_code':r['return_code'],'output_bytes':r['output_bytes'],'wire_calls':[{k:t[k]for k in ('op','prefix','data')}for t in r['calls']]}
  if actual!={k:row[k]for k in actual}:raise RuntimeError('native oracle mismatch')
 for bank in (0,1,2):
  r=Bank(data).run(logical=(bank<<8)|0x5d,cached=255)
  if r['final_cached_bank']!=bank or r['return_code']:raise RuntimeError('selector mismatch')
 for logical,n,switched in ((0x0a,6,False),(0x55,1,False),(0x56,1,True),(0x55,2,True),(0x61,1,True)):
  r=Bank(data).run(logical=logical,cached=1,length=n)
  if bool(r['calls'])!=switched:raise RuntimeError('shared prefix mismatch')
 r=Transfer(data).run_transfer(error_at=1)
 if r['return_code']!=0xe00002c0 or len(r['calls'])!=1 or r['final_cached']!=2:raise RuntimeError('failed selection mismatch')
 r=Transfer(data).run_transfer(error_at=2)
 if r['return_code']!=0xe00002c0 or r['final_cached']!=0:raise RuntimeError('data-error original cache behavior mismatch')
 r=Transfer(data).run_transfer(cached=0,hardware=2)
 if r['return_code'] or len(r['calls'])!=1 or r['calls'][0]['hardware_bank_after']!=2:raise RuntimeError('cache-trust original mismatch')
 print('16 native bank/packet oracle cases plus selector/shared-prefix/error/cache controls PASS; all I/O fake')
