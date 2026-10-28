#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Execute the old actual reserved-address filter against current C fixture.
The expected assertion proves the new regression detects its high-address alias rejection.
"""
from pathlib import Path
import hashlib,importlib.util,json,shutil,subprocess,tempfile
P=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('tests',P/'test-owned-adc.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
with tempfile.TemporaryDirectory() as td:
 q=Path(td)
 for f in P.iterdir():
  if f.is_file()and f.suffix in ('.c','.h','.inc','.json'):shutil.copy2(f,q/f.name)
 (q/'linux').mkdir()
 for h in ('atomic.h','i2c.h','mutex.h','regmap.h','err.h','errno.h','interrupt.h','kernel.h','lockdep.h','string.h','sched.h','types.h','device.h','slab.h','delay.h','ktime.h'):(q/'linux'/h).write_text('#include "fake-linux.h"\n')
 (q/'regmap-bodies.h').write_text(''.join(m.function((q/'regmap-entrypoints-fixture.h').read_text(),n)for n in ('regmap_read','regmap_write','_regmap_update_bits','regmap_update_bits_base')))
 (q/'a5-pmu-raw-filter.inc').write_bytes((P/'historical-raw-address-filter.inc').read_bytes())
 exe=q/'control';subprocess.run(['gcc','-std=gnu11','-pthread','-Wall','-Wextra','-Werror','-Wno-unused-function','-fsanitize=address,undefined','-fno-pie','-no-pie','-I',str(q),str(q/'fixture.c'),'-o',str(exe)],check=True)
 r=subprocess.run([str(exe),'15'],capture_output=True,text=True)
 assert r.returncode!=0 and '==-EINVAL' in r.stderr,(r.returncode,r.stderr)
 print(json.dumps({'format':'p105-pmu-address-alias-control-v1','hardware_access':False,'historical_source_sha256':hashlib.sha256((P/'historical-raw-address-filter.inc').read_bytes()).hexdigest(),'fixed_source_sha256':hashlib.sha256((P/'a5-pmu-raw-filter.inc').read_bytes()).hexdigest(),'historical_returncode':r.returncode,'historical_stderr':r.stderr,'expected_failure':'old exact-address filter allows0xbc although actual FIFO command equals PMU0x3c'},indent=2))
