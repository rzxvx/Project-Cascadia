#!/usr/bin/env python3
"""Real C preparation against original instruction trace and integer oracle."""
import ctypes as C,json,random,sys
from pathlib import Path
HERE=Path(__file__).resolve().parents[1]/'experiments/p105-ppn'
from experimental_helpers import native_stage
_stage,_library=native_stage(HERE,['ppn_seq_plan.c'],shared=True)
class Request(C.Structure):
    _fields_=[('bus',C.c_uint32),('clock_factor',C.c_uint32),('timing_weights',C.c_uint32*2),('count',C.c_uint32),('unpatched_words',C.POINTER(C.c_uint32))]
class Plan(C.Structure):
    _fields_=[('bus',C.c_uint32),('clock_factor',C.c_uint32),('words',C.c_uint32*44),('offsets',C.c_uint32*44)]
lib=C.CDLL(str(_library))
f=lib.ppn_seq_prepare;f.argtypes=[C.POINTER(Request),C.POINTER(Plan)];f.restype=C.c_int
count=0;matched=0
def run(r,source):
    global count
    count+=1;r.unpatched_words=source;p=Plan();C.memset(C.byref(p),0xa5,C.sizeof(p));old=bytes(p)
    rc=f(C.byref(r),C.byref(p))
    if rc:assert bytes(p)==old
    return rc,p
def oracle(r,src,p):
    expected=[]
    for word in src:
        if word in(0x0e000000,0x0e000001):word=0x0e000000+(r.clock_factor*r.timing_weights[word&1]//1000000)
        expected.append(word)
    assert list(p.words)==expected and p.bus==r.bus and p.clock_factor==r.clock_factor
    assert list(p.offsets)==[0xc1000+4*i for i in range(44)]
for x in json.loads((HERE/'sequencer-trace.json').read_text())['cases']:
    if x['already_patched']:continue
    src=(C.c_uint32*44)(*x['program']);r=Request(x['bus'],x['clock'],(C.c_uint32*2)(*x['weights']),44,None)
    rc,p=run(r,src)
    if not r.clock_factor or max(r.clock_factor*w for w in r.timing_weights)>0xffffffff:assert rc<0;continue
    assert rc==0;oracle(r,src,p);assert list(p.words)==x['patched'];matched+=1
rng=random.Random(0x44_88a4)
for i in range(1024):
    clock=rng.randrange(1,0xffffffff);limit=0xffffffff//clock
    weights=[rng.randint(0,limit),rng.randint(0,limit)]
    words=[rng.getrandbits(32)for _ in range(44)];words[:4]=[0x0e000000,0x0e000001,0x0e000002,0x0e000003]
    src=(C.c_uint32*44)(*words);r=Request(i%2,clock,(C.c_uint32*2)(*weights),44,None)
    rc,p=run(r,src);assert rc==0;oracle(r,src,p)
# Power-of-million boundaries and full32-bit products validate an independent
# integer-division oracle against the exact original magic multiply expression.
for product in (1,999999,1000000,1000001,4293999999,4294000000,0xffffffff):
    r=Request(0,product,(C.c_uint32*2)(1,1),44,None);src=(C.c_uint32*44)(*([0x0e000000,0x0e000001]*22))
    rc,p=run(r,src);assert rc==0;oracle(r,src,p)
for bus,clock,n,weights in [(2,1,44,(1,1)),(0,0,44,(1,1)),(0,1,43,(1,1)),(0,1,45,(1,1)),(0,0xffffffff,44,(2,1)),(0,0xffffffff,44,(1,2))]:
    r=Request(bus,clock,(C.c_uint32*2)(*weights),n,None);src=(C.c_uint32*44)(*range(44));rc,p=run(r,src);assert rc<0
r=Request(0,1,(C.c_uint32*2)(1,1),44,None);rc,p=run(r,None);assert rc<0
assert f(None,C.byref(p))<0 and f(C.byref(r),None)<0
print(f'PASS: {count+2} actual C cases; {matched} original88a4 matches; separate per-controller inputs and strict overflow/clock rejection')

# Unit conversion executes the actual candidate API, including original low32
# truncation controls which the candidate intentionally rejects.
clock=lib.ppn_seq_hclk_khz
clock.argtypes=[C.c_uint64,C.POINTER(C.c_uint32)];clock.restype=C.c_int
clock_cases=json.loads((HERE/'clock-field-trace.json').read_text())['cases']
for row in clock_cases:
    hz=row['hclk_value'];factor=C.c_uint32(0xa5a5a5a5)
    rc=clock(hz,C.byref(factor))
    if hz<1000 or hz//1000>0xffffffff:
        assert rc<0 and factor.value==0xa5a5a5a5
    else:
        assert rc==0 and factor.value==hz//1000==row['command_b8']
assert clock(24000000,None)<0
print('PASS: exact original11 HCLK conversion cases; zero/truncation rejected')
