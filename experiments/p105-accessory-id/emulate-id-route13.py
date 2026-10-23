from pathlib import Path
import argparse
import importlib.util,hashlib,struct,json
from unicorn import Uc,UC_ARCH_ARM,UC_MODE_THUMB,UC_HOOK_CODE
from unicorn.arm_const import *
parser=argparse.ArgumentParser(description='Bounded13G digital-ID source oracle; syntheticI2C/context, nohardware')
parser.add_argument('kernelcache',type=Path)
parser.add_argument('--source-root',type=Path,default=Path(__file__).resolve().parents[1])
args=parser.parse_args()
s=importlib.util.spec_from_file_location('m',args.source_root/'scripts/usb-phy-plan.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m)
d=args.kernelcache.read_bytes();SHA='8eae16f060a71d4f3790385951588f8198d0f21cdb74146f9750941a3d764125';assert hashlib.sha256(d).hexdigest()==SHA
assert m.symbol_address(d,'_PE_i_can_has_debugger') & ~1 == 0x80388eec
assert struct.unpack_from('<I',d,m.address_offset(d,0x80808030))[0]==0x80388eed
name=m.pic_literal_pointer(d,0x809ee15c,0x809ee160,1);no=m.address_offset(d,name)
assert d[no:d.find(b'\0',no)]==b'AppleTHS7383'
OBJ,VT,ID,P1,P2,OPTS=0x10000000,0x10001000,0x10002000,0x10002100,0x10002104,0x10002200
STACK,TRAP,END=0x20000000,0x30000000,0x30000ffc

def experiment(raw,debugger_permission=False,method3e8=0,byte7ed=0):
 assert len(raw)==6
 mu=Uc(UC_ARCH_ARM,UC_MODE_THUMB)
 for a,b in((0x807f3000,0x8081b000),):
  x,y=[m.address_offset(d,t)for t in(a,b-1)];assert y-x==b-a-1
  mu.mem_map(a,b-a);mu.mem_write(a,d[x:y+1])
 for p,n in((OBJ,0x10000),(STACK,0x10000),(TRAP,0x1000),(0x80412000,0x1000)):mu.mem_map(p,n)
 def put(a,v):mu.mem_write(a,struct.pack('<I',v))
 def get(a):return struct.unpack('<I',mu.mem_read(a,4))[0]
 put(OBJ,VT);put(VT+0x3e8,TRAP|1);mu.mem_write(OBJ+0x7ed,bytes([byte7ed]));mu.mem_write(ID,raw)
 calls=[]
 def hook(mc,pc,size,_):
  if pc==0x8080552c:
   calls.append({'fake':'PE_i_can_has_debugger','input':mc.reg_read(UC_ARM_REG_R0),'result':int(debugger_permission)});out=int(debugger_permission)
  elif pc==TRAP:
   assert mc.reg_read(UC_ARM_REG_R0)==OBJ;calls.append({'fake':'manager slot3e8','result':method3e8});out=method3e8
  else:return
  mc.reg_write(UC_ARM_REG_R0,out);mc.reg_write(UC_ARM_REG_PC,mc.reg_read(UC_ARM_REG_LR))
 mu.hook_add(UC_HOOK_CODE,hook)
 sp=STACK+0xff00
 for r,v in((UC_ARM_REG_R0,ID),(UC_ARM_REG_R1,P1),(UC_ARM_REG_R2,P2),(UC_ARM_REG_SP,sp),(UC_ARM_REG_LR,END|1)):mu.reg_write(r,v)
 mu.emu_start(0x80802f25,END,count=3000);assert mu.reg_read(UC_ARM_REG_PC)==END
 first,second,valid=get(P1),get(P2),mu.reg_read(UC_ARM_REG_R0)
 mu.mem_write(sp+0x22,raw);put(sp+0x34,first);put(sp+0x38,second);put(sp+0x10,valid);put(OPTS,0)
 for r,v in((UC_ARM_REG_R5,OBJ),(UC_ARM_REG_R6,OPTS),(UC_ARM_REG_R0,raw[0]),(UC_ARM_REG_R1,raw[2]),(UC_ARM_REG_SP,sp)):mu.reg_write(r,v)
 mu.emu_start(0x80803577,0x808035ec,count=3000);assert mu.reg_read(UC_ARM_REG_PC)==0x808035ec
 # str at35ea is actual finalflags store; stopboundary includes this store.
 out=get(OPTS)
 return {'digital_id':raw.hex(),'debugger_permission':debugger_permission,'method3e8':method3e8,'prior_byte7ed':byte7ed,'decoder_valid':valid,'selectors':[hex(first),hex(second)],'USBd_low_flags':hex(out),'calls':calls,'hardware_access':False,'camera_adapter_or_authentication_verified':False}
def read_id(response, status=0x80, errors=(0,0)):
 assert len(response)==7 and 0<=status<=255 and len(errors)==2
 mu=Uc(UC_ARCH_ARM,UC_MODE_THUMB)
 lo,hi=0x809eb000,0x809f5000;x,y=[m.address_offset(d,t)for t in(lo,hi-1)]
 assert y-x==hi-lo-1;mu.mem_map(lo,hi-lo);mu.mem_write(lo,d[x:y+1])
 for p,n in((OBJ,0x10000),(STACK,0x10000),(TRAP,0x1000)):mu.mem_map(p,n)
 def put(a,v):mu.mem_write(a,struct.pack('<I',v))
 vptr=m.pic_literal_pointer(d,0x809ee18c,0x809ee18e,1)+8
 assert vptr==0x809f12cc and struct.unpack_from('<I',d,m.address_offset(d,vptr+0x380))[0]==0x809ee42d
 put(OBJ,vptr);provider=OBJ+0x5000;put(OBJ+0x74,provider);put(provider,VT);put(VT+0x410,TRAP|1)
 mu.mem_write(ID,bytes([0xa5])*6);calls=[]
 def hook(mc,pc,size,_):
  if pc==TRAP:
   r0,r1,r2,r3=[mc.reg_read(r)for r in(UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3)]
   i=len(calls);assert r0==provider and i<2
   assert (r1,r3)==[(0x39,7),(0x0c,1)][i]
   calls.append({'mock':'THS provider read','register':hex(r1),'length':r3,'result':errors[i]})
   if not errors[i]:mc.mem_write(r2,response if i==0 else bytes([status]))
   out=errors[i]
  elif pc in(0x80334000,):raise RuntimeError('unproven imported method')
  elif pc==0x809ef5ec:out=0 # log only
  elif pc in(0x8032e3c4,0x80306db8):out=0 # excluded class-name debugging
  elif pc in(0x809ecdf0,):raise RuntimeError('unexpected classifier execution')
  else:return
  mc.reg_write(UC_ARM_REG_R0,out&0xffffffff);mc.reg_write(UC_ARM_REG_PC,mc.reg_read(UC_ARM_REG_LR))
 # Bad marker diagnostics access object virtualf0/1cc. Their synthetic targets are kept separate from I2C.
 put(VT+0xf0,(TRAP+4)|1);put(VT+0x1cc,(TRAP+8)|1)
 # Override only diagnostic virtual slots in a RAM copy of original immutable vtable.
 ramv=OBJ+0x6000;mu.mem_write(ramv,bytes(mu.mem_read(vptr,0x414)));put(OBJ,ramv)
 put(ramv+0xf0,(TRAP+4)|1);put(ramv+0x1cc,(TRAP+8)|1)
 def diagnostics(mc,pc,size,_):
  if pc in(TRAP+4,TRAP+8):mc.reg_write(UC_ARM_REG_R0,0);mc.reg_write(UC_ARM_REG_PC,mc.reg_read(UC_ARM_REG_LR))
 mu.hook_add(UC_HOOK_CODE,hook);mu.hook_add(UC_HOOK_CODE,diagnostics)
 for r,v in((UC_ARM_REG_R0,OBJ),(UC_ARM_REG_R1,ID),(UC_ARM_REG_SP,STACK+0xff00),(UC_ARM_REG_LR,END|1)):mu.reg_write(r,v)
 mu.emu_start(0x809ee42d,END,count=3000);assert mu.reg_read(UC_ARM_REG_PC)==END
 return {'response':response.hex(),'status':hex(status),'provider_errors':list(errors),'read_calls':calls,'return_code':hex(mu.reg_read(UC_ARM_REG_R0)),'digital_id':bytes(mu.mem_read(ID,6)).hex(),'hardware_access':False}

if __name__=='__main__':
 reads=[read_id(bytes.fromhex('05040302010875')),read_id(bytes.fromhex('05040302010875'),status=0),read_id(bytes.fromhex('05040302010874')),read_id(bytes.fromhex('05040302010875'),errors=(-5,0)),read_id(bytes.fromhex('05040302010875'),errors=(0,-5))]
 assert reads[0]['digital_id']=='080102030405' and reads[0]['return_code']=='0x0'
 assert all(x['digital_id']=='a5a5a5a5a5a5' and x['return_code']!='0x0'for x in reads[1:])
 assert [len(x['read_calls'])for x in reads]==[2,2,2,1,2]
 results=[experiment(bytes([b,0,b2,b3,0,0]))for b2 in (0,8,0x20,0x28)for b3 in (0,0x10,0x20,0x70)for b in range(256)]
 results += [experiment(bytes([b,0,0,0,0,0]),debugger_permission=True)for b in range(256)]
 controls=[experiment(bytes([b,0,b2,b3,0,0]),debugger_permission=dbg,method3e8=meth,byte7ed=prior)for b,b2,b3,dbg,meth,prior in[(8,0,0,False,0,0),(8,0,0,False,1,0),(16,0,0,False,1,0),(24,0,0,False,0,1),(0x2a,0,0,False,0,0),(0x2a,0,0,True,0,0),(9,8,0x20,False,0,0),(9,0,0x20,False,0,0)]]
 assert results[8]['selectors']==['0x75736230','0x0'] and results[8]['USBd_low_flags']=='0x5'
 assert results[16]['selectors']==results[8]['selectors'] and results[16]['USBd_low_flags']=='0x2'
 assert results[24]['selectors']==results[8]['selectors'] and results[24]['USBd_low_flags']=='0x1'
 print(json.dumps({'sha256':SHA,'scope':'Actual THS6B extraction, decoder, bounded low USBd flags; fakeI2C and PE_i_can_has_debugger and diagnostic/provider policy; no fullmanager/auth/power','transport_experiments':reads,'experiments':results,'policy_controls':controls},indent=2))
