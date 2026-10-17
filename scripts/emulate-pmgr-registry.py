"""Actual original parent recursion+notification+deferred register apply; all MMIO fake."""
import hashlib,importlib.util,json,struct
from pathlib import Path
from unicorn import *
from unicorn.arm_const import *
e=data=adt=table=None
MIN=MAX=None
rows={}
OBJ,ACTIVE,COUNTS,COMPARE,INDEX,TABLE=0x10000000,0x10002000,0x10003000,0x10004000,0x10005000,0x10008000
IOFN,PHKEY=0x10011900,0x10011c00
PNODE,ANODE,PDATA,PB,PSYM,MARKER,MATCH=0x10010000,0x10010800,0x10011000,0x10011200,0x10011400,0x10011600,0x10011800
IO,FUNC,ODATA,FNAME,FBYTES,OUT,CG,VT=0x10001000,0x10001100,0x10001200,0x10001400,0x10001500,0x10001600,0x10017000,0x10017400
REGS=0x10018000
STACK,TRAP,END=0x20000000,0x30000000,0x30000ffc
REG=[UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3]
def experiment(requests, clock_word_override=None, initial_reg_seed=0x24600000,
               auto_ack=True, off_zero_indices=(), allow_budget_exhaustion=False, base_init=True, use_wrong_cpui=False, parent_cast=True, phandle=0x10000000, registered=True, phandle_present=True):
 if not 1<=len(requests)<=64 or any(cid not in rows or state not in(0,1)for cid,state in requests):raise ValueError('bounded known ID requests required')
 u=Uc(UC_ARCH_ARM,UC_MODE_THUMB);lo,hi=0x80b8b000,0x80b92000;u.mem_map(lo,hi-lo);u.mem_write(lo,data[e.address_offset(data,lo):e.address_offset(data,hi-1)+1])
 for a,size in((OBJ,0x20000),(STACK,0x10000),(TRAP,0x1000)):u.mem_map(a,size)
 def w(a,v):u.mem_write(a,struct.pack('<I',v))
 def r(a):return struct.unpack('<I',u.mem_read(a,4))[0]
 guardptr=e.pic_literal_pointer(data,0x80b8c980,0x80b8c986,0);guard=struct.unpack_from('<I',data,e.address_offset(data,guardptr))[0]
 page=guard&~0xfff;u.mem_map(page,0x1000);w(guard,0x12345678)
 w(OBJ,0x80b90938);
 # +448 is real inherited notification helper. With synthetic null observer+530 it returns0.
 for page in(0x80398000,0x802c5000,0x802c3000):u.mem_map(page,0x1000);u.mem_write(page,data[e.address_offset(data,page):e.address_offset(data,page+0xfff)+1])
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
 w(PNODE,0x804d2bb8);w(ANODE,VT+0x200);w(VT+0x200+0x80,(TRAP+16)|1);w(PDATA,VT);w(OBJ+0x1ac,PNODE);w(OBJ+0x208,0);w(OBJ+0x540,0);w(PB,phandle)
 for page in{r(0x804d2bb8+0x80)&~0xfff,r(0x80b90938+0x58)&~0xfff,r(0x80b90938+0x160)&~0xfff}:
  if page not in(0x802c5000,):u.mem_map(page,0x1000);u.mem_write(page,data[e.address_offset(data,page):e.address_offset(data,page+0xfff)+1])
 w(ODATA,VT);w(FNAME,VT);w(VT+0x68,(TRAP+4)|1);w(VT+0x4c,(TRAP+12)|1);w(VT+0x10,(TRAP+8)|1);w(VT+0x14,(TRAP+8)|1);w(PSYM,VT);w(MATCH,VT);w(IOFN,VT);w(PHKEY,VT);w(e.symbol_address(data,"_gIODTPHandleKey"),PHKEY)
 w(CG,VT+0x100);w(VT+0x100+0x70,TRAP|1);w(OBJ+0x1b4,CG)
 # Exact property bytes, matched by real factory. The synthetic identity
 # of IOFunctionParent is separate from this causal source dispatch.
 w(FBYTES,0x10000000);w(FBYTES+4,0x63707549 if use_wrong_cpui else 0x636c6b47)
 g=e.pic_literal_pointer(data,0x804b01b8,0x804b01be,0);w(r(g),IOFN)
 assert e.pic_literal_pointer(data,0x804b0288,0x804b028a,0)+8==0x804d4468
 events=[];steps=0;io_reads=0;io_writes=0;recursion_flags=None;registry={};names={};match_key=None
 assert r(0x80b90938+0x3d8)==0x804b1d11
 assert e.pic_literal_pointer(data,0x80b8bc80,0x80b8bc82,1)+8==0x80b90938
 def hook(m,a,n,_):
  nonlocal steps,recursion_flags,match_key
  steps+=1
  if steps>12000:raise RuntimeError('bounded original recursion')
  if a==0x804b3204:recursion_flags=m.reg_read(REG[0])
  if a in(0x80b8c0b8,0x804b1d2c,0x80b8c168,0x804b01ac):events.append(['actual_source_stage',hex(a)])
  if a==END:m.emu_stop();return
  x,y,z,q=[m.reg_read(v)for v in REG]
  if a==0x804c9e24:
   assert z==0x804ca341 and y==33
   text='IOFunctionParent%08X'%q;events.append(['fake_snprintf',text,q]);m.mem_write(x,text.encode()+b'\0');m.reg_write(REG[0],len(text))
  elif a==0x804c9a54:
   text=bytes(m.mem_read(x,33)).split(b'\0')[0].decode();assert text.startswith('IOFunctionParent')
   names[PSYM]=text;events.append(['fake_OSSymbol_intern',text]);m.reg_write(REG[0],PSYM)
  elif a==0x804c99b4:
   assert x==0 and y==0;events.append(['fake_empty_OSData']);w(MARKER,VT);m.reg_write(REG[0],MARKER)
  elif a==(r(0x804d2bb8+0x80)&~1):
   assert x==PNODE and y==PHKEY;events.append(['fake_PMGR_phandle_property',phandle_present]);m.reg_write(REG[0],PDATA if phandle_present else 0)
  elif a==(r(0x80b90938+0x58)&~1):
   assert x==OBJ and y==PSYM and z==MARKER
   registry[names[y]]=OBJ;events.append(['fake_registry_publish_on_actual_Performance',names[y]]);m.reg_write(REG[0],1)
  elif a==(r(0x80b90938+0x160)&~1):
   assert x==OBJ and y==2;events.append(['fake_registerService',y]);m.reg_write(REG[0],0)
  elif a==0x804c9af4:
   assert x==PSYM and y==MARKER and z==0;match_key=names[x];events.append(['fake_propertyMatching',match_key]);m.reg_write(REG[0],MATCH)
  elif a==0x804c9aa4:
   assert x==MATCH and y==0;found=registry.get(match_key,0);events.append(['fake_waitForService',match_key,hex(found)])
   if not found:raise LookupError('fake_waitForService has no matching parent; no fabricated service')
   m.reg_write(REG[0],found)
  elif a==TRAP+12:
   assert x==ODATA;events.append(['fake_OSData_length',8]);m.reg_write(REG[0],8)
  elif a==TRAP+16:
   assert x==ANODE and y==FNAME;events.append(['fake_armio_clock_function_property']);m.reg_write(REG[0],ODATA)
  elif a==TRAP:
   assert x==CG and y==0x804b3141 and z in rows and q in(0,1)
   assert r(m.reg_read(UC_ARM_REG_SP))==0 and r(m.reg_read(UC_ARM_REG_SP)+4)==0
   events.append(['fake_CommandGate_runAction',hex(y),z,q]);m.reg_write(REG[0],OBJ);m.reg_write(REG[1],z);m.reg_write(REG[2],q);m.reg_write(REG[3],0);m.reg_write(UC_ARM_REG_PC,y);return
  elif a==TRAP+4:
   assert x in(ODATA,PDATA);events.append(['fake_OSData_bytes',hex(x)]);m.reg_write(REG[0],FBYTES if x==ODATA else PB)
  elif a==TRAP+8:
   assert x in(ODATA,FNAME,MARKER,PSYM,IOFN,PHKEY);events.append(['fake_retain_or_release',hex(x)])
  elif a==0x804c9a34:
   assert x==0x18;events.append(['fake_allocate',x]);m.reg_write(REG[0],FUNC)
  elif a==0x804c9a04:
   assert x==FUNC;events.append(['fake_OSObject_constructor'])
  elif a==0x804c9b94:
   events.append(['fake_meta_release'])
  elif a in(0x804c9884,0x802c3c50):
   assert x in(0,ODATA,PDATA,OBJ,PNODE);events.append(['fake_metacast',hex(x),hex(y)]);m.reg_write(REG[0],0 if x==OBJ and not parent_cast else x)
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
 # Actual BasePerformance startup retains its metacast PMGR provider.
 w(OBJ+0x1ac,0);u.reg_write(UC_ARM_REG_R4,OBJ);u.reg_write(UC_ARM_REG_R6,PNODE)
 u.emu_start(0x804af6eb,0x804af702,count=100);assert r(OBJ+0x1ac)==PNODE
 if registered:
  # Execute the actual S5L startup virtual+3d8 caller block, not a guessed call.
  steps=0;u.reg_write(UC_ARM_REG_R4,OBJ);u.reg_write(UC_ARM_REG_SP,STACK+0xff00)
  u.emu_start(0x80b8c0b9,0x80b8c0c4,count=12001);assert u.reg_read(UC_ARM_REG_PC)==0x80b8c0c4
  regret=u.reg_read(REG[0])
  if phandle_present:assert registry.get('IOFunctionParent%08X'%phandle)==OBJ
  else:assert not registry
 else:regret=None
 try:created=invoke(0x804ab31c,(ANODE,FNAME,0,0))
 except LookupError as err:return {'published_keys':list(registry),'init_events':events,'registration_return':regret,'blocked':str(err),'states':[],'hardware_access':False}
 assert created==FUNC and r(FUNC)==(0x804d45a8 if use_wrong_cpui else 0x804d4468)
 assert r(FUNC+8)==OBJ and r(FUNC+0x14)==OBJ
 fret=0;init=1;init_events=list(events)
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
 return {'factory_return':fret,'factory_vptr':hex(r(FUNC)),'init_return':init,'init_events':init_events,'registration_return':regret,'published_keys':list(registry),'states':out,'synthetic_options':{'clock_word_override':clock_word_override,'initial_reg_seed':initial_reg_seed,'auto_ack':auto_ack,'off_zero_indices':list(off_zero_indices)},'hardware_access':False}


def verify():
 positive=experiment([(89,1),(87,1),(89,0),(87,0)])
 assert [e[1]for e in positive['states'][0]['events']if e[0]=='fake_pmgr_write']==['0x48','0x4c','0x50','0x54','0x9c','0x1080','0x1088']
 assert [e[1]for e in positive['init_events']if e[0]=='actual_source_stage']==['0x80b8c0b8','0x804b1d2c','0x80b8c168','0x804b01ac']
 assert positive['published_keys']==['IOFunctionParent10000000']
 assert all(s['return']==0 for s in positive['states'])
 return {'kernelcache_sha256':e.KC_SHA256,'adt_sha256':e.ADT_SHA256,'scope':'Actual Performance +3d8 publisher through real registration API, actual generic lookup, S5LPerformance factory override/fallback, actual factory+base init+typedclkG. Fake registry/IOKit providers and MMIO, no hardware.', 'positive':positive,'negative': [experiment([(89,1)],phandle=0x11000000),experiment([(89,1)],registered=False),experiment([(89,1)],phandle_present=False)]}
def verify_with_sources(helper, image, tree, parsed_table, minimum, maximum, index):
 global e,data,adt,table,MIN,MAX,rows
 if hashlib.sha256(image).hexdigest()!=helper.KC_SHA256 or hashlib.sha256(tree).hexdigest()!=helper.ADT_SHA256:
  raise ValueError('exact pinned P10512H321 inputs required')
 if (minimum,maximum)!=(56,128)or len(parsed_table)!=5104:raise ValueError('unexpected native table/limits')
 # Only a prevalidated parser context from the public entry point is accepted.
 # Source identities are checked again; these are not executable Apple bytes.
 n,end=helper.dtparse.parse_node(tree,0);found=[]
 def walk(node):
  if node['props'].get('name',b'').rstrip(b'\0')==b'pmgr':found.append(node['props'])
  for child in node['children']:walk(child)
 walk(n)
 if end!=len(tree)or len(found)!=1 or found[0]['AAPL,phandle']!=bytes.fromhex('00000010')or found[0]['device-clocks']!=parsed_table:
  raise ValueError('unexpected PMGR identity')
 expected={parsed_table[o+3]:o//44 for o in range(0,len(parsed_table),44)}
 if index!=expected:raise ValueError('unexpected native row index')
 e,data,adt,table,MIN,MAX,rows=helper,image,tree,parsed_table,minimum,maximum,index
 return verify()
