#!/usr/bin/env python3
"""Actual candidate C, original IRQ traces and independent normalized oracle."""
import argparse,pathlib,json,hashlib,subprocess,os,tempfile,atexit
p=argparse.ArgumentParser();p.add_argument('--cc',default='cc');p.add_argument('--sanitize',action=argparse.BooleanOptionalAction,default=True);p.add_argument('--output-dir',type=pathlib.Path,default=None);a=p.parse_args();repo=pathlib.Path(__file__).resolve().parents[1];root=repo/'experiments/p105-ppn';fixture=repo/'tests/fixtures/ppn-cdma-completion'
# Frozen actual production dependencies, not mirrored implementations.
for name in ['ppn_cdma_plan.c','ppn_cdma_plan.h','ppn_stage_engine.c','ppn_stage_engine.h','test_ppn_stage.c']:
 # The manifest itself is pinned at freeze; source changes require explicit review.
 expected=json.loads((fixture/'dependency-hashes.json').read_text())[name]
 assert hashlib.sha256((root/name).read_bytes()).hexdigest()==expected
tr=json.loads((fixture/'completion-original-traces.json').read_text());assert tr['result']=='PASS' and tr['actual_original_runs']==274 and tr['hardware_calls']==0
rows=[r for r in tr['rows'] if r['kind']=='irq'];assert len(rows)==112
temporary=tempfile.TemporaryDirectory(prefix='ppn-cdma-completion-');atexit.register(temporary.cleanup)
out=a.output_dir.resolve() if a.output_dir else pathlib.Path(temporary.name);out.mkdir(parents=True,exist_ok=True)
header=['struct oracle { unsigned int channel, control, pending, remaining; int result; unsigned int mask, finish; int post_error; };','static const struct oracle oracles[] = {']
for r in rows:
 ctl,pending,left=r['initial'],r['pending'],r['residue']
 # Separately constructed safety oracle; no code copied from C candidate.
 if ctl & 0x40000: result,mask,finish,post=-5,0,False,0
 elif ctl & 0x100000 or not ctl & 0x80000: result,mask,finish,post=-11,0,False,0
 else:
  result=0;finish=pending==0 and left==0;mask=3 if finish else 1
  post=-95 if pending else -71 if left else 0
 ack=[x for x in r['trace'] if x[0]=='dma-write' and int(x[1],16)==0x37000000+(r['channel']<<12)]
 disable=[x for x in r['trace'] if x[0]=='dma-write' and int(x[1],16)==0x37000008]
 cb=[x for x in r['trace']if x[0]=='callback']
 assert bool(ack)==bool(mask) and (not ack or ack[0][2]==0x80000)
 if finish:assert cb and disable==[['dma-write','0x37000008',1<<r['channel']]]
 # Original signed-remaining shortcut is explicitly stricter in Linux candidate.
 elif cb:assert not pending and left>=0x80000000
 header.append('{%du,%du,%du,%du,%d,%du,%du,%d},'%(r['channel'],ctl,pending,left,result,mask,finish,post))
header.append('};');(out/'oracle.h').write_text('\n'.join(header)+'\n')
flags=['-std=c11','-Wall','-Wextra','-Werror','-O1','-g','-I'+str(root),'-I'+str(fixture),'-I'+str(out)]
if a.sanitize:flags+=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie']
for name,sources in [('completion',['test_completion.c','ppn_cdma_completion.c','ppn_cdma_plan.c']),('engine-completion',['test_engine_completion.c','ppn_cdma_completion.c','ppn_cdma_plan.c','ppn_stage_engine.c'])]:
 target=out/name;subprocess.run([a.cc,*flags,*[str((fixture if s.startswith('test_') else root)/s)for s in sources],'-o',str(target)],check=True)
 env=os.environ.copy();env['ASAN_OPTIONS']='detect_leaks=1:abort_on_error=1';env['UBSAN_OPTIONS']='halt_on_error=1'
 subprocess.run([str(target)],check=True,env=env,timeout=20)
print('PASS actual C/original oracle, no hardware APIs or engine source changes')
