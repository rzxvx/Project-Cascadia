#!/usr/bin/env python3
"""Calls real unbound C function; independent oracle and original trace comparison."""
import ctypes as C, json, random, sys
from pathlib import Path
HERE=Path(__file__).resolve().parents[1]/'experiments/p105-ppn'
from experimental_helpers import native_stage
_stage,_library=native_stage(HERE,['ppn_cdma_plan.c'],shared=True)
class SG(C.Structure): _fields_=[('dma_address',C.c_uint32),('length',C.c_uint32)]
class Request(C.Structure):
    _fields_=[(k,C.c_uint32) for k in ('bus','metadata','direction','selector','start_selector','chain_dma','chain_bytes','size','word','burst','count')]+[('sg',C.POINTER(SG))]
class Plan(C.Structure):
    _fields_=[('chain',C.c_uint32*1032),('channel',C.c_uint32),('cache_bytes',C.c_uint32),('register_offsets',C.c_uint32*6),('register_values',C.c_uint32*6)]
lib=C.CDLL(str(_library)); f=lib.ppn_cdma_build_plan
f.argtypes=[C.POINTER(Request),C.POINTER(Plan)]; f.restype=C.c_int
cases=0
def invoke(r,sg):
    global cases
    cases+=1; r.sg=sg; p=Plan();C.memset(C.byref(p),0xa5,C.sizeof(p));old=bytes(p)
    rc=f(C.byref(r),C.byref(p))
    if rc: assert bytes(p)==old,'invalid request partially published a plan'
    return rc,p
def oracle(r,sg,p):
    expected=[0]*1032
    for k in range(r.count):
        expected[k*8:k*8+4]=[r.chain_dma+(k+1)*32,3+(r.selector<<24)+(256 if k==r.count-1 else 0),sg[k].dma_address,sg[k].length]
    expected[r.count*8]=r.chain_dma+(r.count+1)*32
    assert list(p.chain)==expected
    ch=5+2*r.bus+r.metadata
    assert p.channel==ch and p.cache_bytes==(r.count+1)*32
    assert list(p.register_offsets)==[ch*4096,ch*4096+4,ch*4096+8,ch*4096+12,ch*4096+20,ch*4096]
    width={1:0,2:4,4:8}[r.word];burst={1:0,2:16,4:32,8:48,16:64,32:80}[r.burst]
    assert list(p.register_values)==[2,(2*r.bus+r.metadata)*65536+width+burst+2*r.direction,
        0x31200014+r.bus*0x100000+4*r.metadata,r.size,r.chain_dma,0x1c0009+(r.start_selector<<24)]
def req(n=1): return Request(0,0,1,2,2,0x10004000,(n+2)*32,n*64,4,8,n,None)
rows=json.loads((HERE/'cdma-trace.json').read_text())['cases']; matched=0
for x in rows:
    if x['state']!='returned' or x['result']!=0 or len(x['lengths'])>127: continue
    n=len(x['lengths']);sg=(SG*n)(*[SG(0x10008000+k*4096,v) for k,v in enumerate(x['lengths'])])
    r=req(n);r.bus=(x['channel']-5)//2;r.metadata=(x['channel']-5)%2;r.direction=x['direction']
    r.size=x['size'];r.word=x['word'];r.burst=x['burst'];r.selector=2 if x['fresh'] or r.direction else 0
    rc,p=invoke(r,sg);assert rc==0;oracle(r,sg,p)
    for k,d in enumerate(x['descriptors']): assert list(p.chain[k*8:k*8+4])==d
    assert list(zip(p.register_offsets,p.register_values))==[tuple(t[1:])for t in x['trace']if t[0]=='virtual-mmio'][-6:]
    matched+=1
rng=random.Random(0x12_3380)
for _ in range(512):
    n=rng.randint(1,127);r=req(n);r.bus=rng.randrange(2);r.metadata=rng.randrange(2);r.direction=rng.randrange(2)
    r.selector=rng.randrange(16);r.start_selector=rng.randrange(16);r.word=rng.choice([1,2,4]);r.burst=rng.choice([1,2,4,8,16,32])
    sg=(SG*n)(*[SG(rng.randrange(0,0xffff)*4096,rng.randint(1,16384)) for k in range(n)])
    r.size=sum(s.length for s in sg);rc,p=invoke(r,sg);assert rc==0;oracle(r,sg,p)
badfields={'bus':[2,0xffffffff],'metadata':[2],'direction':[2],'selector':[16],'start_selector':[16],
 'chain_dma':[1,0xffffffe0],'chain_bytes':[0,95,0xffffffff],'size':[0,65,0x80000000],
 'word':[0,3,8],'burst':[0,3,64],'count':[0,128,0xffffffff]}
for name,values in badfields.items():
    for v in values:
        r=req();setattr(r,name,v);sg=(SG*1)(SG(0x10008000,64));rc,p=invoke(r,sg);assert rc<0,(name,v)
for addr,length,expect in [(0,0,True),(0,0x1000001,True),(0xffffffff,2,True),
                           (0xffffffc0,64,False),(0xffffffe0,64,True)]:
    r=req();r.size=length;sg=(SG*1)(SG(addr,length));rc,p=invoke(r,sg)
    assert (rc<0)==expect
    if not rc:oracle(r,sg,p)
sg=(SG*1)(SG(0x10008000,64));r=req();r.sg=sg;p=Plan()
assert f(None,C.byref(p))<0 and f(C.byref(r),None)<0
print(f'PASS: {cases+2} real C cases; {matched} exact original cc08/cf30 traces; independent adversarial SG oracle')
