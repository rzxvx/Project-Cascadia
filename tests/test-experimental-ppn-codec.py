#!/usr/bin/env python3
"""Real C codec versus independent Python byte oracle; ordinary process RAM only."""
import argparse, ctypes as C, hashlib, json, os, pathlib, random, struct, subprocess, tempfile
P=pathlib.Path(__file__).resolve().parents[1]/'experiments/p105-ppn'
for name, expected in [('ppn_bbt_tx.c','ace1a85f2fff772f671498df3b4c6a86eb0201c6196566b4dbebb40262aefca0'),('ppn_bbt_tx.h','ccca6fb106f33da9e437dbe3c86e955880aa5ff787458af913a46741decab7e1')]:
    assert hashlib.sha256((P/name).read_bytes()).hexdigest()==expected, 'frozen v3 dependency changed'
parser=argparse.ArgumentParser();parser.add_argument('--cc',default=os.environ.get('CC','gcc'));parser.add_argument('--sanitize',action='store_true');parser.add_argument('--library',type=pathlib.Path);args=parser.parse_args()
U8=C.c_uint8;U32=C.c_uint32;U64=C.c_uint64;PAGE=16384;BITMAP=8192;MAX64=2**64-1
class Slot(C.Structure): _fields_=[('physical_id',U64),('alias_domain',U64)]
class Config(C.Structure): _fields_=[('media_id',U64),('owner_id',U64),('lease_epoch',U64),('blocks',U32),('slots',Slot*2),('ownership_proven',C.c_bool)]
class Snapshot(C.Structure): _fields_=[(n,U64) for n in ['media_id','owner_id','record_epoch','generation','physical_slot','alias_domain']]+[(n,U32) for n in ['blocks','operation','pending_block']]+[('bad',U8*BITMAP)]
class View(C.Structure): _fields_=[('snapshot',Snapshot),('body_crc',U32)]
class Decoded(C.Structure): _fields_=[('snapshot',Snapshot),('body_crc',U32),('manifest_crc',U32)]
class Copy(C.Structure): _fields_=Decoded._fields_+[(n,C.c_bool) for n in ['body_present','manifest_present','complete_read','absent_manifest_erased','empty_slot_erased']]
class Tx(C.Structure): _fields_=[('config',Config),('committed',Snapshot),('prepared',Snapshot)]+[(n,U32) for n in ['committed_crc','prepared_crc','active_slot','target_slot','phase','after_commit']]
# Deliberately table-based CRC, independent of the C bit loop.
TABLE=[]
for n in range(256):
    v=n
    for _ in range(8): v=(v>>1)^(0x82f63b78 if v&1 else 0)
    TABLE.append(v)
def crc(data):
    v=0xffffffff
    for b in data: v=TABLE[(v^b)&255]^(v>>8)
    return v^0xffffffff
assert crc(b'123456789')==0xe3069283

def make(blocks=256,slot=0,op=0,gen=3,epoch=0x123456789abcdef):
    c=Config(0x1122334455667788,0xfedcba9876543210,epoch,blocks,(Slot*2)(Slot(0x102030405060708,0x8877665544332211),Slot(0x9999999988888888,0x7777777766666666)),False)
    s=Snapshot(c.media_id,c.owner_id,epoch,gen,c.slots[slot].physical_id,c.slots[slot].alias_domain,blocks,op,blocks-1 if op else 0xffffffff)
    for b in sorted({0,blocks//2,blocks-1}): s.bad[b//8]|=1<<(b%8)
    return c,s

def norm(s):
    fields=struct.pack('<6Q3I',s.media_id,s.owner_id,s.record_epoch,s.generation,s.physical_slot,s.alias_domain,s.blocks,s.operation,s.pending_block)
    b=crc(struct.pack('<Q',0x314254424e505050)+fields+bytes(s.bad))
    m=crc(struct.pack('<Q',0x3154494d4d4f4350)+fields[:48]+struct.pack('<I',b))
    return fields,b,m

def oracle(s,kind):
    fields,b,m=norm(s); p=bytearray(b'\xff'*PAGE);p[:128]=bytes(128)
    p[:8]=b'PPNLBOD1' if kind==1 else b'PPNLCMT1'
    struct.pack_into('<HHIIIII',p,8,1,kind,128,PAGE,BITMAP if kind==1 else 0,8320 if kind==1 else 128,0)
    p[32:92]=fields
    struct.pack_into('<IIIQI',p,92,b,0 if kind==1 else m,0,0x314254424e505050 if kind==1 else 0x3154494d4d4f4350,BITMAP)
    if kind==1:p[128:8320]=bytes(s.bad)
    struct.pack_into('<I',p,100,crc(p))
    return bytes(p)

def recalc(p):
    p=bytearray(p);p[100:104]=bytes(4);struct.pack_into('<I',p,100,crc(p));return bytes(p)

def buf(b): return (U8*len(b)).from_buffer_copy(b)
def clone(o): return type(o).from_buffer_copy(bytes(o))
count=0
with tempfile.TemporaryDirectory(prefix='ppn-codec-') as td:
    so=args.library or pathlib.Path(td)/'codec.so'
    if not args.library:
        sizes=pathlib.Path(td)/'sizes.c';sizes.write_text('#include "ppn_bbt_codec.h"\nunsigned long codec_size(unsigned int n){const unsigned long s[]={sizeof(struct ppn_bbt_config),sizeof(struct ppn_bbt_snapshot),sizeof(struct ppn_bbt_body_view),sizeof(struct ppn_bbt_decoded),sizeof(struct ppn_bbt_copy),sizeof(struct ppn_bbt_tx)};return n<6?s[n]:0;}\n')
        flags=['-fsanitize=address,undefined','-fno-omit-frame-pointer'] if args.sanitize else []
        subprocess.run([args.cc,'-shared','-fPIC','-std=c11','-Wall','-Wextra','-Werror','-O2',*flags,'-I'+str(P),'-I'+str(P),str(P/'ppn_bbt_codec.c'),str(P/'ppn_bbt_tx.c'),str(sizes),'-o',str(so)],check=True)
    lib=C.CDLL(str(so));lib.codec_size.argtypes=[U32];lib.codec_size.restype=C.c_ulong
    for i,t in enumerate([Config,Snapshot,View,Decoded,Copy,Tx]):assert lib.codec_size(i)==C.sizeof(t)
    for n,a in [('encode_body',[C.c_void_p,U32,C.c_void_p,C.c_void_p,C.c_size_t]),('encode_manifest',[C.c_void_p,U32,C.c_void_p,U32,U32,C.c_void_p,C.c_size_t]),('decode_body',[C.c_void_p,U32,C.c_void_p,C.c_size_t,C.c_void_p]),('decode_pair',[C.c_void_p,U32,C.c_void_p,C.c_size_t,C.c_void_p,C.c_size_t,C.c_void_p])]:
        f=getattr(lib,'ppn_bbt_codec_'+n);f.argtypes=a;f.restype=C.c_int
    eb=lib.ppn_bbt_codec_encode_body;em=lib.ppn_bbt_codec_encode_manifest;db=lib.ppn_bbt_codec_decode_body;dp=lib.ppn_bbt_codec_decode_pair
    lib.ppn_bbt_snapshot_crc.argtypes=[C.c_void_p];lib.ppn_bbt_snapshot_crc.restype=U32
    lib.ppn_bbt_manifest_crc.argtypes=[C.c_void_p,U32];lib.ppn_bbt_manifest_crc.restype=U32
    def call(f,*a,ok=True,output=None):
        global count
        before=bytes(output) if output is not None else None;r=f(*a);count+=1
        assert (r==0)==ok,(f.__name__,r,count)
        if not ok and output is not None:assert bytes(output)==before
        return r
    def reject_body(c,p,slot=0):
        v=View();C.memset(C.byref(v),0xa5,C.sizeof(v));b=buf(p)
        call(db,C.byref(c),slot,b,len(p),C.byref(v),ok=False,output=v);assert bytes(b)==p
    def reject_pair(c,b,m,slot=0):
        o=Decoded();C.memset(C.byref(o),0xa5,C.sizeof(o));bb,mm=buf(b),buf(m)
        call(dp,C.byref(c),slot,bb,len(b),mm,len(m),C.byref(o),ok=False,output=o);assert bytes(bb)==b and bytes(mm)==m
    rng=random.Random(0x105BB7)
    good=[]
    for blocks in [1,7,8,9,255,256,65535,65536]:
      for slot in [0,1]:
       for op in [0,1,2]:
        c,s=make(blocks,slot,op,gen=MAX64 if blocks==65536 else rng.randrange(1,MAX64));beforec=bytes(c);befores=bytes(s)
        b,m=oracle(s,1),oracle(s,2);_,bc,mc=norm(s)
        assert lib.ppn_bbt_snapshot_crc(C.byref(s))==bc and lib.ppn_bbt_manifest_crc(C.byref(s),bc)==mc
        bb,mm=buf(b'\xa5'*PAGE),buf(b'\xa5'*PAGE)
        call(eb,C.byref(c),slot,C.byref(s),bb,PAGE);call(em,C.byref(c),slot,C.byref(s),bc,mc,mm,PAGE)
        assert bytes(bb)==b and bytes(mm)==m
        out=Decoded();view=View();call(dp,C.byref(c),slot,bb,PAGE,mm,PAGE,C.byref(out));call(db,C.byref(c),slot,bb,PAGE,C.byref(view))
        assert bytes(out.snapshot)==bytes(s)==bytes(view.snapshot) and out.body_crc==bc==view.body_crc and out.manifest_crc==mc
        assert bytes(c)==beforec and bytes(s)==befores and not c.ownership_proven
        good.append((c,s,b,m))
    c,s=make();b,m=oracle(s,1),oracle(s,2);golden=b+m
    golden_hash=hashlib.sha256(golden).hexdigest()
    assert golden_hash=='fc4ce149300de196c59c39f52a05bf1c385635bd1ab59437f6afed9a621ee533'
    reject_body(c,bytes(PAGE));reject_body(c,b"\xff"*PAGE)
    reject_pair(c,b,b"\xff"*PAGE);reject_pair(c,b"\xff"*PAGE,m)
    # Every byte in both full pages: malformed data never publishes an output.
    p=bytearray(b)
    for i in range(PAGE):
        p[i]^=1;reject_body(c,bytes(p));p[i]^=1
    p=bytearray(m)
    for i in range(PAGE):
        p[i]^=1;reject_pair(c,b,bytes(p));p[i]^=1
    # Recompute page CRC so all structural checks and normalized fingerprints matter.
    for off,size in [(0,8),(8,2),(10,2),(12,4),(16,4),(20,4),(24,4),(28,4),(32,8),(40,8),(48,8),(56,8),(64,8),(72,8),(80,4),(84,4),(88,4),(92,4),(96,4),(104,8),(112,4),(116,12),(128,1),(8319,1),(8320,1),(16383,1)]:
        for i in range(size):
            q=bytearray(b);q[off+i]^=1;reject_body(c,recalc(q))
            q=bytearray(m);q[off+i]^=1;reject_pair(c,b,recalc(q))
    # Independently valid records may still not be a pair.
    for field in ['record_epoch','generation','operation','pending_block']:
        t=clone(s)
        if field=='operation':t.operation=1;t.pending_block=0
        elif field=='pending_block':t.operation=1;t.pending_block=1
        else:setattr(t,field,getattr(t,field)+1)
        reject_pair(c,b,oracle(t,2))
    t=clone(s);t.bad[0]^=1;reject_pair(c,b,oracle(t,2))
    # Historical lease epochs are data, not evidence of a current hardware lease.
    old=clone(s);old.record_epoch-=1;o=Decoded();call(dp,C.byref(c),0,buf(oracle(old,1)),PAGE,buf(oracle(old,2)),PAGE,C.byref(o));assert o.snapshot.record_epoch==old.record_epoch
    for field,value in [('media_id',0),('owner_id',0),('record_epoch',0),('generation',0),('physical_slot',99),('alias_domain',99),('blocks',0),('blocks',65537),('operation',3),('pending_block',0),('record_epoch',old.record_epoch)]:
        t=clone(s);setattr(t,field,value);out=buf(b'\xa5'*PAGE);_,bc,mc=norm(t)
        call(eb,C.byref(c),0,C.byref(t),out,PAGE,ok=False,output=out);call(em,C.byref(c),0,C.byref(t),bc,mc,out,PAGE,ok=False,output=out)
    for field,value in [('media_id',0),('owner_id',0),('lease_epoch',0),('blocks',0),('blocks',65537)]:
        cc=clone(c);setattr(cc,field,value);reject_body(cc,b);reject_pair(cc,b,m)
    for field in ['physical_id','alias_domain']:
        cc=clone(c);setattr(cc.slots[1],field,getattr(cc.slots[0],field));reject_body(cc,b);reject_pair(cc,b,m)
    for blocks in [1,7,9,65535]:
        cc,t=make(blocks);t.bad[blocks//8]|=1<<(blocks%8);out=buf(b'\xa5'*PAGE)
        call(eb,C.byref(cc),0,C.byref(t),out,PAGE,ok=False,output=out);reject_body(cc,oracle(t,1))
    for length in [0,1,127,128,PAGE-1,PAGE+1,2**(8*C.sizeof(C.c_size_t))-1]:
        bb,mm=buf(b),buf(m);out=Decoded();v=View()
        call(eb,C.byref(c),0,C.byref(s),bb,length,ok=False,output=bb)
        call(em,C.byref(c),0,C.byref(s),*norm(s)[1:],mm,length,ok=False,output=mm)
        call(db,C.byref(c),0,bb,length,C.byref(v),ok=False,output=v)
        call(dp,C.byref(c),0,bb,length,mm,PAGE,C.byref(out),ok=False,output=out)
        call(dp,C.byref(c),0,bb,PAGE,mm,length,C.byref(out),ok=False,output=out)
    for slot in [2,0xffffffff]:reject_body(c,b,slot);reject_pair(c,b,m,slot)
    bb,mm=buf(b),buf(m);o=Decoded();v=View();bc,mc=norm(s)[1:]
    call(em,C.byref(c),0,C.byref(s),bc^1,mc,mm,PAGE,ok=False,output=mm);call(em,C.byref(c),0,C.byref(s),bc,mc^1,mm,PAGE,ok=False,output=mm)
    # Null arguments (no dereference) and every prohibited alias relation.
    for index in [0,2,3]:
        a=[C.byref(c),0,C.byref(s),bb,PAGE];a[index]=None;call(eb,*a,ok=False,output=bb)
    for index in [0,2,5]:
        a=[C.byref(c),0,C.byref(s),bc,mc,mm,PAGE];a[index]=None;call(em,*a,ok=False,output=mm)
    for index in [0,2,4]:
        a=[C.byref(c),0,bb,PAGE,C.byref(v)];a[index]=None;call(db,*a,ok=False,output=v)
    for index in [0,2,4,6]:
        a=[C.byref(c),0,bb,PAGE,mm,PAGE,C.byref(o)];a[index]=None;call(dp,*a,ok=False,output=o)
    for dest in [C.byref(c),C.byref(s)]:
        call(eb,C.byref(c),0,C.byref(s),dest,PAGE,ok=False)
        call(em,C.byref(c),0,C.byref(s),bc,mc,dest,PAGE,ok=False)
    call(eb,C.byref(c),0,C.byref(c),bb,PAGE,ok=False,output=bb)
    call(em,C.byref(c),0,C.byref(c),bc,mc,bb,PAGE,ok=False,output=bb)
    for dest in [C.byref(c),bb]:call(db,C.byref(c),0,bb,PAGE,dest,ok=False)
    for dest in [C.byref(c),bb,mm]:call(dp,C.byref(c),0,bb,PAGE,mm,PAGE,dest,ok=False)
    call(dp,C.byref(c),0,bb,PAGE,bb,PAGE,C.byref(o),ok=False,output=o)
    call(db,bb,0,bb,PAGE,C.byref(v),ok=False,output=v)
    call(dp,bb,0,bb,PAGE,mm,PAGE,C.byref(o),ok=False,output=o)
    # Unaligned byte buffers are supported; partial overlap is forbidden.
    arena=(U8*(PAGE*2+16))();C.memmove(C.byref(arena,1),b,PAGE);C.memmove(C.byref(arena,PAGE+2),m,PAGE)
    call(dp,C.byref(c),0,C.byref(arena,1),PAGE,C.byref(arena,PAGE+2),PAGE,C.byref(o));assert bytes(o.snapshot)==bytes(s)
    call(dp,C.byref(c),0,C.byref(arena,1),PAGE,C.byref(arena,PAGE),PAGE,C.byref(o),ok=False,output=o)
    call(db,C.byref(c),0,C.byref(arena,1),PAGE,C.byref(arena,PAGE),ok=False)
    # Valid bytes cannot create complete-read/blank/ownership/durability witnesses.
    lib.ppn_bbt_recover.argtypes=[C.c_void_p,C.c_void_p,C.c_void_p];lib.ppn_bbt_recover.restype=C.c_int
    call(dp,C.byref(c),0,bb,PAGE,mm,PAGE,C.byref(o))
    copies=(Copy*2)();copies[0].snapshot=clone(o.snapshot);copies[0].body_crc=o.body_crc;copies[0].manifest_crc=o.manifest_crc
    copies[0].body_present=True;copies[0].manifest_present=True
    copies[1].complete_read=True;copies[1].empty_slot_erased=True
    cc=clone(c);cc.ownership_proven=True;tx=Tx();before=bytes(copies)
    call(lib.ppn_bbt_recover,C.byref(tx),C.byref(cc),copies,ok=False);assert tx.phase==6 and bytes(copies)==before
    # Only this independent test fixture supplies explicit external read/blank flags.
    copies[0].body_present=True;copies[0].manifest_present=True;copies[0].complete_read=True
    copies[1].complete_read=True;copies[1].empty_slot_erased=True
    tx=Tx();call(lib.ppn_bbt_recover,C.byref(tx),C.byref(cc),copies);assert tx.phase==2 and tx.committed.generation==s.generation
    assert not hasattr(o,'complete_read') and not hasattr(o,'durable') and not hasattr(o,'quiesced')
    # Fully deterministic pseudo-random independently byte-built vectors.
    for _ in range(64):
        blocks=rng.randrange(1,65537);slot=rng.randrange(2);cc,t=make(blocks,slot,rng.randrange(3),rng.randrange(1,MAX64),rng.randrange(1,MAX64))
        for _ in range(20):i=rng.randrange(blocks);t.bad[i//8]|=1<<(i%8)
        out=Decoded();call(dp,C.byref(cc),slot,buf(oracle(t,1)),PAGE,buf(oracle(t,2)),PAGE,C.byref(out));assert bytes(out.snapshot)==bytes(t)
    print(json.dumps({'result':'PASS','actual_C_calls':count,'golden_sha256':golden_hash,'golden_bytes':len(golden),'single_byte_corruptions':32768,'independent_oracle':True,'hardware_calls':0},sort_keys=True))
