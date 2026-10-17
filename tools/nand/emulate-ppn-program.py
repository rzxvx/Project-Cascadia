#!/usr/bin/env python3
"""Original93b8 orchestration only; strict virtual MMIO/helper fakes.
Runs actual queue builder on synthetic RAM, never accesses host device registers.
Hardware responses/CDMA/feature state are deliberately synthetic and NOT proof
of persistent completion. Output is a semantic trace of synthetic registers, with no firmware bytes.
No executable device probe or real DMA/MMIO backend exists.
"""
import argparse,struct,itertools,json
from pathlib import Path
from unicorn import Uc,UC_ARCH_ARM,UC_MODE_ARM,UC_HOOK_CODE,UC_HOOK_MEM_WRITE,UC_HOOK_MEM_READ,UC_PROT_READ,UC_PROT_EXEC,UC_PROT_WRITE
from unicorn.arm_const import UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3,UC_ARM_REG_SP,UC_ARM_REG_LR,UC_ARM_REG_PC,UC_ARM_REG_CPSR,UC_CPU_ARM_CORTEX_A9
from ppn_12h321_inventory import inventory,segments
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--kernelcache',type=Path,required=True)
parser.add_argument('--output',type=Path,required=True)
args=parser.parse_args()
if not args.kernelcache.is_file() or args.kernelcache.stat().st_size>64*1024**2:
 parser.error('regular bounded kernelcache file required')
if args.output.exists():parser.error('output already exists')
D=args.kernelcache.read_bytes();I=inventory(D);T=segments(D,0)
s=next(x for z in I['kexts']['com.apple.driver.IOP_s5l8942xiop_firmware']['segments'] for x in z['sections']if x['name']=='fwFirmwareImage')
t=next(t for t in T if t['address']<=s['address']<t['address']+t['file_size']);fo=t['offset']+s['address']-t['address'];B=D[fo:fo+s['size']]
MEM=0x10000000;FMI=MEM;STATE=MEM+0x1000;REQ=MEM+0x2000;SCRATCH=MEM+0x4000;ARRAYS=(MEM+0x5000,MEM+0x5200);DATA=MEM+0x6000;META=MEM+0x6200;STACK=MEM+0xe000;STOP=0x3000
REGS=(UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3)
u=Uc(UC_ARCH_ARM,UC_MODE_ARM);u.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_A9)
u.mem_map(0,0x24000,UC_PROT_READ|UC_PROT_EXEC);u.mem_write(0,B);u.mem_map(MEM,0x10000,UC_PROT_READ|UC_PROT_WRITE)
for base in (0x31200000,0x31300000):
 for ofs,z in ((0,0x1000),(0x40000,0x1000),(0xc0000,0x2000)):u.mem_map(base+ofs,z,UC_PROT_READ|UC_PROT_WRITE)
trace=[];steps=0;stopped=False;bus=None;status=0x40;waitfail=None;drains=0;counts=None;feature_lists={};ce_mask=1
class OriginalRequire(Exception):pass

def put(a,f,*v):u.mem_write(a,struct.pack(f,*v))
def get(a,f):return struct.unpack(f,u.mem_read(a,struct.calcsize(f)))
def ret(v=1):u.reg_write(UC_ARM_REG_R0,v);u.reg_write(UC_ARM_REG_PC,u.reg_read(UC_ARM_REG_LR))
def hook(uc,at,n,_):
 global steps,stopped,drains
 steps+=1;assert steps<30000,('instructionlimit',hex(at))
 if at==STOP:stopped=True;uc.emu_stop();return
 a,b,c,d=(uc.reg_read(x)for x in REGS);sp=uc.reg_read(UC_ARM_REG_SP)
 if at==0x5578:
  assert a==1 and b in (5,6,7,8) and d in (bus+0x14,bus+0x18)
  vals=get(sp,'<IIII');assert vals[0]>0 and vals[0]<=0x200000 and vals[1]in(1,4) and vals[2]in(1,8,32)
  assert MEM<=c< MEM+0x10000
  trace.append(('dma-start',b,d,*vals));ret();return
 if at==0x55dc:
  assert a in (5,6,7,8)and b==2000000
  trace.append(('dma-wait',a,b));ret(0 if a==waitfail else 1);return
 if at==0x566c:assert a in(5,6,7,8);trace.append(('dma-cancel',a));ret();return
 if at==0xaf6c:
  assert a==1 and MEM<=b and b+c<=MEM+0x10000 and c%32==0
  trace.append(('cache',a,b,c));ret();return
 if at==0xb5b8:assert MEM<=a<MEM+0x10000;trace.append(('translate',a));ret(a);return
 if at==0xa108:
  assert a==FMI and STACK-0x200<=b<STACK and STACK-0x200<=c<STACK
  drains+=1;trace.append(('original-drain',drains,status))
  # Execute genuine a108..a1e4, with hardware response registers supplied below.
  return
 if at==0x32a4:assert a==FMI;trace.append(('bus-stop',));ret();return
 if at==0x5e34:
  assert a==FMI and b in(0,1) and c in(0x3080,0x3280,0x3180)and d==SCRATCH
  length,error=get(sp,'<II');assert length==512 and STACK-0x200<=error<STACK
  pages=feature_lists.get((b,c),())
  assert len(pages)<=127
  u.mem_write(SCRATCH,bytes(512));put(SCRATCH,'<'+'I'*(len(pages)+1),len(pages),*pages)
  put(error,'<B',0x40);trace.append(('feature-query',b,c,length,tuple(pages)));ret();return
 if at==0x5f50:
  assert a==FMI and b in(0,1) and c==0x3380 and STACK-0x200<=d<STACK
  length,s1,s2=get(sp,'<III');assert (length,s1,s2)==(4,0,0)
  trace.append(('feature-set',b,c,length,get(d,'<I')[0]));ret();return
 if at==0xd8c8:trace.append(('diagnostic',at));ret(0);return
 if at==0xb2c4:
  trace.append(('original-require',uc.reg_read(UC_ARM_REG_LR)&~1));raise OriginalRequire()
 assert (0x93b8<=at<0xa0bc)or(0x6488<=at<0x6568)or(0xa108<=at<0xa1fa),('unexpected code',hex(at))

def memwrite(uc,k,a,n,v,_):
 global ce_mask
 if any(x<=a<x+0xc2000 for x in(0x31200000,0x31300000)):
  assert bus<=a<bus+0xc2000 and n==4
  ofs=a-bus;assert ofs in(0,4,0x34,0x40020,0xc0000,0xc0014,0xc0020,0xc002c),('unrecognized MMIOwrite',hex(ofs))
  trace.append(('virtual-mmio',ofs,v))
  if ofs==0xc0000 and v&0xff000000==0x09000000 and v&0xff:ce_mask=v&0xff
  return
 assert (STACK-0x200<=a and a+n<=STACK+0x100)or(REQ<=a and a+n<=REQ+0x1000)or(STATE+0x208<=a and a+n<=STATE+0x22c)or any(x<=a and a+n<=x+512 for x in ARRAYS),('unrecognized RAMwrite',hex(a),n)
def memread(uc,k,a,n,v,_):
 if any(x<=a<x+0xc2000 for x in(0x31200000,0x31300000)):
  assert bus<=a<bus+0xc2000 and n==4
  ofs=a-bus;assert ofs in(4,0xc0004,0xc0018,0xc0020,0x40048,0x4000c),('unrecognized MMIOread',hex(ofs))
  # Fake both FIFO depth registers empty; these are controller response assumptions.
  if ofs in(0xc0004,0xc0018):put(a,'<I',0)
  elif ofs==0xc0020:put(a,'<I',1)
  elif ofs==0x40048:put(a,'<I',status)
  elif ofs==0x4000c:put(a,'<I',ce_mask)
u.hook_add(UC_HOOK_CODE,hook);u.hook_add(UC_HOOK_MEM_WRITE,memwrite);u.hook_add(UC_HOOK_MEM_READ,memread)

def case(busnum,n,split,rawstatus,failchannel=None,lists=None):
 global trace,steps,stopped,bus,status,waitfail,drains,counts,feature_lists,ce_mask
 trace=[];steps=0;stopped=False;bus=0x31200000+busnum*0x100000;status=rawstatus;waitfail=failchannel;drains=0;ce_mask=1;feature_lists={} if lists is None else lists
 u.mem_write(MEM,bytes(0x10000));put(0x13188,'<I',0x12345678)
 for ofs,val in((0,busnum),(4,bus),(8,24),(0x34,16),(0x3c,4096),(0x48,5),(0x1dc,STATE),(0x1f4,SCRATCH)):put(FMI+ofs,'<I',val)
 put(STATE+0xec,'<I',128);put(STATE+0x204,'<B',3)
 for i,x in enumerate(ARRAYS):put(STATE+0x224+4*i,'<I',x)
 put(REQ+8,'<HH',n,n*4);counts=(split,n-split)
 put(REQ+0xe,'<HHH',0,counts[0],0);put(REQ+0x14,'<HHH',1,counts[1],split)
 rows=[]
 for i in range(n):
  group=0 if i<split else 1;row=0x800000+i*256;put(REQ+0x20+12*i,'<IBBB5x',row,group,4,0);rows.append(row)
 for r,val in zip(REGS,(FMI,REQ,DATA,META)):u.reg_write(r,val)
 u.reg_write(UC_ARM_REG_CPSR,0x33);u.reg_write(UC_ARM_REG_SP,STACK);u.reg_write(UC_ARM_REG_LR,STOP|1)
 required=False
 try:u.emu_start(0x93b9,0,count=30000,timeout=2000000)
 except OriginalRequire:required=True
 except Exception:
  print('unexpected stop',hex(u.reg_read(UC_ARM_REG_PC)),'steps',steps,'trace',trace[-8:]);raise
 assert stopped or required
 return {'bus':busnum,'count':n,'groups':counts,'status':status,'wait_failure':failchannel,'original_require':required,'result':u.reg_read(UC_ARM_REG_R0),'aggregate':get(REQ+0xc,'<B')[0], 'geb_latched':get(STATE+0x208,'<I')[0],'failing_ce':get(STATE+0x20c,'<I')[0], 'entry_status':[get(REQ+0x26+12*i,'<B')[0]for i in range(n)],'trace':trace.copy()}
if __name__=='__main__':
 results=[]
 for bn,n in itertools.product((0,1),(1,2,3,5)):
  for split in sorted(set((0,n//2,n))):
   for raw in (0x40,0x41,0x50):
    x=case(bn,n,split,raw);results.append(x)
 # Independent semantic oracle for current original orchestration.
 for x in results:
  assert not x['original_require']
  n=x['count'];raw=x['status'];split=x['groups'][0];bn=x['bus'];t=x['trace'];done=1 if raw==0x50 else n
  params=[e[2]for e in t if e[0]=='virtual-mmio' and e[1]==0xc0014]
  perpage=[v for v in params if v in (0x87,0x8a)]
  assert perpage==[0x8a if i==(split-1 if i<split else n-1) else 0x87 for i in range(done)]
  assert [v for v in params if 0x800000<=v<0x900000]==[0x800000+i*256 for i in range(done)]
  commands=[e[2]for e in t if e[0]=='virtual-mmio' and e[1]==0xc0000]
  assert commands.count(0x11001709)==done and commands.count(0x8000017)==done
  assert commands.count(0x11000605)==(sum(c>0 for c in x['groups']) if n>1 else 0)+(sum(c>0 for c in x['groups'])if raw==0x41 else done)
  assert x['geb_latched']==(raw==0x50)
  if raw==0x50:assert x['failing_ce']==bn*8+(0 if split else 1)
  dma=[e for e in t if e[0]=='dma-start' and e[5]!=32]
  assert dma==[('dma-start',5+2*bn,0x31200014+bn*0x100000,n*16384,4,8,0),('dma-start',6+2*bn,0x31200018+bn*0x100000,n*64,1,1,0)]
  assert x['aggregate']==raw and x['result']=={0x40:1,0x41:0x80200000,0x50:0x80000020}[raw]
  expected_entries=([0x40 if i in(0,split) else 0 for i in range(n)]if raw==0x41 else [0x40]*done+[0]*(n-done))
  assert x['entry_status']==expected_entries,(x['count'],x['groups'],raw,x['entry_status'])
  cancels=[e[1]for e in t if e[0]=='dma-cancel'];assert cancels==([6+2*bn,5+2*bn]if raw==0x50 else [])
 extra=[]
 for bn,ch in itertools.product((0,1),(0,1)):
  x=case(bn,1,1,0x40,5+2*bn+ch);extra.append(x)
  assert not x['original_require'] and x['result']==1 and x['aggregate']==0x40
  assert [e[1]for e in x['trace'] if e[0]=='dma-cancel']==[6+2*bn,5+2*bn]
 for bn in (0,1):
  x=case(bn,2,2,0x40,5+2*bn);extra.append(x)
  assert x['original_require'] and any(e==('original-require',0xa0bc)for e in x['trace'])
  assert not any(e[0]=='dma-cancel'for e in x['trace'])
 for bn in (0,1):
  lists={(0,0x3080):(0x800000,), (0,0x3280):(0x800000,), (0,0x3180):(0x800000,)}
  x=case(bn,1,1,0x41,lists=lists);extra.append(x)
  assert x['result']==0x80200000 and x['entry_status']==[0x45]
  assert [e[2]for e in x['trace']if e[0]=='feature-query']==[0x3080,0x3280,0x3180]
  assert [e[2:]for e in x['trace']if e[0]=='feature-set']==[(0x3380,4,1)]
 results.extend(extra)
 print('executions',len(results),'sample',[(x['count'],x['groups'],hex(x['status']),hex(x['result']),hex(x['aggregate']))for x in results[:9]])
 # Full trace never published by default; address-only semantic JSON stays private.
 with args.output.open('x') as output:output.write(json.dumps(results,indent=2)+'\n')
 print('PASS: original IOP write orchestration executed under strict virtual MMIO/helper boundaries; hardware responses synthetic, no device access.')
