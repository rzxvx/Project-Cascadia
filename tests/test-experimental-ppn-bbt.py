#!/usr/bin/env python3
"""Actual C transitions with separately modelled persisted copies and CRC oracle.
No file-backed NAND, firmware execution, device APIs, signals or callbacks.
"""
import ctypes as C,random,struct,sys
from pathlib import Path
HERE=Path(__file__).resolve().parents[1]/'experiments/p105-ppn'
from experimental_helpers import native_stage
_stage,_library=native_stage(HERE,['ppn_bbt_tx.c'],shared=True)
U8=C.c_uint8;U32=C.c_uint32;U64=C.c_uint64;B=C.c_bool
class Slot(C.Structure):_fields_=[('physical_id',U64),('alias_domain',U64)]
class Config(C.Structure):_fields_=[(s,U64)for s in('media_id','owner_id','lease_epoch')]+[('blocks',U32),('slots',Slot*2),('ownership_proven',B)]
class Snapshot(C.Structure):_fields_=[(s,U64)for s in('media_id','owner_id','record_epoch','generation','physical_slot','alias_domain')]+[(s,U32)for s in('blocks','operation','pending_block')]+[('bad',U8*8192)]
class Receipt(C.Structure):_fields_=[(s,U64)for s in('media_id','owner_id','lease_epoch','generation','physical_slot','alias_domain')]+[(s,U32)for s in('body_crc','manifest_crc','stage')]+[('durable',B),('quiesced',B)]
class Copy(C.Structure):_fields_=[('snapshot',Snapshot),('body_crc',U32),('manifest_crc',U32),('body_present',B),('manifest_present',B),('complete_read',B),('absent_manifest_erased',B),('empty_slot_erased',B)]
class Tx(C.Structure):_fields_=[('config',Config),('committed',Snapshot),('prepared',Snapshot)]+[(s,U32)for s in('committed_crc','prepared_crc','active_slot','target_slot','phase','after_commit')]
lib=C.CDLL(str(_library))
for n,args,ret in [('create_empty',[C.POINTER(Tx),C.POINTER(Config),C.POINTER(Receipt)],C.c_int),('recover',[C.POINTER(Tx),C.POINTER(Config),C.POINTER(Copy)],C.c_int),
 ('checkpoint',[C.POINTER(Tx),C.POINTER(Receipt)],C.c_int),('begin',[C.POINTER(Tx),U32,U32,C.POINTER(Receipt)],C.c_int),
 ('finish',[C.POINTER(Tx),B,B,B,B,C.POINTER(Receipt)],C.c_int),('confirm',[C.POINTER(Tx),C.POINTER(Receipt)],C.c_int),
 ('get_manifest',[C.POINTER(Tx),C.POINTER(U32)],C.c_int),('snapshot_crc',[C.POINTER(Snapshot)],U32),('manifest_crc',[C.POINTER(Snapshot),U32],U32)]:
 f=getattr(lib,'ppn_bbt_'+n);f.argtypes=args;f.restype=ret
calls=0
def genesis_receipts(c):
 return (Receipt*2)(*[Receipt(c.media_id,c.owner_id,c.lease_epoch,0,s.physical_id,s.alias_domain,0,0,0,True,True)for s in c.slots])
def invoke(name,*args):
 global calls
 if name=='create_empty' and len(args)==2:
  args=(*args,genesis_receipts(args[1])if isinstance(args[1],Config)else None)
 calls+=1;return getattr(lib,'ppn_bbt_'+name)(*[C.byref(x)if isinstance(x,C.Structure)else x for x in args])
def clone(x):return type(x).from_buffer_copy(bytes(x))
def cfg(n=256):return Config(11,22,101,n,(Slot*2)(Slot(900,9000),Slot(901,9001)),True)
# Table CRC uses explicit canonical bytes; it does not reuse C bit-loop code.
table=[]
for i in range(256):
 x=i
 for _ in range(8):x=(x>>1)^ (0x82f63b78 if x&1 else 0)
 table.append(x)
def crc(data):
 v=0xffffffff
 for byte in data:v=table[(v^byte)&255]^(v>>8)
 return v^0xffffffff
def body_crc(s):
 return crc(struct.pack('<7Q3I',0x314254424e505050,s.media_id,s.owner_id,s.record_epoch,s.generation,s.physical_slot,s.alias_domain,s.blocks,s.operation,s.pending_block)+bytes(s.bad))
def manifest_crc(s,bcrc):
 return crc(struct.pack('<7QI',0x3154494d4d4f4350,s.media_id,s.owner_id,s.record_epoch,s.generation,s.physical_slot,s.alias_domain,bcrc))
def erased(t):
 slot=t.config.slots[t.active_slot^1]
 return Receipt(t.config.media_id,t.config.owner_id,t.config.lease_epoch,t.committed.generation+1,slot.physical_id,slot.alias_domain,0,0,0,True,True)
def ack(t,stage):
 p=t.prepared
 return Receipt(t.config.media_id,t.config.owner_id,t.config.lease_epoch,p.generation,p.physical_slot,p.alias_domain,t.prepared_crc,manifest_crc(p,t.prepared_crc)if stage==2 else 0,stage,True,True)
def recovery(c,copies,gen,phase):
 t=Tx();assert invoke('recover',t,c,copies)==0
 assert t.committed.generation==gen and t.phase==phase
 return t
def publish(t,copies,expected_before,expected_after):
 old=bytes(t.committed);blank=Copy();blank.complete_read=True;blank.empty_slot_erased=True;copies[t.target_slot]=blank
 if expected_before is not None:recovery(t.config,copies,*expected_before)
 copy=Copy();copy.complete_read=True;copy.absent_manifest_erased=True;copy.snapshot=clone(t.prepared)
 copy.body_crc=body_crc(copy.snapshot);copy.body_present=True
 assert copy.body_crc==t.prepared_crc==invoke('snapshot_crc',copy.snapshot)
 copies[t.target_slot]=copy
 m=U32(0xa5a5);assert invoke('get_manifest',t,C.byref(m))<0 and m.value==0xa5a5
 if expected_before is not None:recovery(t.config,copies,*expected_before)
 assert invoke('confirm',t,ack(t,1))==0 and t.phase==4
 assert bytes(t.committed)==old
 assert invoke('get_manifest',t,C.byref(m))==0
 assert m.value==manifest_crc(t.prepared,t.prepared_crc)==invoke('manifest_crc',t.prepared,t.prepared_crc)
 if expected_before is not None:recovery(t.config,copies,*expected_before)
 copies[t.target_slot].manifest_crc=m.value;copies[t.target_slot].manifest_present=True;copies[t.target_slot].absent_manifest_erased=False
 # Marker persisted but receipt lost: recovery sees new copy, runtime not yet.
 recovery(t.config,copies,*expected_after);assert bytes(t.committed)==old
 assert invoke('confirm',t,ack(t,2))==0
 assert t.committed.generation==expected_after[0]
def genesis(n=256):
 t=Tx();c=cfg(n);copies=(Copy*2)()
 for copy in copies:copy.complete_read=True;copy.empty_slot_erased=True
 assert invoke('create_empty',t,c)==0 and t.phase==1
 assert invoke('begin',t,1,0,erased(t))<0
 assert invoke('checkpoint',t,erased(t))==0 and t.phase==3
 publish(t,copies,None,(1,2));assert t.phase==2
 return t,copies
t,copies=genesis()
# Each operation is preceded by persistent intent; readonly recovery neither
# invents success nor turns a pending block into a physical bad block.
for op in(1,2):
 t,copies=genesis();assert invoke('begin',t,op,3,erased(t))==0
 publish(t,copies,(1,2),(2,6));assert t.phase==5 and t.committed.operation==op
 reboot=recovery(t.config,copies,2,6);assert not any(reboot.committed.bad)
 assert invoke('finish',t,True,True,True,False,erased(t))==0
 publish(t,copies,(2,6),(3,2));assert t.phase==2 and t.committed.pending_block==0xffffffff
 t,copies=genesis();assert invoke('begin',t,op,3,erased(t))==0;publish(t,copies,(1,2),(2,6))
 assert invoke('finish',t,False,False,True,True,erased(t))==0
 publish(t,copies,(2,6),(3,2));assert t.committed.bad[0]==8
 assert invoke('begin',t,op,3,erased(t))<0
# Wrong confirmations and immutable prepared/committed snapshots.
for stage in(1,2):
 for field in('media_id','owner_id','lease_epoch','generation','physical_slot','alias_domain','body_crc','manifest_crc','stage','durable','quiesced'):
  t,copies=genesis();assert invoke('checkpoint',t,erased(t))==0
  if stage==2:assert invoke('confirm',t,ack(t,1))==0
  before=bytes(t.committed);r=ack(t,stage)
  setattr(r,field,False if field in('durable','quiesced')else getattr(r,field)^1)
  assert invoke('confirm',t,r)<0 and t.phase==6 and bytes(t.committed)==before
for field in('media_id','owner_id','lease_epoch','generation','physical_slot','alias_domain','body_crc','manifest_crc','stage','durable','quiesced'):
 t,copies=genesis();r=erased(t);setattr(r,field,False if field in('durable','quiesced')else getattr(r,field)^1)
 before=bytes(t.committed);assert invoke('checkpoint',t,r)<0 and t.phase==6 and bytes(t.committed)==before
for prepared in(False,True):
 t,copies=genesis()
 if prepared:
  assert invoke('checkpoint',t,erased(t))==0;t.prepared.bad[0]^=1
  assert invoke('confirm',t,ack(t,1))<0 and t.phase==6
 else:
  t.committed.bad[0]^=1;assert invoke('checkpoint',t,erased(t))<0 and t.phase==6
# Unknown completion / failed quiescence retains the committed intent.
for success,durable,quiet,retired in[(True,False,True,False),(True,True,False,False),(False,False,True,False),(False,False,False,True),(True,True,True,True)]:
 t,copies=genesis();assert invoke('begin',t,1,4,erased(t))==0;publish(t,copies,(1,2),(2,6))
 before=bytes(t.committed);assert invoke('finish',t,success,durable,quiet,retired,erased(t))<0
 assert t.phase==6 and bytes(t.committed)==before;recovery(t.config,copies,2,6)
# Invalid/foreign identity, alias, shape and generation recovery boundaries.
for name in('ownership_proven','blocks','media_id','owner_id','lease_epoch','alias','physical'):
 c=cfg();t=Tx()
 if name=='alias':c.slots[1].alias_domain=c.slots[0].alias_domain
 elif name=='physical':c.slots[1].physical_id=c.slots[0].physical_id
 elif name=='blocks':c.blocks=65537
 else:setattr(c,name,0)
 assert invoke('create_empty',t,c)<0 and t.phase==6
for field in('media_id','owner_id','record_epoch','physical_slot','alias_domain','blocks','operation','pending_block','bad'):
 t,copies=genesis(65);r=copies[0]
 if field=='bad':r.snapshot.bad[8]=2
 elif field=='operation':r.snapshot.operation=3
 elif field=='pending_block':r.snapshot.pending_block=65
 elif field=='record_epoch':r.snapshot.record_epoch=0
 else:setattr(r.snapshot,field,getattr(r.snapshot,field)^1)
 r.body_crc=body_crc(r.snapshot);r.manifest_crc=manifest_crc(r.snapshot,r.body_crc)
 out=Tx();assert invoke('recover',out,t.config,copies)<0 and out.phase==6
t,copies=genesis();assert invoke('checkpoint',t,erased(t))==0;publish(t,copies,(1,2),(2,2))
copies[0].snapshot.generation=2;copies[0].body_crc=body_crc(copies[0].snapshot);copies[0].manifest_crc=manifest_crc(copies[0].snapshot,copies[0].body_crc)
out=Tx();assert invoke('recover',out,t.config,copies)<0 and out.phase==6
for damage in('body','manifest','presence'):
 t,copies=genesis();assert invoke('checkpoint',t,erased(t))==0;publish(t,copies,(1,2),(2,2))
 if damage=='body':copies[1].body_crc^=1
 elif damage=='manifest':copies[1].manifest_crc^=1
 else:copies[1].manifest_present=False
 out=Tx();assert invoke('recover',out,t.config,copies)<0 and out.phase==6
t,copies=genesis();copies[0].snapshot.generation=0xffffffffffffffff;copies[0].body_crc=body_crc(copies[0].snapshot);copies[0].manifest_crc=manifest_crc(copies[0].snapshot,copies[0].body_crc)
out=recovery(t.config,copies,0xffffffffffffffff,6)
assert invoke('checkpoint',out,erased(out))<0
out.phase=2;assert invoke('checkpoint',out,erased(out))<0 and out.phase==6
# Epoch changes invalidate runtime acknowledgments, not authenticated old media.
t,copies=genesis();c=clone(t.config);c.lease_epoch+=1;fresh=recovery(c,copies,1,2)
assert invoke('checkpoint',fresh,erased(fresh))==0;r=ack(fresh,1);r.lease_epoch-=1
assert invoke('confirm',fresh,r)<0 and fresh.phase==6
# Independent randomized bad-block oracle and active-intent crash checkpoints.
rng=random.Random(0xBB7);t,copies=genesis();bad=set()
for i in range(128):
 block=rng.choice([b for b in range(256)if b not in bad]);op=rng.choice([1,2]);oldgen=t.committed.generation
 assert invoke('begin',t,op,block,erased(t))==0;publish(t,copies,(oldgen,2),(oldgen+1,6))
 retiring=bool(rng.randrange(2))
 assert invoke('finish',t,not retiring,not retiring,True,retiring,erased(t))==0
 publish(t,copies,(oldgen+1,6),(oldgen+2,2))
 if retiring:bad.add(block)
 want=bytearray(8192)
 for b in bad:want[b//8]|=1<<(b%8)
 assert bytes(t.committed.bad)==want and t.phase==2
# A damaged once-committed intent may never disappear into an older READY copy.
for damage in ('body_crc','manifest_crc','complete_read','missing_marker'):
 t,copies=genesis();assert invoke('begin',t,1,4,erased(t))==0;publish(t,copies,(1,2),(2,6))
 if damage=='body_crc':copies[1].body_crc^=1
 elif damage=='manifest_crc':copies[1].manifest_crc^=1
 elif damage=='complete_read':copies[1].complete_read=False
 else:copies[1].manifest_present=False
 out=Tx();assert invoke('recover',out,t.config,copies)<0 and out.phase==6
# Only a complete read proving an erased marker allows ignoring a torn body.
t,copies=genesis();torn=Copy();torn.complete_read=True;torn.body_present=True;torn.absent_manifest_erased=True
torn.body_crc=123;copies[1]=torn;recovery(t.config,copies,1,2)
copies[1].absent_manifest_erased=False;out=Tx();assert invoke('recover',out,t.config,copies)<0 and out.phase==6
# Workspace maximum is a declared bound, not assumed hardware geometry.
t,copies=genesis(65536);assert invoke('begin',t,2,65535,erased(t))==0;publish(t,copies,(1,2),(2,6))
assert invoke('finish',t,False,False,True,True,erased(t))==0;publish(t,copies,(2,6),(3,2))
assert t.committed.bad[8191]==128 and sum(t.committed.bad)==128
empty=(Copy*2)();out=Tx();assert invoke('recover',out,cfg(),empty)<0 and out.phase==6
assert invoke('create_empty',None,cfg())<0
# Root-review regression: no initializer may destroy an active or latched workspace.
import errno
for phase in (1,2,3,4,5,6):
 t,copies=genesis()
 if phase in (3,4,5,6):
  assert invoke('begin',t,1,7,erased(t))==0
  if phase in (4,5,6):assert invoke('confirm',t,ack(t,1))==0
  if phase in (5,6):assert invoke('confirm',t,ack(t,2))==0
  if phase==6:assert invoke('finish',t,True,False,True,False,erased(t))<0
 if phase==1:
  t=Tx();assert invoke('create_empty',t,cfg())==0
 assert t.phase==phase
 old=bytes(t);c=clone(t.config)
 assert invoke('create_empty',t,c)==-errno.EALREADY and bytes(t)==old
 assert invoke('recover',t,c,copies)==-errno.EALREADY and bytes(t)==old
 # Passing the workspace's own config cannot bypass the fresh-only guard.
 assert invoke('create_empty',t,t.config)==-errno.EALREADY and bytes(t)==old
 assert invoke('recover',t,t.config,copies)==-errno.EALREADY and bytes(t)==old
# Phase0 alone is insufficient when other retained bytes exist.
t=Tx();t.committed.bad[0]=1;old=bytes(t)
assert invoke('create_empty',t,cfg())==-errno.EALREADY and bytes(t)==old
assert invoke('recover',t,cfg(),(Copy*2)())==-errno.EALREADY and bytes(t)==old
# Fresh input/output overlaps are rejected before any memset or input dereference.
t=Tx();old=bytes(t);cptr=C.cast(C.byref(t),C.POINTER(Config))
assert invoke('create_empty',t,cptr)<0 and bytes(t)==old
copyptr=C.cast(C.byref(t),C.POINTER(Copy))
assert invoke('recover',t,cfg(),copyptr)<0 and bytes(t)==old
# A corrupt prepared snapshot must latch readonly on manifest retrieval too.
t,copies=genesis();assert invoke('checkpoint',t,erased(t))==0;assert invoke('confirm',t,ack(t,1))==0
published=bytes(t.committed);t.prepared.bad[0]^=1;m=U32(0xa5a5)
assert invoke('get_manifest',t,C.byref(m))<0 and m.value==0xa5a5 and t.phase==6 and bytes(t.committed)==published
t,copies=genesis();assert invoke('checkpoint',t,erased(t))==0;assert invoke('confirm',t,ack(t,1))==0
old=bytes(t);overlapping=C.cast(C.byref(t,Tx.prepared_crc.offset),C.POINTER(U32))
assert invoke('get_manifest',t,overlapping)<0 and bytes(t)==old
# V3 Genesis: a new RAM allocation is not evidence that existing media are blank.
for slot in(0,1):
 for field in('media_id','owner_id','lease_epoch','generation','physical_slot','alias_domain','body_crc','manifest_crc','stage','durable','quiesced'):
  c=cfg();receipts=genesis_receipts(c)
  setattr(receipts[slot],field,False if field in('durable','quiesced')else getattr(receipts[slot],field)^1)
  fresh=Tx();assert invoke('create_empty',fresh,c,receipts)<0 and fresh.phase==6
  assert invoke('begin',fresh,1,0,erased(fresh))<0
fresh=Tx();assert invoke('create_empty',fresh,cfg(),None)<0 and fresh.phase==6
# A restarted lease observes an existing durable intent in one slot. It may
# recover readonly, but cannot bootstrap from one blank plus one nonblank slot.
t,copies=genesis();assert invoke('begin',t,2,9,erased(t))==0;publish(t,copies,(1,2),(2,6))
# Old alternate slot was erased before the interrupted completion checkpoint.
blank=Copy();blank.complete_read=True;blank.empty_slot_erased=True;copies[0]=blank
c=clone(t.config);c.lease_epoch+=1;receipts=genesis_receipts(c)
for i,copy in enumerate(copies):
 receipts[i].durable=bool(copy.complete_read and copy.empty_slot_erased and not copy.body_present and not copy.manifest_present)
fresh=Tx();assert invoke('create_empty',fresh,c,receipts)<0 and fresh.phase==6
restored=recovery(c,copies,2,6);assert restored.committed.pending_block==9
# Opaque old/new confirmation generations must remain distinct from Genesis0.
for stage in(1,2):
 t,copies=genesis();assert invoke('checkpoint',t,erased(t))==0
 if stage==2:assert invoke('confirm',t,ack(t,1))==0
 r=ack(t,stage);r.generation=0;before=bytes(t.committed)
 assert invoke('confirm',t,r)<0 and t.phase==6 and bytes(t.committed)==before
# Genesis input receipts themselves must not alias immutable configuration/output.
fresh=Tx();old=bytes(fresh);alias=C.cast(C.byref(fresh),C.POINTER(Receipt))
assert invoke('create_empty',fresh,cfg(),alias)<0 and bytes(fresh)==old
print(f'PASS: {calls} real C calls; independent persisted-copy/CRC/badblock oracle, all transaction powercut points and identity/durability gates')
