#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Pinned actual Linux-C composition; no device access or real crypto."""
import argparse,hashlib,json,os,subprocess,tempfile
from pathlib import Path
ap=argparse.ArgumentParser();ap.add_argument('--source-root',type=Path,required=True);args=ap.parse_args();here=Path(__file__).resolve().parent;root=args.source_root
sources={'experiments/p105-ths-idbus/cascadia-ths-idbus.c':'bd6da8ec07730a79c5d3bfc9b936085bd67016c02bfc70337a0b69978f5af61c','experiments/p105-authcp-aid/cascadia-authcp-aid.c':'382cc9dd37bd18d37e9466830fe109c0b29bbe1bdab6885bf65acdff2584b529','experiments/p105-ths-idbus/authcp-ths-window13-oracle.json':'78b0091f7f704676495e3121d378eba9d6abfe8234318f686af8aacb37e48e75'}
sources.update({'experiments/p105-ths-idbus/cascadia-ths-idbus.h': '7a29ca7b1eca0a85d1d5d212c429948179c7f86daf475619041b6c20b6a69f3d', 'experiments/p105-authcp-aid/cascadia-authcp-aid.h': '87f49ff6b1ad50869125612e9563d5d512aa800e3deb3a0a1fa8ce8f4d15fd33'})
for path,sha in sources.items():assert hashlib.sha256((root/path).read_bytes()).hexdigest()==sha,path
r=json.loads((root/'experiments/p105-ths-idbus/authcp-ths-window13-oracle.json').read_text())
def bytesarray(b):return'{'+','.join(str(v)for v in b)+'}'
lines=[]
for c in r['cases']:
 lines.append('static const struct wire_frame v'+str(c['version'])+'[]={')
 for t in c['transfers']:
  tx=bytes.fromhex(t['tx']);rx=bytes.fromhex(t['rx']);lines.append('{'+bytesarray(tx)+','+bytesarray(rx)+','+str(len(tx))+','+str(len(rx))+'},')
 lines.append('};')
with tempfile.TemporaryDirectory(prefix='cascadia-ths-pipeline-')as td:
 p=Path(td);(p/'frames.inc').write_text('\n'.join(lines)+'\n');exe=p/'pipeline'
 subprocess.run(['cc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie','-pthread','-I'+str(here/'include'),'-I'+str(root/'experiments/p105-ths-idbus'),'-I'+str(root/'experiments/p105-authcp-aid'),'-I'+str(p),str(root/'experiments/p105-ths-idbus/cascadia-ths-idbus.c'),str(root/'experiments/p105-authcp-aid/cascadia-authcp-aid.c'),str(here/'pipeline-fixture.c'),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1:halt_on_error=1'})
