# SPDX-License-Identifier: GPL-2.0-only
"""Actual current-core ordering/mode checks; all PMU I/O is synthetic.

This verifies software register transactions, not physical current, calibrated
protection, inherited analog retention or a hardware-safe activation sequence.
"""
import hashlib
import os
from pathlib import Path
import resource
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(os.environ.get('CASCADIA_CURRENT_ROOT', Path(__file__).resolve().parents[1] / 'experiments/p105-current'))
# Exact pre-reorder GPL current-core function. Kept only as a negative control.
HISTORICAL = '''int d1946_input_limit(const struct d1946_io *io, unsigned int ma)
{
\tint code = d1946_input_profile(ma), ret;
\tif (code < 0)
\t\treturn code;
\tret = d1946_update_checked(io, 0x1b, 0x04, ma ? 0 : 0x04);
\tif (ret)
\t\treturn ret;
\tret = d1946_update_checked(io, 0x1c, 0x80, ma ? 0 : 0x80);
\tif (ret)
\t\treturn ret;
\treturn d1946_update_checked(io, 0x3d, 0xff, code);
}'''

C = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "d1946-current-core.h"
struct fake {
 u8 regs[256];unsigned int calls,writes,fail_at,corrupt_at;
 unsigned int trace_reg[16],trace_value[16],unsafe_releases,target;
};
static int rd(void *ctx,unsigned int reg,u8 *out,unsigned int n){
 struct fake *f=ctx;assert(n==1&&reg<256);if(++f->calls==f->fail_at)return -EREMOTEIO;
 *out=f->regs[reg];if(f->calls==f->corrupt_at)*out^=1;return 0;
}
static int wr(void *ctx,unsigned int reg,const u8 *in,unsigned int n){
 struct fake *f=ctx;assert(n==1&&reg<256);if(++f->calls==f->fail_at)return -EREMOTEIO;
 /* Inspect exact state at each actual suspend-release write. */
 if(((reg==0x1b&&!(in[0]&4))||(reg==0x1c&&!(in[0]&0x80)))&&f->regs[0x3d]!=f->target)f->unsafe_releases++;
 assert(f->writes<16);f->trace_reg[f->writes]=reg;f->trace_value[f->writes]=*in;f->writes++;f->regs[reg]=*in;return 0;
}
static void init(struct fake *f,unsigned int target,bool positive){
 memset(f,0,sizeof(*f));f->target=target;
 f->regs[0x1b]=positive?0xa5:0xa1;f->regs[0x1c]=positive?0x93:0x13;f->regs[0x3d]=158;
 f->regs[0x40]=0xc7;f->regs[0x3f]=0x7f; /* already charge-inhibited */
}
static void order(void){
 const unsigned int ma[]={100,500,1000,1500,2100};
 for(unsigned int i=0;i<5;i++){
  struct fake f;unsigned int code=d1946_input_profile(ma[i]);init(&f,code,true);
  struct d1946_io io={&f,rd,wr};assert(!d1946_input_limit(&io,ma[i]));
  fprintf(stderr,"input%u target%u unsafe_suspend_release_writes%u\n",ma[i],code,f.unsafe_releases);
  assert(!f.unsafe_releases&&"positive_limit_before_suspend_release");
  assert(f.regs[0x3d]==code&&f.regs[0x1b]==0xa1&&f.regs[0x1c]==0x13);
  assert(f.regs[0x40]==0xc7&&f.regs[0x3f]==0x7f); /* no mode/charge changes */
  if(code!=158)assert(f.writes==3&&f.trace_reg[0]==0x3d&&f.trace_reg[1]==0x1b&&f.trace_reg[2]==0x1c);
 }
 struct fake f;init(&f,0,false);struct d1946_io io={&f,rd,wr};
 assert(!d1946_input_limit(&io,0));
 assert(f.writes==3&&f.trace_reg[0]==0x1b&&f.trace_reg[1]==0x1c&&f.trace_reg[2]==0x3d);
 assert(f.regs[0x1b]==0xa5&&f.regs[0x1c]==0x93&&!f.regs[0x3d]);
 puts("positive-before-resume/zero-suspend-first actual core PASS");
}
static void faults(void){
 for(unsigned int positive=0;positive<=1;positive++){
  struct fake f;init(&f,positive?34:0,positive);struct d1946_io io={&f,rd,wr};
  assert(!d1946_input_limit(&io,positive?500:0));unsigned int length=f.calls;
  for(unsigned int at=1;at<=length;at++){
   init(&f,positive?34:0,positive);f.fail_at=at;
   assert(d1946_input_limit(&io,positive?500:0)==-EREMOTEIO&&f.calls==at);
   if(positive)assert(!f.unsafe_releases);
   /* No speculative recovery/retry after a failed transaction. */
  }
 }
 struct fake f;init(&f,34,true);struct d1946_io io={&f,rd,wr};f.corrupt_at=3;
 assert(d1946_input_limit(&io,500)==-EIO&&f.calls==3&&f.writes==1);
 assert(f.regs[0x1b]==0xa5&&f.regs[0x1c]==0x93); /* both remain suspended */
 puts("actual input every-transport-failure/readback-stop PASS");
}
static void handoff(void){
 struct fake f={0};struct d1946_io io={&f,rd,wr};
 for(unsigned int mode=0;mode<256;mode++)for(unsigned int system=0;system<256;system++){
  f.calls=f.writes=0;f.regs[0x40]=mode;f.regs[0x1b]=system;
  int want=(mode&0x40)&&!(system&0x40)?0:-EIO;
  assert(d1946_input_handoff(&io)==want&&f.calls==2&&!f.writes);
  assert(f.regs[0x40]==mode&&f.regs[0x1b]==system);
 }
 for(unsigned int at=1;at<=2;at++){
  memset(&f,0,sizeof(f));f.regs[0x40]=0x40;f.fail_at=at;
  assert(d1946_input_handoff(&io)==-EREMOTEIO&&f.calls==at&&!f.writes);
 }
 assert(d1946_input_handoff(NULL)==-EINVAL);
 io.read=NULL;assert(d1946_input_handoff(&io)==-EINVAL);
 puts("all65536-mode-pairs/read-only/error propagation actual core PASS");
}
int main(int argc,char **argv){assert(argc==2);if(!strcmp(argv[1],"order"))order();else if(!strcmp(argv[1],"faults"))faults();else handoff();}
'''


def extract(text, name):
    start = text.index('int ' + name + '(')
    brace = text.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


class Handoff(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which('gcc'):
            raise unittest.SkipTest('gcc required')
        cls.temp = tempfile.TemporaryDirectory()
        path = Path(cls.temp.name)
        source = (ROOT / 'd1946-current-core.c').read_text()
        assert hashlib.sha256(HISTORICAL.encode()).hexdigest() == '9f7c02a6564460280e5e6e98b914ea01a506ce946a48ac2fce32c01b3e9c7f71'
        current = extract(source, 'd1946_input_limit')
        assert current != HISTORICAL
        (path / 'fixture.c').write_text(C)
        (path / 'old-core.c').write_text(source.replace(current, HISTORICAL, 1))
        cls.binaries = {}
        for name, core in [('fixed', ROOT / 'd1946-current-core.c'), ('historical', path / 'old-core.c')]:
            target = path / name
            subprocess.run(['gcc', '-DCASCADIA_OFFLINE', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fno-pie', '-no-pie', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-I', str(ROOT), str(core), str(path / 'fixture.c'), '-o', str(target)], check=True, timeout=60)
            cls.binaries[name] = target
        print('historical input function SHA256=' + hashlib.sha256(HISTORICAL.encode()).hexdigest())

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_fixed(self, case):
        self.assertIn('PASS', subprocess.check_output([str(self.binaries['fixed']), case], text=True, timeout=10))

    def test_actual_positive_and_zero_transaction_order(self): self.run_fixed('order')
    def test_actual_every_transport_failure_and_readback(self): self.run_fixed('faults')
    def test_actual_readonly_handoff_all_mode_pairs(self): self.run_fixed('handoff')

    def test_historical_resume_exposes_previous_input_code(self):
        def no_core(): resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        result = subprocess.run([str(self.binaries['historical']), 'order'], text=True, capture_output=True, preexec_fn=no_core, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('unsafe_suspend_release_writes2', result.stderr)
        self.assertIn('positive_limit_before_suspend_release', result.stderr)
        print('historical control: ' + result.stderr.splitlines()[0])


if __name__ == '__main__': unittest.main()
