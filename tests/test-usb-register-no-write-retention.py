#!/usr/bin/env python3
"""Actual no-write membership retention; synthetic registers/epochs, no device."""
from pathlib import Path
import hashlib,json,subprocess,tempfile,os,resource
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
r=Path(__file__).resolve().parents[1];g=r/'experiments/p105-usb-registers';current=(g/'cascadia-usb-registers.c').read_text();block=(g/'historical-no-write-fault.c').read_text();v=json.loads((g/'no-write-retention-provenance.json').read_text());assert hashlib.sha256(block.encode()).hexdigest()==v['historical_fault_SHA256'];assert hashlib.sha256(current.encode()).hexdigest()==v['candidate_source_SHA256'];assert not v['normal_runtime_activation']and not v['hardware_access'];a=current.index('static int fault(struct cascadia_usb_registers *r, int ret)\n{');end=current.index('\nstatic int write_reg(',a);historical=current[:a]+block+current[end:];assert hashlib.sha256(historical.encode()).hexdigest()==v['previous_source_SHA256']
MAIN=r'''#include <stdio.h>
static void restore_validator(void) {backend.validate_epoch=validate;epoch_error=0;stale_validate=0;}
static int request(int tuning) {return tuning?tune(2,7,1,2,1,1,1):update(2,7,2);}
static void lose(int which) {if(which==0)backend.validate_epoch=NULL;else epoch_error=which==1?-116:1;}
int main(void) {
 const unsigned int masks[]={1,2,4,8},offsets[]={0x1084,0x1088,0x1090,0x1094},owners[]={1,15};unsigned int cases=0;
 for(unsigned int m=0;m<4;m++)for(unsigned int o=0;o<2;o++)for(int lost=0;lost<3;lost++)for(int tuning=0;tuning<2;tuning++) {
  int expected=lost==0?-19:lost==1?-116:-5;unsigned int owner=owners[o];
  assert(!start(1,1,1));seed(1,offsets[m],0x3ff);assert(!update(owner,7,masks[m]));
  assert(backend.masks[owner]==masks[m]&&!backend.touched&&!writes);int before=io_count;
  unsigned int pending_owner=backend.pending_owner;u32 pending_mask=backend.pending_mask;
  lose(lost);assert(request(tuning)==expected);assert(io_count==before&&backend.masks[owner]==masks[m]&&!backend.masks[2]);restore_validator();
#if HISTORICAL_CONTROL
  (void)pending_owner;(void)pending_mask;assert(!backend.fault);assert(!request(tuning));
#else
  assert(backend.fault&&backend.fault_error==expected);assert(request(tuning)==expected);
  assert(io_count==before&&backend.masks[owner]==masks[m]&&!backend.masks[2]);
  assert(backend.pending_owner==pending_owner&&backend.pending_mask==pending_mask);assert(reinit()==-114);
#endif
  cases++;
 }
#if !HISTORICAL_CONTROL
 for(int lost=0;lost<3;lost++)for(int tuning=0;tuning<2;tuning++) {
  int expected=lost==0?-19:lost==1?-116:-5;
  assert(!start(1,1,1));lose(lost);assert(request(tuning)==expected);assert(!backend.fault&&!io_count&&!writes&&!backend.touched);
  restore_validator();assert(!request(tuning));cases++;
 }
#endif
 printf("PASS %u %s actual clock no-write-retention cases; providers fake, hardware=0\n",cases,HISTORICAL_CONTROL?"historical":"fixed/cold");return 0;
}
'''

flags=['-std=gnu11','-Wall','-Wextra','-Werror','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie','-I'+str(g/'test-include'),'-I'+str(g)];env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',UBSAN_OPTIONS='halt_on_error=1')
with tempfile.TemporaryDirectory()as name:
 p=Path(name);fixture=p/'fixture.c';fixture.write_text((g/'backend-fixture.c').read_text()+MAIN)
 for old in [True,False]:
  source=p/('historical.c'if old else'fixed.c');source.write_text(historical if old else current);exe=p/('old'if old else'fixed')
  subprocess.run(['cc',*flags,'-DHISTORICAL_CONTROL='+str(int(old)),str(source),str(fixture),'-o',str(exe)],check=True,timeout=60)
  subprocess.run([str(exe)],check=True,timeout=20,env=env)
