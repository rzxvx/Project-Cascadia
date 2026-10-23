"""Pinned original AppleAuthCPAID signer method; fake IDbus/OSData/time only."""
from pathlib import Path
import argparse
ap=argparse.ArgumentParser();ap.add_argument('firmware_directory',type=Path);ap.add_argument('--source-root',type=Path,required=True);args=ap.parse_args()
import hashlib,importlib.util,json,struct
from unicorn import Uc,UC_ARCH_ARM,UC_MODE_THUMB,UC_HOOK_CODE
from unicorn.arm_const import *
sp=importlib.util.spec_from_file_location('m',str(args.source_root/'scripts/usb-phy-plan.py'));m=importlib.util.module_from_spec(sp);sp.loader.exec_module(m)
K=(args.firmware_directory/'kernelcache.13G36.macho').read_bytes();SHA='8eae16f060a71d4f3790385951588f8198d0f21cdb74146f9750941a3d764125';assert hashlib.sha256(K).hexdigest()==SHA
OBJ,PROV,PVT,NONCE,OUT,CAP,DATA,DVT,BYTES=0x10000000,0x10001000,0x10002000,0x10003000,0x10004000,0x10005000,0x10006000,0x10007000,0x10008000
STACK,TRAP,END=0x20000000,0x30000000,0x30000ffc
assert struct.unpack_from('<I',K,m.address_offset(K,0x80ec4808+0x340))[0]==0x80ec2b6d

def run(version=2,nonce_len=None,max_tx=64,max_rx=64,cap=128,case='success'):
 if nonce_len is None:nonce_len={2:20,3:32}.get(version,20)
 mu=Uc(UC_ARCH_ARM,UC_MODE_THUMB);lo=0x80ec0000;mu.mem_map(lo,0x8000);off=m.address_offset(K,lo);mu.mem_write(lo,K[off:off+0x8000])
 for p,n in((OBJ,0x10000),(STACK,0x20000),(TRAP,0x1000)):mu.mem_map(p,n)
 def put(a,v):mu.mem_write(a,struct.pack('<I',v&0xffffffff))
 def ptr(a):return struct.unpack('<I',mu.mem_read(a,4))[0]
 put(OBJ,0x80ec4808);put(OBJ+0x54,PROV);put(OBJ+0x58,max_rx);put(OBJ+0x5c,max_tx)
 put(PROV,PVT);put(PVT+0x34c,TRAP|1);put(PVT+0x178,(TRAP+4)|1)
 put(DATA,DVT);put(DVT+0x14,(TRAP+20)|1);put(DVT+0x4c,(TRAP+8)|1);put(DVT+0x60,(TRAP+12)|1);put(DVT+0x68,(TRAP+16)|1)
 put(CAP,cap);mu.mem_write(NONCE,bytes(range(nonce_len)));mu.mem_write(OUT,b'\xcc'*128)
 # Actual stackguard double-indirection is bound to synthetic loader RAM.
 # movw/movt +pc at2b90: 0x1510+PC2b94=40a4; final0xc44+PC3460=40a4.
 put(0x80ec40a4,BYTES+0x400);put(BYTES+0x400,0x12345678)
 transfers=[];mockcalls=[];dbytes=bytearray();readcount=0
 def hook(mc,pc,size,_):
  nonlocal readcount,dbytes
  r0,r1,r2,r3=[mc.reg_read(r)for r in(UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3)]
  if pc==0x80ec1410:assert r0==OBJ;out=version;mockcalls.append('version getter')
  elif pc==0x80ec3a00:put(r2,1000*r0);put(r2+4,0);out=0;mockcalls.append('interval conversion fake')
  elif pc==0x80ec3a30:mu.mem_write(r0,bytes(mu.mem_read(r1,r2)));out=r0
  elif pc==0x80ec38d0:out=0;mockcalls.append('delay fake')
  elif pc==0x80ec3940:assert r0 in(64,128);dbytes=bytearray();out=DATA;mockcalls.append('OSData allocator fake')
  elif pc==TRAP+4:assert r0==PROV and r1==OBJ;out=0;mockcalls.append('retry/continuation provider fake false')
  elif pc==TRAP+8:assert r0==DATA;out=len(dbytes)
  elif pc==TRAP+12:assert r0==DATA;dbytes.extend(bytes(mu.mem_read(r1,r2)));out=1
  elif pc==TRAP+20:assert r0==DATA;out=0;mockcalls.append('OSData release fake')
  elif pc==TRAP+16:assert r0==DATA;mu.mem_write(BYTES,bytes(dbytes));out=BYTES
  elif pc==TRAP:
   assert r0==PROV and 1<=r2<=37
   tx=bytes(mu.mem_read(r1,r2));stack=mc.reg_read(UC_ARM_REG_SP);args=[ptr(stack+4*i)for i in range(6)]
   rxcap=args[0];assert 1<=rxcap<=129
   cmd=tx[0];out=0
   if cmd==0x94:reply=bytes([0x95,0xff,0])
   elif cmd==0x96:reply=bytes([0x97,0xc0])
   elif cmd==0x9a:reply=bytes([0x9b,0])
   elif cmd==0x98:
    assert len(tx)==5;offset=int.from_bytes(tx[1:3],'big');length=int.from_bytes(tx[3:5],'big');assert length+1==rxcap
    reply=bytes([0x99])+bytes((offset+i)&255 for i in range(length));readcount+=1
   else:raise AssertionError(hex(cmd))
   if case=='transport_error' and not transfers:out=0xe00002bc
   if case=='malformed_nonce' and cmd==0x94:reply=bytes([0,0,0])
   if case=='zero_ack' and cmd==0x94:reply=bytes([0x95,0,0])
   if case=='not_ready' and cmd==0x96:reply=bytes([0x97,0x80])
   if case=='busy' and cmd==0x96:reply=bytes([0x97,0])
   if case=='signature_status_error' and cmd==0x9a:reply=bytes([0x9b,1])
   if case=='bad_signature_opcode' and cmd==0x98:reply=bytes([0])+reply[1:]
   assert len(reply)==rxcap
   mu.mem_write(r3,reply);transfers.append({'tx':tx.hex(),'rx':reply.hex(),'capacity':rxcap,'opaque_args':args[1:],'error':hex(out)})
  elif pc==0x80ec39f0:raise AssertionError('stack guard failed')
  else:return
  mc.reg_write(UC_ARM_REG_R0,out&0xffffffff);mc.reg_write(UC_ARM_REG_PC,mc.reg_read(UC_ARM_REG_LR))
 mu.hook_add(UC_HOOK_CODE,hook);initialsp=STACK+0x1ff00;put(initialsp,CAP)
 for r,v in((UC_ARM_REG_R0,OBJ),(UC_ARM_REG_R1,NONCE),(UC_ARM_REG_R2,nonce_len),(UC_ARM_REG_R3,OUT),(UC_ARM_REG_SP,initialsp),(UC_ARM_REG_LR,END|1)):mu.reg_write(r,v)
 try:mu.emu_start(0x80ec2b6d,END,count=30000)
 except Exception as e:raise RuntimeError(f'{version}/{nonce_len}/{max_tx}/{max_rx}/{case} PC={mu.reg_read(UC_ARM_REG_PC):#x} transfers={transfers}')from e
 assert mu.reg_read(UC_ARM_REG_PC)==END,'instruction budget exhausted'
 return {'version':version,'nonce_length':nonce_len,'max_tx':max_tx,'max_rx':max_rx,'input_capacity':cap,'case':case,'return':hex(mu.reg_read(UC_ARM_REG_R0)),'output_size':ptr(CAP),'output':bytes(mu.mem_read(OUT,128)).hex(),'transfers':transfers,'mock_calls':mockcalls}

rows=[run(),run(3),run(3,max_tx=32),run(max_rx=17),run(3,max_rx=17),run(cap=64),run(nonce_len=19),run(3,nonce_len=20),run(2,nonce_len=32)]
rows +=[run(case=c)for c in('transport_error','malformed_nonce','zero_ack','not_ready','busy','signature_status_error','bad_signature_opcode')]
assert [r['return'] for r in rows[:5]]==['0x0']*5
for r in rows[:5]:
 size=64 if r['version']==3 else 128
 assert r['output_size']==size
 assert bytes.fromhex(r['output'])[:size]==bytes(range(size))
 assert bytes.fromhex(r['output'])[size:]==b'\xcc'*(128-size)
 assert all(t['error']=='0x0'for t in r['transfers'])
assert rows[0]['transfers'][0]['tx']=='94'+bytes(range(20)).hex()
assert rows[1]['transfers'][0]['tx']=='9400000020'+bytes(range(32)).hex()
assert [t['tx']for t in rows[2]['transfers'][:2]]==['9400000010'+bytes(range(16)).hex(),'9400100010'+bytes(range(16,32)).hex()]
assert [r['return']for r in rows[5:]]==['0xe00002c2']*4+['0xe00002bc','0xe00002c6','0xe00002c6','0xe00002c6','0xe00002d6','0xe3ff8001','0xe00002cd']
assert all(not r['transfers']for r in rows[5:9])
assert len(rows[13]['transfers'])==92
assert all(bytes.fromhex(r['output'])==b'\xcc'*128 for r in rows[5:])
print(json.dumps({'source_sha256' :SHA,'no_hardware':True,'scope':'actual signer+data-chunk methods; fake provider transfer/continuation, OSData, version, interval, memcpy and delay APIs; synthetic replies only; no cryptography','cases':rows},indent=2))
