"""Compile entire real Linux-C source, fake regmap only; pinned native fixtures."""
from pathlib import Path
import hashlib,json,os,subprocess,tempfile
root=Path(__file__).resolve().parent
ths=root/'ths-idbus13-oracle.json';frames=root/'authcp-ths-window13-oracle.json'
assert hashlib.sha256(ths.read_bytes()).hexdigest()=='41fcad6663a84dd689edbd1bf84710d2a7f4881a67b0a5b7fa5973ed537886cc'
assert hashlib.sha256(frames.read_bytes()).hexdigest()=='78b0091f7f704676495e3121d378eba9d6abfe8234318f686af8aacb37e48e75'
r=json.loads(ths.read_text());assert sum(len(r[k])for k in('size_cases','submit_cases','complete_cases'))==21
native=json.loads(frames.read_text())['cases'];assert [c['max_tx']for c in native]==[32,32]and[c['max_rx']for c in native]==[32,32]
def array(bs):return '{'+','.join(str(b)for b in bs)+'}'
lines=['static void native_frames(void) {']
for c in native:
 lines+=['{ struct fake_bus b; setup(&b); unsigned char out[32];size_t n;u64 id;']
 for tr in c['transfers']:
  tx=bytes.fromhex(tr['tx']);rx=bytes.fromhex(tr['rx']);assert max(len(tx),len(rx))<=32
  lines+=['{ const unsigned char tx[]='+array(tx)+',rx[]='+array(rx)+'; b.count=0;',
    'for(size_t j=0;j<sizeof(rx);j++)b.raw[j]=rx[sizeof(rx)-1-j];',
    'assert(!cascadia_ths_submit(&b.ctx,b.ctx.owner,9,tx,sizeof(tx),sizeof(rx),&id));',
    'assert(!cascadia_ths_receive(&b.ctx,b.ctx.owner,9,id,out,sizeof(out),&n));',
    'assert(n==sizeof(rx) && !memcmp(out,rx,n));assert(b.count==5);',
    'assert(b.op[0].reg==0x20 && b.op[0].n==sizeof(tx) && !memcmp(b.op[0].bytes,tx,sizeof(tx)));',
    'assert(b.op[1].reg==0x41 && b.op[1].bytes[0]==2*(sizeof(rx)-1));',
    'assert(b.op[2].reg==0x40 && b.op[2].bytes[0]==2*sizeof(tx)-1);',
    'assert(b.op[3].reg==0x0d && b.op[4].reg==0x40-sizeof(rx));}']
 lines+=['}']
lines+=['}']
assert hashlib.sha256((root/'historical/ths-before-validator.c').read_bytes()).hexdigest()=='50d61d93b58e82f3bf2112ea4e97244b4d40309ead206bcce87a44a2b2d0480f'
with tempfile.TemporaryDirectory(prefix='cascadia-ths-idbus-')as td:
 p=Path(td);(p/'native-frames.inc').write_text('\n'.join(lines)+'\n');exe=p/'fixture'
 subprocess.run(['cc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie','-pthread','-I'+str(root/'include'),'-I'+str(root),'-I'+str(p),str(root/'transport-fixture.c'),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1:halt_on_error=1'})

# Exact earlier private V1 control; no copied/rewritten production functions.
prefix=(root/'transport-fixture.c').read_text().split('static void native_submissions')[0]
prefix='\n'.join(line for line in prefix.splitlines()if 'cascadia_ths_invalidate(&b->ctx)'not in line)
control=r'''
int main(void) {
 for(int mode=1;mode<=3;mode++){
  struct fake_bus b;setup(&b);u8 tx[1]={0},out[32];u64 id;size_t n=0;
  b.raw[0]=1;b.raw[1]=2;b.raw[2]=3;
  if(mode==1)b.flip_epoch_at=7;
  if(mode==2)b.flip_owner_at=7;
  if(mode==3)b.positive_at=7;
  assert(!cascadia_ths_submit(&b.ctx,b.ctx.owner,9,tx,1,3,&id));
  int error=cascadia_ths_receive(&b.ctx,b.ctx.owner,9,id,out,32,&n);
  if(HISTORICAL && mode!=3){assert(error==0 && n==3 && out[0]==3 && b.ctx.state==CASCADIA_THS_IDLE);}
  else{assert(error==(mode==3?(HISTORICAL?-EIO:-EPROTO):-ESTALE));assert(!n);zero(out,32);assert(b.ctx.state==CASCADIA_THS_UNKNOWN);}
 }
 puts(HISTORICAL?"Historical V1 exposes stale reply publication / positive callback EIO":"Candidate rejects stale callback replies / positive callback EPROTO");return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='cascadia-ths-validator-control-')as td:
 p=Path(td)
 for historical in (True,False):
  actual=root/'historical/ths-before-validator.c'if historical else root/'cascadia-ths-idbus.c'
  body=prefix.replace('#include "cascadia-ths-idbus.c"','#include "'+str(actual)+'"')+control
  q=p/('old.c'if historical else'new.c');q.write_text(body);exe=p/q.stem
  subprocess.run(['cc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-function','-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie','-pthread','-DHISTORICAL='+str(int(historical)),'-I'+str(root/'include'),'-I'+str(root),str(q),'-o',str(exe)],check=True)
  subprocess.run([str(exe)],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1:halt_on_error=1'})
