#!/usr/bin/env python3
"""Actual reset peer retention with fake adoption/lease/epoch/register providers."""
from pathlib import Path
import hashlib,json,subprocess,tempfile,os,resource
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
r=Path(__file__).resolve().parents[1];g=r/'experiments/p105-usb-reset';current=(g/'cascadia-reset-client.c').read_text();tail=(g/'historical-peer-failed.c').read_text();v=json.loads((g/'adopted-peer-provenance.json').read_text());assert hashlib.sha256(tail.encode()).hexdigest()==v['historical_failed_tail_SHA256'];assert hashlib.sha256(current.encode()).hexdigest()==v['candidate_source_SHA256'];assert not v['normal_runtime_activation']and not v['hardware_access'];a=current.index('failed:\n');historical=current[:a]+tail;assert hashlib.sha256(historical.encode()).hexdigest()==v['previous_source_SHA256']
MAIN=r'''
#include <stdio.h>
static void lose_provider_or_epoch(int which) {
 if(which==0)c.validate_epoch=NULL;
 else if(which==1)c.temporary_power_mask=NULL;
 else if(which==2)epoch_error=-116;
 else stale_validation=2;
}
static void restore_providers(void) {
 c.validate_epoch=validate;c.temporary_power_mask=power;epoch_error=0;stale_validation=0;
}
int main(void) {
 unsigned int kinds[]={0,1,5},peers[]={1,15},cases=0;
 for(unsigned int peer_index=0;peer_index<2;peer_index++)
 for(unsigned int j=0;j<3;j++)for(int missing=0;missing<5;missing++) {
  unsigned int kind=kinds[j],peer=peers[peer_index];int expected=missing>=2?-116:-19,add=missing!=3;
  assert(!start(1,0xef4c0000));c.memberships[peer]=1U<<kind;c.owner_epochs[peer]=7;
  lose_provider_or_epoch(missing);
  assert(update(7,2,1,add)==expected);
  assert(!io_count&&c.memberships[peer]==(1U<<kind)&&c.owner_epochs[peer]==7&&!c.memberships[2]&&!c.touched);
  unsigned int pending_owner=c.pending_owner;u8 pending_mask=c.pending_mask;
  restore_providers();
#if HISTORICAL_CONTROL
  (void)pending_owner;(void)pending_mask;assert(!c.fault);assert(!update(7,2,1,add));
  assert(c.memberships[2]==(add?2:0));assert(add?io_count>0:io_count==0);
#else
  assert(c.fault&&c.fault_error==expected);assert(update(7,2,1,add)==expected);
  assert(!io_count&&c.memberships[peer]==(1U<<kind)&&c.owner_epochs[peer]==7&&!c.memberships[2]);
  assert(c.pending_owner==pending_owner&&c.pending_mask==pending_mask);assert(reinit()==-114);
#endif
  cases++;
 }
#if !HISTORICAL_CONTROL
 for(int missing=0;missing<5;missing++) {
  int expected=missing>=2?-116:-19,add=missing!=3;
  assert(!start(1,0xef4c0000));lose_provider_or_epoch(missing);
  assert(update(7,2,1,add)==expected);assert(!c.fault&&!io_count&&!c.touched);
  restore_providers();assert(!update(7,2,1,add));assert(c.memberships[2]==(add?2:0));cases++;
 }
#endif
 printf("PASS %u %s actual reset adopted-peer cases; providers fake, hardware=0\n",cases,HISTORICAL_CONTROL?"historical":"fixed/cold");return 0;
}
'''
flags=['-std=gnu11','-Wall','-Wextra','-Werror','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie','-I'+str(g/'test-include'),'-I'+str(g)];env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',UBSAN_OPTIONS='halt_on_error=1')
with tempfile.TemporaryDirectory()as name:
 p=Path(name);fixture=p/'fixture.c';fixture.write_text((g/'reset-fixture.c').read_text()+MAIN)
 for old in [True,False]:
  source=p/('historical.c'if old else'fixed.c');source.write_text(historical if old else current);exe=p/('old'if old else'fixed')
  subprocess.run(['cc',*flags,'-DHISTORICAL_CONTROL='+str(int(old)),str(source),str(fixture),'-o',str(exe)],check=True,timeout=60)
  subprocess.run([str(exe)],check=True,timeout=20,env=env)
