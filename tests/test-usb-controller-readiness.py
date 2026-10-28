#!/usr/bin/env python3
"""Actual controller C retains readiness gaps; fake registers, no hardware."""
from pathlib import Path
import hashlib,json,subprocess,tempfile,os,resource
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
r=Path(__file__).resolve().parents[1];g=r/'experiments/p105-usb-controller';current=(g/'cascadia-controller.c').read_text();old=(g/'historical-gate-ready.c').read_text();v=json.loads((g/'readiness-provenance.json').read_text());assert hashlib.sha256(old.encode()).hexdigest()==v['historical_gate_ready_SHA256'];assert hashlib.sha256(current.encode()).hexdigest()==v['candidate_controller_SHA256'];assert v['normal_runtime_activation']is False and v['hardware_access']is False
a=current.index('static int gate_ready(');end=current.index('\nstatic int complete(',a)
fixture=(g/'controller-fixture.c').read_text()+r'''
static void observe_gate(unsigned int value) { maps[1].values[0x1088 / 4] = value; }
int main(void) {
 const unsigned int observations[]={0x0f,0xf0,0x800000ff};
 unsigned int cases=0;
 for(int reset=0;reset<=1;reset++) for(unsigned int i=0;i<3;i++) {
  int expected=i==2?-16:-112;
#if !HISTORICAL_CONTROL
  assert(!start(1,observations[i],0xa50000e1));
  assert(operation(7,reset,1)==expected);assert(!c.fault&&!c.touched&&!c.completed_operations&&io==1);
  observe_gate(0x1ff);assert(!operation(7,reset,1));assert(c.completed_operations==1);cases++;
#endif
  assert(!start(1,0x1ff,0xa50000e1));assert(!operation(7,reset,1));assert(c.completed_operations==1&&c.touched);
  observe_gate(observations[i]);int before=io;assert(operation(7,reset,1)==expected);assert(io==before+1);before=io;
#if HISTORICAL_CONTROL
  assert(!c.fault);observe_gate(0x1ff);assert(!operation(7,reset,1));assert(c.completed_operations==2&&io>before);
#else
  assert(c.fault&&c.fault_error==expected&&c.completed_operations==1);unsigned int pending=c.pending_operation;
  observe_gate(0x1ff);assert(operation(7,reset,1)==expected);assert(io==before&&c.completed_operations==1&&c.pending_operation==pending);assert(reinit()==-114);
#endif
  cases++;
 }
 printf("PASS %u %s actual controller readiness cases; providers synthetic, hardware=0\n",cases,HISTORICAL_CONTROL?"historical replay":"fixed retained/cold");
 return 0;
}
'''
# fixture's original headers do not need libc output; test adds stdio only.
fixture='#include <stdio.h>\n'+fixture
flags=['-std=gnu11','-Wall','-Wextra','-Werror','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie','-I'+str(g/'test-include'),'-I'+str(g)];env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',UBSAN_OPTIONS='halt_on_error=1')
with tempfile.TemporaryDirectory()as name:
 p=Path(name);f=p/'fixture.c';f.write_text(fixture)
 for historical in [True,False]:
  source=p/('historical.c'if historical else'fixed.c');source.write_text(current[:a]+old+current[end:]if historical else current);binary=p/('old'if historical else'fixed')
  subprocess.run(['cc',*flags,'-DHISTORICAL_CONTROL='+str(int(historical)),str(source),str(f),'-o',str(binary)],check=True,timeout=60)
  subprocess.run([str(binary)],check=True,timeout=20,env=env)
