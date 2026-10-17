"""Actual C private client state engine against original port callback oracle."""
import ctypes,json,subprocess,tempfile,sys
from pathlib import Path
P=Path(__file__).resolve().parents[1]/'experiments/p105-usb-client'
sys.path.insert(0,str(Path(__file__).resolve().parent))
from experimental_helpers import native_stage
stage=native_stage(P,['cascadia-external-client.c'],shared=True)
STAGE_PATH=stage[1]
V=ctypes.CFUNCTYPE(ctypes.c_int,ctypes.c_void_p,ctypes.c_uint64)
C=ctypes.CFUNCTYPE(ctypes.c_int,ctypes.c_void_p,ctypes.c_uint64,ctypes.c_uint32)
class Ops(ctypes.Structure):_fields_=[('validate_epoch',V),('parent_clock_mask',C),('phy_client_state',C)]
class Client(ctypes.Structure):
 _fields_=[('ops',ctypes.POINTER(Ops)),('context',ctypes.c_void_p),('connection_epoch',ctypes.c_uint64),('initialized',ctypes.c_bool),('permitted',ctypes.c_bool),('touched',ctypes.c_bool),('fault',ctypes.c_bool),('pending',ctypes.c_bool),('phy_known',ctypes.c_bool),('clock_known',ctypes.c_bool),('parent_mask',ctypes.c_uint32),('raw_clock_mask',ctypes.c_uint32),('phy_state',ctypes.c_int),('target',ctypes.c_int)]
lib=ctypes.CDLL(str(STAGE_PATH));ptr=ctypes.POINTER(Client)
init=lib.cascadia_external_client_init;init.argtypes=[ptr,ctypes.c_uint32]
prepare=lib.cascadia_external_client_prepare;prepare.argtypes=[ptr,ctypes.c_int,ctypes.c_uint64]
complete=lib.cascadia_external_client_complete;complete.argtypes=[ptr,ctypes.c_uint64,ctypes.c_int]
cleanup=lib.cascadia_external_client_cleanup;cleanup.argtypes=[ptr,ctypes.c_uint64,ctypes.c_bool]
class Fake:
 def __init__(self):
  self.client=Client();self.calls=[];self.current_epoch=42;self.fail_index=None;self.callback_error=-5
  self.validate_calls=0;self.invalidate_on_validate=None;self.detach_on_operation=None
  self.check=V(self.check_epoch)
  self.clock=C(lambda _,epoch,mask:self.op('parent-clock-mask',epoch,mask))
  self.phy=C(lambda _,epoch,state:self.op('phy-client-state',epoch,state))
  self.ops=Ops(self.check,self.clock,self.phy)
  assert init(ctypes.byref(self.client),0x30002)==0
  self.client.ops=ctypes.pointer(self.ops);self.client.connection_epoch=42;self.client.permitted=True
 def check_epoch(self,_,epoch):
  self.validate_calls+=1
  if self.validate_calls==self.invalidate_on_validate:self.current_epoch+=1
  return 0 if epoch==self.current_epoch else -116
 def op(self,name,epoch,value):
  if epoch!=self.current_epoch:return -116
  self.calls.append((name,value))
  if len(self.calls)==self.detach_on_operation:self.current_epoch+=1
  return self.callback_error if len(self.calls)==self.fail_index else 0
 def prep(self,state,epoch=42):return prepare(ctypes.byref(self.client),state,epoch)
 def done(self,error=0,epoch=42):return complete(ctypes.byref(self.client),epoch,error)
 def clean(self,quiesced=True,epoch=42):return cleanup(ctypes.byref(self.client),epoch,quiesced)

oracle=json.loads((P/'original-oracle.json').read_text());rows=oracle['experiments']
assert oracle['sha256']=='db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c'
def original(stage,state):
 r=next(x for x in rows if x['stage']==stage and x['state']==state and x['self_device'] and x['return']=='0x0' and all(c['synthetic_return']==0 for c in x['callbacks']))
 return [(c['operation'],int(c['mask'],16) if 'mask' in c else c['state']) for c in r['callbacks'] if not c['operation'].startswith('inherited')]
f=Fake();assert f.prep(1)==0;assert f.calls==original('will',2);assert f.done()==0 and not f.client.pending
assert f.client.phy_state==1 and f.client.parent_mask==0x30002
assert f.prep(2)==0;assert f.done()==0
assert f.calls[2:]==original('will',1)+original('did',1)
assert f.client.phy_state==2 and f.client.parent_mask==0x10002
assert f.prep(0)==0;assert f.done()==0
assert f.calls[-3:]==[('parent-clock-mask',0x30002)]+original('did',0)
assert f.client.phy_state==0 and f.client.parent_mask==0 and not f.client.fault
# Errors at each prepare stage leave a fault and never claim ownership is gone.
for phase in (1,2):
 f=Fake();f.fail_index=phase;assert f.prep(1)==-5 and f.client.fault
 before=list(f.calls);assert f.prep(1)==-5 and f.calls==before
 assert f.clean(False)==-16 and f.calls==before
 f.fail_index=None;assert f.clean()==0 and not f.client.fault
 assert f.client.phy_state==0 and f.client.parent_mask==0
# Controller failure keeps both references, unlike actual firmware's Did callback.
f=Fake();assert f.prep(1)==0;assert f.done(-5)==-5
assert f.client.fault and f.client.phy_state==1 and f.client.parent_mask==0x30002
assert f.calls==original('will',2)
assert f.clean()==0 and f.calls[-2:]==original('did',0)
# A failed cleanup must stop before the later clock-release stage.
f=Fake();assert f.prep(1)==0;f.fail_index=3
assert f.clean()==-5 and f.client.fault and not f.client.phy_known
assert f.calls[-1]==('phy-client-state',10) and f.client.parent_mask==0x30002
f.fail_index=None;assert f.clean()==0 and f.calls[-2:]==original('did',0)
# Standby final clock failure: retained membership, unknown clock disposition.
f=Fake();assert f.prep(1)==0;assert f.done()==0;assert f.prep(2)==0
f.fail_index=5;assert f.done()==-5 and f.client.fault and f.client.phy_state==2
assert not f.client.clock_known;f.fail_index=None;assert f.clean()==0
# Epoch gates before entry and between prepare and complete, no forged success.
f=Fake();assert f.prep(1,41)==-116 and f.calls==[]
assert f.prep(1)==0;f.current_epoch=43;before=list(f.calls)
assert f.done()==-116 and f.calls==before and f.client.pending
f.current_epoch=42;assert f.clean()==0
# Caller state and permissions checks.
f=Fake();f.client.permitted=False;assert f.prep(1)==-13 and f.calls==[]
f.client.permitted=True;f.client.ops=ctypes.POINTER(Ops)();assert f.prep(1)==-19 and f.calls==[]
f=Fake();assert f.done()==-22;assert f.prep(3)==-22;assert f.prep(1)==0
assert f.prep(1)==-16;assert f.clean(False)==-16;assert f.clean()==0
# Positive backend / controller returns are failures, never success counts.
f=Fake();f.callback_error=1;f.fail_index=1;assert f.prep(1)==-5 and f.client.fault
f=Fake();assert f.prep(1)==0;assert f.done(1)==-5 and f.client.fault
# Reinitialization must not discard pending or owned metadata.
f=Fake();assert f.prep(1)==0
before=bytes(f.client);assert init(ctypes.byref(f.client),0x30006)==-114
assert bytes(f.client)==before and f.client.parent_mask==0x30002
assert f.clean()==0;before=bytes(f.client)
assert init(ctypes.byref(f.client),0x30002)==-114 and bytes(f.client)==before
assert init(None,0x30002)==-22
# Epoch invalidation after successful I/O cannot commit known-success state.
for phase in (1,2):
 f=Fake();f.detach_on_operation=phase
 assert f.prep(1)==-116 and f.client.fault and f.client.pending
 assert not (f.client.clock_known if phase==1 else f.client.phy_known)
# Check after prepare and again immediately before complete's success commit.
f=Fake();f.invalidate_on_validate=6
assert f.prep(1)==-116 and f.client.fault and f.client.pending
f=Fake();assert f.prep(1)==0
f.invalidate_on_validate=f.validate_calls+2
assert f.done()==-116 and f.client.fault and f.client.pending
# Epoch change after standby PHY update prevents the final clock operation.
f=Fake();assert f.prep(1)==0;assert f.done()==0;assert f.prep(2)==0
f.detach_on_operation=len(f.calls)+1
assert f.done()==-116 and f.client.fault and not f.client.phy_known
assert f.calls[-1]==('phy-client-state',25)
# Epoch change after final cleanup update cannot clear the fault/pending latch.
f=Fake();assert f.prep(1)==0;f.detach_on_operation=len(f.calls)+2
assert f.clean()==-116 and f.client.fault and f.client.pending
assert f.calls[-1]==('parent-clock-mask',0) and not f.client.clock_known
print('Actual C client core: normal firmware-order comparisons and bounded error/lifetime cases PASS')

stage[0].cleanup()
