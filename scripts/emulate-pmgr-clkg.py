"""Actual original parent recursion+notification+deferred register apply; all MMIO fake."""
import hashlib,importlib.util,json,struct
from pathlib import Path
from unicorn import *
from unicorn.arm_const import *
ROOT=Path(__file__).resolve().parents[1]
s=importlib.util.spec_from_file_location('e',ROOT/'scripts/usb-phy-plan.py')
e=importlib.util.module_from_spec(s);s.loader.exec_module(e)
data=adt=table=None
MIN=MAX=None
rows={}
def load_sources(kernelcache=None,device_tree=None):
 global data,adt,table,MIN,MAX,rows
 cache=Path(kernelcache) if kernelcache else ROOT/'build/firmware/kernelcache.12H321.macho'
 dt=Path(device_tree) if device_tree else ROOT/'build/usb-host-research/adt-evidence/DeviceTree.p105ap.raw'
 image=cache.read_bytes();tree=dt.read_bytes()
 if hashlib.sha256(image).hexdigest()!=e.KC_SHA256 or hashlib.sha256(tree).hexdigest()!=e.ADT_SHA256:
  raise ValueError('exact official P10512H321 inputs required')
 node,end=e.dtparse.parse_node(tree,0)
 if end!=len(tree):raise ValueError('trailing ADT bytes')
 tables=[]
 def walk(n):
  if n['props'].get('name',b'').rstrip(b'\0')==b'pmgr':tables.append(n['props'])
  for c in n['children']:walk(c)
 walk(node)
 if len(tables)!=1 or len(tables[0]['device-clocks'])!=5104:raise ValueError('unexpected PMGR table')
 parsed=tables[0]['device-clocks'];minimum=struct.unpack('<I',tables[0]['device-clocks-min'])[0];maximum=struct.unpack('<I',tables[0]['device-clocks-max'])[0]
 if (minimum,maximum)!=(56,128):raise ValueError('unexpected native PMGR limits')
 data,adt,table,MIN,MAX=image,tree,parsed,minimum,maximum
 rows={table[o+3]:o//44 for o in range(0,len(table),44)}

OBJ,ACTIVE,COUNTS,COMPARE,INDEX,TABLE=0x10000000,0x10002000,0x10003000,0x10004000,0x10005000,0x10008000
IO,FUNC,ODATA,FNAME,FBYTES,OUT,CG,VT=0x10001000,0x10001100,0x10001200,0x10001400,0x10001500,0x10001600,0x10017000,0x10017400
REGS=0x10018000
STACK,TRAP,END=0x20000000,0x30000000,0x30000ffc
REG=[UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3]
def experiment(requests, clock_word_override=None, initial_reg_seed=0x24600000,
               auto_ack=True, off_zero_indices=(), allow_budget_exhaustion=False, base_init=True, use_wrong_cpui=False, parent_cast=True):
 if data is None:raise ValueError('load pinned sources before emulation')
 if not 1<=len(requests)<=64 or any(cid not in rows or state not in(0,1)for cid,state in requests):raise ValueError('bounded known ID requests required')
 u=Uc(UC_ARCH_ARM,UC_MODE_THUMB);lo,hi=0x80b8b000,0x80b92000;u.mem_map(lo,hi-lo);u.mem_write(lo,data[e.address_offset(data,lo):e.address_offset(data,hi-1)+1])
 for a,size in((OBJ,0x20000),(STACK,0x10000),(TRAP,0x1000)):u.mem_map(a,size)
 def w(a,v):u.mem_write(a,struct.pack('<I',v))
 def r(a):return struct.unpack('<I',u.mem_read(a,4))[0]
 guardptr=e.pic_literal_pointer(data,0x80b8c980,0x80b8c986,0);guard=struct.unpack_from('<I',data,e.address_offset(data,guardptr))[0]
 page=guard&~0xfff;u.mem_map(page,0x1000);w(guard,0x12345678)
 w(OBJ,0x80b90938);
 # +448 is real inherited notification helper. With synthetic null observer+530 it returns0.
 for page in(0x80398000,0x802c5000):u.mem_map(page,0x1000);u.mem_write(page,data[e.address_offset(data,page):e.address_offset(data,page+0xfff)+1])
 corelo,corehi=0x804a9000,0x804e0000;u.mem_map(corelo,corehi-corelo);u.mem_write(corelo,data[e.address_offset(data,corelo):e.address_offset(data,corehi-1)+1])
 for offset,value in((0x1c8,MIN),(0x1cc,MAX),(0x1d8,ACTIVE),(0x1fc,COUNTS),(0x200,COMPARE),(0x204,INDEX),(0x564,TABLE)):w(OBJ+offset,value)
 u.mem_write(TABLE,table)
 if clock_word_override is not None:
  assert 0<=clock_word_override<=0xffffffff
  w(TABLE+44*rows[42]+0x10,clock_word_override)
 for index in off_zero_indices:
  assert 1<=index<=87
  off=OBJ+0x590+4*(index//32);w(off,r(off)|(1<<(index%32)))
 w(OBJ+0x550,REGS)
 w(OBJ+0x1e0,OBJ+0x15000);w(OBJ+0x1e4,OBJ+0x16000)
 w(OBJ+0x1c0,0x12345678);w(OBJ+0x20c,OBJ+0x12000)
 for off in range(0,0x1800,4):w(REGS+off,initial_reg_seed)
 for cid,index in rows.items():u.mem_write(INDEX+cid,bytes([index]))
 w(IO,0x80b90138);w(IO+0x78,FUNC)
 w(ODATA,VT);w(FNAME,VT);w(VT+0x68,(TRAP+4)|1);w(VT+0x10,(TRAP+8)|1)
 w(CG,VT+0x100);w(VT+0x100+0x70,TRAP|1);w(OBJ+0x1b4,CG)
 # Exact property bytes, matched by real factory. The synthetic identity
 # of IOFunctionParent is separate from this causal source dispatch.
 w(FBYTES,0x10000000);w(FBYTES+4,0x63707549 if use_wrong_cpui else 0x636c6b47)
 g=e.pic_literal_pointer(data,0x804b01b8,0x804b01be,0);w(r(g),FNAME)
 assert e.pic_literal_pointer(data,0x804b0288,0x804b028a,0)+8==0x804d4468
 events=[];steps=0;io_reads=0;io_writes=0;recursion_flags=None
 def hook(m,a,n,_):
  nonlocal steps,recursion_flags
  steps+=1
  if steps>12000:raise RuntimeError('bounded original recursion')
  if a==0x804b3204:recursion_flags=m.reg_read(REG[0])
  if a==END:m.emu_stop();return
  x,y,z,q=[m.reg_read(v)for v in REG]
  if a==TRAP:
   assert x==CG and y==0x804b3141 and z in rows and q in(0,1)
   assert r(m.reg_read(UC_ARM_REG_SP))==0 and r(m.reg_read(UC_ARM_REG_SP)+4)==0
   events.append(['fake_CommandGate_runAction',hex(y),z,q]);m.reg_write(REG[0],OBJ);m.reg_write(REG[1],z);m.reg_write(REG[2],q);m.reg_write(REG[3],0);m.reg_write(UC_ARM_REG_PC,y);return
  if a==TRAP+4:
   assert x==ODATA;events.append(['fake_OSData_bytes']);m.reg_write(REG[0],FBYTES)
  elif a==TRAP+8:
   assert x in(ODATA,FNAME);events.append(['fake_retain',hex(x)])
  elif a==0x804c9a34:
   assert x==0x18;events.append(['fake_allocate',x]);m.reg_write(REG[0],FUNC)
  elif a==0x804c9a04:
   assert x==FUNC;events.append(['fake_OSObject_constructor'])
  elif a==0x804c9b94:
   events.append(['fake_meta_release'])
  elif a==0x804c9884:
   assert x in(ODATA,OBJ);events.append(['fake_metacast',hex(x),hex(y)]);m.reg_write(REG[0],0 if x==OBJ and not parent_cast else x)
  elif a==0x802c5340:
   assert x==FUNC;events.append(['fake_OSObject_init',base_init]);m.reg_write(REG[0],int(base_init))
  elif a==0x802c5498:
   assert x==OBJ;events.append(['fake_Performance_retain'])
  elif a in(0x804c9da4,0x804c93f4,0x804c9404):
   events.append(['fake_irq_or_lock',hex(a)]);m.reg_write(REG[0],0)
  elif a==0x80b8f46c:
   assert z==12;events.append(['fake_memcpy',z]);m.mem_write(x,bytes(m.mem_read(y,z)))
  elif a==0x80b8f47c:
   assert z in(12,16)and y==0;events.append(['fake_bzero',z]);m.mem_write(x,b'\0'*z)
  else:return
  m.reg_write(UC_ARM_REG_PC,m.reg_read(UC_ARM_REG_LR))
 def mmio(m,access,a,n,v,_):
  nonlocal io_reads,io_writes
  assert n==4
  off=a-REGS
  if access==UC_MEM_READ:
   io_reads+=1;events.append(['fake_pmgr_read',hex(off),r(a)])
  else:
   io_writes+=1;events.append(['fake_pmgr_write',hex(off),v])
 def acknowledge(m,a,n,_):
  # Fake PMGR device: at every real read accessor invocation, ack target nibble.
  off=m.reg_read(UC_ARM_REG_R1)
  if auto_ack and a==0x80b8df5c and 0x1000<=off<0x1800:
   val=r(REGS+off);w(REGS+off,(val&~0xf0)|((val&15)<<4))
 u.hook_add(UC_HOOK_CODE,hook);u.hook_add(UC_HOOK_CODE,acknowledge)
 u.hook_add(UC_HOOK_MEM_READ|UC_HOOK_MEM_WRITE,mmio,begin=REGS,end=REGS+0x17ff)
 # Source startup chooses the actual object's +350 as default CG action.
 u.reg_write(UC_ARM_REG_R4,OBJ);u.emu_start(0x804afa0b,0x804afa1c,count=20);assert r(OBJ+0x1b8)==0x804b3141
 def invoke(pc,args,stackwords=()):
  nonlocal steps
  steps=0
  for rg,val in zip(REG,args):u.reg_write(rg,val)
  sp=STACK+0xff00
  for i,val in enumerate(stackwords):w(sp+4*i,val)
  u.reg_write(UC_ARM_REG_SP,sp);u.reg_write(UC_ARM_REG_LR,END|1)
  try:u.emu_start(pc|1,END,count=12001)
  except UcError as err:raise RuntimeError(hex(u.reg_read(UC_ARM_REG_PC)),[hex(u.reg_read(v))for v in REG],events[-5:])from err
  assert u.reg_read(UC_ARM_REG_PC)==END
  return u.reg_read(REG[0])
 fret=invoke(0x804b01ac,(OBJ,FNAME,0,FNAME),(FNAME,ODATA,OUT))
 assert fret==0 and r(OUT)==FUNC and r(FUNC)==(0x804d45a8 if use_wrong_cpui else 0x804d4468)
 init=invoke(0x804b5718 if use_wrong_cpui else 0x804b5440,(FUNC,OBJ,ODATA,FNAME))
 init_events=list(events)
 if not base_init:
  assert init==0
  return {'factory_return':fret,'factory_vptr':hex(r(FUNC)),'init_return':init,'init_events':init_events,'states':[],'hardware_access':False}
 if not parent_cast:
  assert init==1 and r(FUNC+8)==OBJ and r(FUNC+0x14)==0
  return {'factory_return':fret,'factory_vptr':hex(r(FUNC)),'init_return':init,'retained_parent_cast':0,'call_not_attempted':'actual init returned1 after failed parent cast; later call would dereference null','init_events':init_events,'states':[],'hardware_access':False}
 assert init==1 and r(FUNC+8)==OBJ and r(FUNC+0x14)==OBJ
 out=[]
 for cid,state in requests:
  events.clear();steps=0;recursion_flags=None
  for rg,val in zip(REG,(IO,cid,state,0)):u.reg_write(rg,val)
  u.reg_write(UC_ARM_REG_SP,STACK+0xff00);u.reg_write(UC_ARM_REG_LR,END|1)
  try:u.emu_start(0x80b8bb11,END,count=12001)
  except RuntimeError as error:
   if not allow_budget_exhaustion or str(error)!='bounded original recursion':raise
   out.append({'request':[cid,state],'completed':False,'error':'bounded instruction cap stops original poll','io_reads':io_reads,'io_writes':io_writes,'writes_before_stop':[event for event in events if event[0]=='fake_pmgr_write'],'instructions':steps})
   break
  assert u.reg_read(UC_ARM_REG_PC)==END
  change_flags=u.reg_read(REG[0])
  out.append({'request':[cid,state],'counts':{str(k):r(COUNTS+4*k)for k in(29,35,40,41,42,87,88,89,90,91)},'active_ids':[k for k in rows if r(ACTIVE+4*(k//32))&(1<<(k%32))], 'pending_gate_bits':[r(OBJ+0x578+4*i)for i in range(3)],'dirty_gate_bits':[r(OBJ+0x584+4*i)for i in range(3)],'events':list(events),'recursion_flags':recursion_flags,'return':change_flags,'instructions':steps})
 return {'factory_return':fret,'factory_vptr':hex(r(FUNC)),'init_return':init,'init_events':init_events,'states':out,'synthetic_options':{'clock_word_override':clock_word_override,'initial_reg_seed':initial_reg_seed,'auto_ack':auto_ack,'off_zero_indices':list(off_zero_indices)},'hardware_access':False}

def verify(kernelcache=None,device_tree=None,registry=False):
 load_sources(kernelcache,device_tree)
 if registry:
  spec=importlib.util.spec_from_file_location('registry',Path(__file__).resolve().with_name('emulate-pmgr-registry.py'))
  module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
  return module.verify_with_sources(e,data,adt,table,MIN,MAX,rows)
 cases=[experiment([(89,1),(87,1),(89,0),(87,0)]),experiment([(89,1),(89,1),(89,0)]),experiment([(87,1),(87,0)])]
 assert [x[1]for x in cases[0]['states'][0]['events']if x[0]=='fake_pmgr_write']==['0x48','0x4c','0x50','0x54','0x9c','0x1080','0x1088']
 assert all(x['return']==0 for c in cases for x in c['states'])
 failed=experiment([(89,1)],base_init=False);assert not failed['states']
 cast_failed=experiment([(89,1)],parent_cast=False)
 wrong=experiment([(89,1)],use_wrong_cpui=True);assert wrong['states'][0]['return']==0xe00002c7 and not[e for e in wrong['states'][0]['events']if e[0]=='fake_pmgr_write']
 return {'kernelcache_sha256':e.KC_SHA256,'adt_sha256':e.ADT_SHA256,'scope':'Actual clkG factory+base init+metacast, S5LIO cached wrapper, Performance CG wrapper+actual startup action+native graph+PMGR; fake registry identity/metacast/reference/CG/PMGR only. cpuI is the independent wrong-type counterproof.', 'cases':cases,'base_init_failure':failed,'parent_cast_failure':cast_failed,'wrong_cpui_control':wrong}
if __name__=='__main__':
 import argparse
 parser=argparse.ArgumentParser(description=__doc__)
 parser.add_argument('--kernelcache',type=Path)
 parser.add_argument('--device-tree',type=Path)
 parser.add_argument('--registry',action='store_true',help='execute source publisher/lookup with an explicit fake registry')
 args=parser.parse_args()
 print(json.dumps(verify(args.kernelcache,args.device_tree,registry=args.registry),indent=2))
