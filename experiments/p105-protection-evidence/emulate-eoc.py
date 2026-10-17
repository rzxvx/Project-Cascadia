"""Two matching D1946 source recovery APIs; fake PMU only, no device calls."""
import argparse,hashlib,json,struct
from pathlib import Path
from unicorn import Uc,UC_ARCH_ARM,UC_MODE_THUMB,UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3,UC_ARM_REG_SP,UC_ARM_REG_LR,UC_ARM_REG_PC
S,P,V,D,DV,STACK,TRAPS,END=0x10000000,0x10001000,0x10002000,0x10003000,0x10003100,0x20000000,0x30000000,0x30000ffc
HASH={'12':'db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c','13':'8eae16f060a71d4f3790385951588f8198d0f21cdb74146f9750941a3d764125'}
CONF={'12':{'lo':0x80ecb000,'hi':0x80ed8000,'sourcevt':0x80ed3c68,'pmuoff':0x18,'pulse':0x80ed0c94,'mode':0x80ed09d4,'force':0x80ed0ce0,'notify':0x80ed1f5c,'slots':(0x74,0x78)},'13':{'lo':0x81144000,'hi':0x81154000,'sourcevt':0x8114dbf0,'pmuoff':0x20,'pulse':0x8114a744,'mode':0x8114a3e4,'force':0x8114a794,'notify':0x8114be0c,'slots':(0x80,0x84)}}
def segs(data):
 o=28;out=[]
 for _ in range(struct.unpack_from('<I',data,16)[0]):
  cmd,size=struct.unpack_from('<II',data,o)
  if cmd==1:out.append(struct.unpack_from('<4I',data,o+24))
  o+=size
 return out
class EOC:
 def __init__(self,data,version):
  if version not in HASH or hashlib.sha256(data).hexdigest()!=HASH[version]:raise ValueError('mismatched original KC')
  self.version=version;self.c=CONF[version];self.m=Uc(UC_ARCH_ARM,UC_MODE_THUMB);self.m.mem_map(self.c['lo'],self.c['hi']-self.c['lo'])
  for va,_,off,n in segs(data):
   a,b=max(va,self.c['lo']),min(va+n,self.c['hi'])
   if a<b:self.m.mem_write(a,data[off+a-va:off+b-va])
  for slot,entry in zip(self.c['slots'],('pulse','force')):
   if struct.unpack('<I',self.m.mem_read(self.c['sourcevt']+slot,4))[0]!=self.c[entry]|1:raise ValueError('original source slot mismatch')
  self.m.mem_map(S,0x10000);self.m.mem_map(STACK,0x10000);self.m.mem_map(TRAPS,0x1000)
  self.w32(S,self.c['sourcevt']);self.w32(S+self.c['pmuoff'],P);self.w32(P,V);self.w32(V+0x378,TRAPS|1);self.w32(V+0x37c,TRAPS+4|1)
  self.registers=bytearray(256);self.transactions=[];self.notifications=[];self.keys=[];self.logs=0;self.errors=set();self.ordinal=0;self.used=False
  for a in (TRAPS,TRAPS+4,self.c['notify']):self.m.hook_add(UC_HOOK_CODE,self.io,begin=a,end=a)
  if version=='13':
   self.m.mem_map(0x8056a000,0x1000)
   for va,_,off,n in segs(data):
    a,b=max(va,0x8056a000),min(va+n,0x8056b000)
    if a<b:self.m.mem_write(a,data[off+a-va:off+b-va])
   self.w32(D,DV);self.w32(DV+0x98,TRAPS+8|1)
   for a in (TRAPS+8,0x8114bbac):self.m.hook_add(UC_HOOK_CODE,self.io,begin=a,end=a)
 def w32(self,a,v):self.m.mem_write(a,struct.pack('<I',v))
 def io(self,m,a,size,_):
  args=[m.reg_read(r)for r in (UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3)];ret=0
  if a in (TRAPS,TRAPS+4):
   self.ordinal+=1;obj,reg,ptr,n=args
   if obj!=P or(reg,n)not in((0x41,1),(0x1b,2)):raise RuntimeError(('unreviewed transaction',args))
   ret=0xe00002c0 if self.ordinal in self.errors else 0
   value=bytes(self.registers[reg:reg+n]) if a==TRAPS else bytes(m.mem_read(ptr,n))
   if not ret:
    if a==TRAPS:m.mem_write(ptr,value)
    else:self.registers[reg:reg+n]=value
   self.transactions.append({'op':'read' if a==TRAPS else 'write','reg':reg,'bytes':list(value),'result':ret,'ordinal':self.ordinal})
  elif a==self.c['notify']:
   if args[:2]!=[S,0]:raise RuntimeError('unknown notifierarguments')
   self.notifications.append(args[:2])
  elif a==TRAPS+8:
   if args[:2]!=[D,0x8114cb6c]:raise RuntimeError('unknown dictionarykey')
   self.keys.append('AppleForceResetEndOfCharge');ret=0x10004000 # opaque nonnull, no bool-value evaluation
  elif a==0x8114bbac:self.logs+=1
  else:raise RuntimeError('unknown mock')
  m.reg_write(UC_ARM_REG_R0,ret);m.reg_write(UC_ARM_REG_PC,m.reg_read(UC_ARM_REG_LR))
 def run(self,kind='pulse',initial=0xaa,errors=()):
  if self.used:raise RuntimeError('fresh instance required')
  if kind not in ('pulse','mode0','mode1','force','dictionary')or type(initial)is not int or not 0<=initial<=255 or any(type(n)is not int or not 1<=n<=3 for n in errors):raise ValueError('bad boundedfixture')
  if kind=='dictionary' and self.version!='13':raise ValueError('specialkey only checked13')
  self.used=True;self.errors=set(errors);self.registers[0x41]=initial;self.registers[0x1b]=0xa1;self.registers[0x1c]=0x13
  self.m.reg_write(UC_ARM_REG_R0,S);self.m.reg_write(UC_ARM_REG_R1,1 if kind=='mode1' else D if kind=='dictionary' else 0);self.m.reg_write(UC_ARM_REG_R2,0);self.m.reg_write(UC_ARM_REG_SP,STACK+0xff00);self.m.reg_write(UC_ARM_REG_LR,END|1)
  addr=0x8056a3c0 if kind=='dictionary' else self.c['mode'] if kind.startswith('mode') else self.c[kind]
  self.m.emu_start(addr|1,END,count=10000)
  if self.m.reg_read(UC_ARM_REG_PC)!=END:raise RuntimeError('instructionbound')
  return{'version':self.version,'entry':hex(addr),'return_code':self.m.reg_read(UC_ARM_REG_R0),'transactions':self.transactions,'notifications':self.notifications,'keys':self.keys,'final':{'41':self.registers[0x41],'1b':self.registers[0x1b],'1c':self.registers[0x1c]}}
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('version',choices=HASH);p.add_argument('image',type=Path);a=p.parse_args();d=a.image.read_bytes();r=[]
 for kind in ('pulse','mode0','mode1','force')+ (('dictionary',)if a.version=='13' else()):
  for e in ((),(1,),(2,),(3,))if kind=='pulse' else((),(1,),(2,)):
   r.append(EOC(d,a.version).run(kind=kind,errors=e))
 print(json.dumps(r,indent=2))
