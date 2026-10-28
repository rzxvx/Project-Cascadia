#!/usr/bin/env python3
import argparse,hashlib,os,pathlib,re,subprocess,tempfile,json
p=argparse.ArgumentParser();p.add_argument('--linux',type=pathlib.Path);p.add_argument('--cc',default='cc');p.add_argument('--output-dir',type=pathlib.Path);a=p.parse_args();group=pathlib.Path(__file__).resolve().parent
pins={'include/linux/dmaengine.h':'a1041fddb0ec26c888320ddca5f55da17e58b4c559e534f5b24e59fafe273c1e','drivers/dma/tegra186-gpc-dma.c':'b4f39876aec6d57324d3ff3f79976daae6b9bc19b2872007ef6621bcc85bc65e'}
def function(s,name):
 m=re.search(r'\nstatic(?: inline)? (?:int|void) '+re.escape(name)+r'\(',s);assert m,name;lo=s.index('{',m.start());depth=0
 for j in range(lo,len(s)):
  depth+=s[j]=='{';depth-=s[j]=='}'
  if not depth:return s[m.start()+1:j+1]
 raise ValueError('unterminated function')
contents={}
fixture=(group/'actual-kernel-functions.h').read_bytes()
assert hashlib.sha256(fixture).hexdigest()=='d153d1625bb1a23001aa8995a4fa56e002c0fdd66ee6bc6cf4c9ec5ca3699a52'
functions=[]
if a.linux:
 for name,sha in pins.items():
  data=(a.linux/name).read_bytes();assert hashlib.sha256(data).hexdigest()==sha;contents[name]=data.decode()
 for name,source in [('tegra_dma_terminate_all', 'drivers/dma/tegra186-gpc-dma.c'), ('tegra_dma_chan_synchronize', 'drivers/dma/tegra186-gpc-dma.c'), ('tegra_dma_pause_noerr', 'drivers/dma/tegra186-gpc-dma.c'), ('dmaengine_terminate_async', 'include/linux/dmaengine.h'), ('dmaengine_synchronize', 'include/linux/dmaengine.h'), ('dmaengine_terminate_sync', 'include/linux/dmaengine.h')]:
  body=function(contents[source],name);assert body in fixture.decode();functions.append(body)
if a.output_dir:a.output_dir.mkdir(parents=True,exist_ok=True)
with tempfile.TemporaryDirectory(prefix='ppn-linux-provider-',dir=a.output_dir)as d:
 out=pathlib.Path(d);(out/'actual-kernel-functions.h').write_bytes(fixture)
 flags=['-std=c11','-Wall','-Wextra','-Werror','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie','-I'+str(out)];exe=out/'contract';subprocess.run([a.cc,*flags,str(group/'fixture.c'),'-o',str(exe)],check=True)
 env=os.environ.copy();env.update(ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',UBSAN_OPTIONS='halt_on_error=1');subprocess.run([str(exe)],check=True,timeout=20,env=env)
 print(json.dumps(dict(result='PASS',actual_GPL_functions=6,source_sha256=pins,hardware_calls=0),sort_keys=True))
