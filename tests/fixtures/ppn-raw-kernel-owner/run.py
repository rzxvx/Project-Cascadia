#!/usr/bin/env python3
"""Actual raw PPN ownership guard/callers with only process-local syscall fakes."""
import argparse,hashlib,json,os,pathlib,subprocess,tempfile
p=argparse.ArgumentParser();p.add_argument('--source',type=pathlib.Path);p.add_argument('--cc',default='cc');p.add_argument('--output-dir',type=pathlib.Path);a=p.parse_args();group=pathlib.Path(__file__).resolve().parent;source=(a.source or group/'candidate/ppn.c').resolve();assert source.name=='ppn.c'
pins=json.loads((group/'baseline-hashes.json').read_text())
for name,sha in pins.items():assert hashlib.sha256((group/'baseline'/name).read_bytes()).hexdigest()==sha
if a.output_dir:a.output_dir.mkdir(parents=True,exist_ok=True)
flags=['-std=gnu11','-Wall','-Wextra','-Werror','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie'];env=os.environ.copy();env.update(ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',UBSAN_OPTIONS='halt_on_error=1')
with tempfile.TemporaryDirectory(prefix='ppn-raw-owner-',dir=a.output_dir)as d:
 out=pathlib.Path(d)
 for label,root,extra in[('historical',group/'baseline',['-DHISTORICAL_CONTROL']),('candidate',source.parent,[])]:
  target=out/label;subprocess.run([a.cc,*flags,*extra,'-I'+str(root),str(group/'test_owner.c'),'-o',str(target)],check=True)
  result=subprocess.run([str(target)],check=True,timeout=20,env=env,text=True,capture_output=True);print(result.stdout.strip())
  assert 'hardware_calls=0'in result.stdout
print(json.dumps(dict(result='PASS',source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),historical_sha256=pins,hardware_calls=0),sort_keys=True))
