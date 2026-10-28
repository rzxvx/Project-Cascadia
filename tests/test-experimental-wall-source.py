"""Actual complete candidate C with explicit regmap/owner fakes; no hardware grants."""
from pathlib import Path
import hashlib,json,shutil,subprocess,tempfile,unittest
HERE=Path(__file__).resolve().parents[1]/'experiments/p105-wall-source'
STUB=r'''
#ifndef STUB_H
#define STUB_H
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#define IS_ERR_OR_NULL(p) (!(p)||(uintptr_t)(p)>=(uintptr_t)-4095)
typedef uint8_t u8;typedef uint16_t u16;typedef uint32_t u32;typedef uint64_t u64;
typedef struct {uint64_t n;}atomic64_t;typedef struct {int n;}atomic_t;
static void atomic64_set(atomic64_t*a,uint64_t n){a->n=n;}static uint64_t atomic64_inc_return(atomic64_t*a){return ++a->n;}static uint64_t atomic64_read(const atomic64_t*a){return a->n;}
static void atomic_set(atomic_t*a,int n){a->n=n;}static int atomic_read(const atomic_t*a){return a->n;}
struct mutex{bool held;};static void mutex_init(struct mutex*m){m->held=false;}static void mutex_lock(struct mutex*m){assert(!m->held);m->held=true;}static void mutex_unlock(struct mutex*m){assert(m->held);m->held=false;}
struct regmap{bool pmu;uint8_t r[256];};
int regmap_read(struct regmap*,unsigned int,unsigned int*);int regmap_write(struct regmap*,unsigned int,unsigned int);int regmap_bulk_read(struct regmap*,unsigned int,void*,size_t);
#endif
'''
FIXTURE=r'''
#include "p105-wall-source.c"
static struct p105_wall_source *active;
static unsigned int io_calls,fail_io,epoch_io,orientation_io,adc_calls,release_calls,acquires,valids,writes,valid_invalidate_at;
static int adc_error,settle_error,acquire_error,release_error,validate_error;
static bool proof,wrong_readback;
static u16 raw[3];
static int check(void){io_calls++;if(io_calls==epoch_io)p105_wall_invalidate(active,false);return io_calls==fail_io?-EIO:0;}
int regmap_read(struct regmap*m,unsigned int r,unsigned int*v){int e=check();if(io_calls==orientation_io&&!m->pmu&&r==12)m->r[12]^=0x40;if(!e)*v=m->r[r]+(wrong_readback&&(r==1||r==3));return e;}
int regmap_write(struct regmap*m,unsigned int r,unsigned int v){int e=check();assert(!m->pmu);writes++;if(!e)m->r[r]=v;return e;}
int regmap_bulk_read(struct regmap*m,unsigned int r,void*v,size_t n){assert(m->pmu&&r==10&&n==6);int e=check();if(!e)memcpy(v,m->r+r,n);return e;}
static int acquire(void*c,struct p105_wall_lease*l){(void)c;acquires++;l->generation=123;l->checked_restore_available=proof;return acquire_error;}
static int validate(void*c,const struct p105_wall_lease*l){(void)c;assert(l->generation==123);valids++;if(valids==valid_invalidate_at)p105_wall_invalidate(active,false);return validate_error;}
static int adc(void*c,const struct p105_wall_lease*l,u16*v){(void)c;assert(l->generation==123);assert(adc_calls<3);if(adc_error)return adc_error;*v=raw[adc_calls++];return 0;}
static int settle(void*c,const struct p105_wall_lease*l,u32 n){(void)c;assert(l->generation==123);assert(n==2000||n==40000);return settle_error;}
/* Synthetic owner release only, NOT proof that real register semantics permit restore. */
static int release(void*c,const struct p105_wall_lease*l){(void)c;assert(l->generation==123);release_calls++;return release_error;}
static const struct p105_wall_ops ops={acquire,validate,adc,settle,release};
static struct regmap pmu,tristar;
static void setup(struct p105_wall_source*s){
 memset(s,0,sizeof(*s));assert(!p105_wall_init(s));active=s;memset(&pmu,0,sizeof(pmu));memset(&tristar,0,sizeof(tristar));pmu.pmu=true;pmu.r[10]=8;tristar.r[12]=0x80;tristar.r[6]=4;
 s->pmu=&pmu;s->tristar=&tristar;s->ops=&ops;s->ctx=NULL;proof=true;wrong_readback=false;
 io_calls=fail_io=epoch_io=orientation_io=adc_calls=release_calls=acquires=valids=writes=valid_invalidate_at=0;adc_error=settle_error=acquire_error=release_error=validate_error=0;
 raw[0]=1802;raw[1]=2457;raw[2]=300;
}
static void assert_zero(struct p105_wall_observation*o){unsigned char*p=(void*)o;for(unsigned int i=0;i<sizeof(*o);i++)assert(p[i]==0);}
static int capture(struct p105_wall_source*s,struct p105_wall_observation*o){memset(o,0xa5,sizeof(*o));int e=p105_wall_capture(s,o);assert(!s->io_lock.held);if(e)assert_zero(o);return e;}
static void defaults(void){
 struct p105_wall_source s;struct p105_wall_observation o;setup(&s);assert(capture(&s,&o)==-EACCES);assert(!acquires&&!io_calls&&!release_calls);
 s.enabled=true;proof=false;assert(capture(&s,&o)==-EOPNOTSUPP);assert(acquires==1&&release_calls==1&&!io_calls&&!writes);
 setup(&s);s.enabled=true;s.ops=NULL;assert(capture(&s,&o)==-EOPNOTSUPP);assert(!io_calls);
 setup(&s);s.enabled=true;acquire_error=-EBUSY;assert(capture(&s,&o)==-EBUSY);assert(!release_calls&&!io_calls);
}
static void positive_observations_only(void){
 struct p105_wall_source s;struct p105_wall_observation o;setup(&s);s.enabled=true;assert(!capture(&s,&o));assert(o.source_type==4&&o.divider_id==4&&o.first_scalar==2199&&o.second_scalar==2999);assert(o.generation==1&&release_calls==1&&adc_calls==2&&writes==8);
 setup(&s);s.enabled=true;tristar.r[12]=0xc0;assert(!capture(&s,&o));assert(o.divider_id==4);
 setup(&s);s.enabled=true;raw[0]=raw[1]=0;assert(!capture(&s,&o));assert(o.source_type==5&&o.third_sample&&o.third_scalar==366&&adc_calls==3&&writes==12);
}
static void all_errors(void){
 struct p105_wall_source s;struct p105_wall_observation o;setup(&s);s.enabled=true;assert(!capture(&s,&o));unsigned int n=io_calls;assert(n>30);
 for(unsigned int i=1;i<=n;i++){setup(&s);s.enabled=true;fail_io=i;assert(capture(&s,&o)==-EIO);assert(release_calls==1);}
 setup(&s);s.enabled=true;adc_error=-ETIMEDOUT;assert(capture(&s,&o)==-ETIMEDOUT);assert(release_calls==1);
 setup(&s);s.enabled=true;raw[0]=4096;assert(capture(&s,&o)==-ERANGE);
 setup(&s);s.enabled=true;settle_error=-EINTR;assert(capture(&s,&o)==-EINTR);
 setup(&s);s.enabled=true;validate_error=-ESTALE;assert(capture(&s,&o)==-ESTALE);assert(!io_calls&&release_calls==1);
 setup(&s);s.enabled=true;wrong_readback=true;assert(capture(&s,&o)==-EIO);assert(release_calls==1);
}
static void removals(void){
 struct p105_wall_source s;struct p105_wall_observation o;setup(&s);s.enabled=true;assert(!capture(&s,&o));unsigned int n=io_calls;
 for(unsigned int i=1;i<=n;i++){setup(&s);s.enabled=true;epoch_io=i;assert(capture(&s,&o)==-ESTALE);assert(release_calls==1);}
 setup(&s);s.enabled=true;orientation_io=18;assert(capture(&s,&o)==-ESTALE);assert(release_calls==1);
 for(unsigned int value=0;value<8;value++){setup(&s);s.enabled=true;pmu.r[10]=value;assert(capture(&s,&o)==-ENODATA);assert(!writes);}
 for(unsigned int value=8;value<=32;value*=2){setup(&s);s.enabled=true;pmu.r[11]=value;assert(capture(&s,&o)==-ENODATA);assert(!writes);}
 setup(&s);s.enabled=true;tristar.r[12]=0;assert(capture(&s,&o)==-EAGAIN);assert(!writes);
 setup(&s);s.enabled=true;tristar.r[6]=0;assert(capture(&s,&o)==-EAGAIN);assert(!writes);
}
static void restore_fault(void){
 struct p105_wall_source s;struct p105_wall_observation o;setup(&s);s.enabled=true;release_error=-EIO;assert(capture(&s,&o)==-EIO);assert(atomic_read(&s.fault)&&atomic64_read(&s.generation)==2);
 unsigned int n=io_calls,a=acquires;assert(capture(&s,&o)==-EACCES);assert(io_calls==n&&acquires==a&&release_calls==1);
}
static void lifetime(void){
 struct p105_wall_source s={0},saved;struct p105_wall_observation o;
 assert(p105_wall_init(NULL)==-EINVAL);assert(p105_wall_init((void*)-EIO)==-EINVAL);assert(p105_wall_capture((void*)-EIO,&o)==-EINVAL);assert_zero(&o);assert(p105_wall_capture(&s,(void*)-EIO)==-EINVAL);assert(p105_wall_classify(0,0,false,0,(void*)-EIO)==-EINVAL);p105_wall_invalidate((void*)-EIO,true);memset(&o,0xa5,sizeof(o));assert(p105_wall_capture(&s,&o)==-EINVAL);assert_zero(&o);
 p105_wall_invalidate(NULL,true);p105_wall_invalidate(&s,true);assert(!s.initialized);
 setup(&s);assert(p105_wall_init(&s)==-EALREADY);assert(s.initialized&&!s.enabled&&atomic64_read(&s.generation)==1);
 for(unsigned int i=0;i<3;i++){setup(&s);s.enabled=true;if(i==0)s.pmu=(void*)-EIO;else if(i==1)s.tristar=(void*)-EIO;else s.ops=(void*)-EIO;assert(capture(&s,&o)==-EOPNOTSUPP);assert(!io_calls&&!acquires);}
 setup(&s);s.enabled=true;release_error=-EIO;assert(capture(&s,&o)==-EIO);assert(s.lease_uncertain&&s.held_error==-EIO&&s.held_lease.generation==123);assert(s.held_lease.orientation_seen);
 saved=s;assert(p105_wall_init(&s)==-EALREADY);assert(!memcmp(&s,&saved,sizeof(s)));assert(capture(&s,&o)==-EACCES);assert(release_calls==1);
 setup(&s);s.enabled=true;atomic64_set(&s.generation,UINT64_MAX);p105_wall_invalidate(&s,false);assert(atomic_read(&s.fault)&&!atomic64_read(&s.generation));p105_wall_invalidate(&s,false);assert(atomic_read(&s.fault));assert(capture(&s,&o)==-EACCES);assert(!io_calls&&!acquires);
}
static void positive_provider_errors(void){struct p105_wall_source s;struct p105_wall_observation o;setup(&s);s.enabled=true;acquire_error=1;assert(capture(&s,&o)==-EPROTO);assert(!io_calls&&!release_calls&&s.lease_uncertain&&s.held_lease.generation==123&&s.held_error==-EPROTO&&atomic_read(&s.fault));acquire_error=0;assert(capture(&s,&o)==-EACCES&&acquires==1);setup(&s);s.enabled=true;validate_error=1;assert(capture(&s,&o)==-EPROTO&&release_calls==1&&!io_calls);setup(&s);s.enabled=true;settle_error=1;assert(capture(&s,&o)==-EPROTO&&release_calls==1);setup(&s);s.enabled=true;adc_error=1;assert(capture(&s,&o)==-EPROTO&&release_calls==1);setup(&s);s.enabled=true;release_error=1;assert(capture(&s,&o)==-EPROTO&&s.held_error==-EPROTO&&s.lease_uncertain&&atomic_read(&s.fault));unsigned int n=io_calls;release_error=0;assert(capture(&s,&o)==-EACCES&&io_calls==n);}
static void validator_epoch(void){struct p105_wall_source s;struct p105_wall_observation o;setup(&s);s.enabled=true;valid_invalidate_at=3;assert(capture(&s,&o)==-ESTALE);assert(io_calls==3&&!writes&&release_calls==1);}
int main(int argc,char**argv){
 if(argc==4){struct p105_wall_observation o;u32 a=strtoul(argv[1],NULL,0),b=strtoul(argv[2],NULL,0),c=strtoul(argv[3],NULL,0);int e=p105_wall_classify(a,b,true,c,&o);printf("%d %u %u\n",e,o.source_type,o.divider_id);return 0;}
 assert(argc==2);switch(atoi(argv[1])){case 0:defaults();break;case 1:positive_observations_only();break;case 2:all_errors();break;case 3:removals();break;case 4:restore_fault();break;case 5:lifetime();break;case 6:positive_provider_errors();break;case 7:validator_epoch();break;default:abort();}return 0;
}
'''
class CTests(unittest.TestCase):
 @classmethod
 def setUpClass(c):
  if not shutil.which('gcc'):raise unittest.SkipTest('GCC required')
  c.tmp=tempfile.TemporaryDirectory();p=Path(c.tmp.name);(p/'linux').mkdir();(p/'stub.h').write_text(STUB)
  for h in ('atomic.h','mutex.h','regmap.h','types.h','errno.h','string.h','err.h'):(p/'linux'/h).write_text('#include <asm-generic/errno.h>\n' if h=='errno.h' else '#include "stub.h"\n')
  (p/'fixture.c').write_text(FIXTURE);c.exe=p/'fixture';subprocess.run(['gcc','-std=gnu11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie','-I',str(p),'-I',str(HERE),str(p/'fixture.c'),'-o',str(c.exe)],check=True,text=True)
 @classmethod
 def tearDownClass(c):c.tmp.cleanup()
 def execute(self,n):subprocess.run([str(self.exe),str(n)],check=True,capture_output=True,text=True)
 def test_invalidation_during_prewrite_validator(self):self.execute(7)
 def test_default_off_unproved_restore_no_io(self):self.execute(0)
 def test_synthetic_observations_no_current_authorization(self):self.execute(1)
 def test_each_transaction_error_and_invalid_adc(self):self.execute(2)
 def test_every_epoch_change_and_absent_fault_status(self):self.execute(3)
 def test_failed_release_latches_fault_no_retry(self):self.execute(4)
 def test_checked_init_retained_failed_lease_error_resources_and_generation_wrap(self):self.execute(5)
 def test_positive_provider_results_reject_and_ambiguous_acquisition_latches(self):self.execute(6)
 def test_recorded_original_instruction_classifier_oracle(self):
  # Normalized observations recorded by actual pinned original instructions.
  oracle_path=HERE/'original-classifier-oracle.json'
  data=oracle_path.read_bytes()
  self.assertEqual(hashlib.sha256(data).hexdigest(),'5acc9e023db6ed319110ce28b52ff2e8e64d97d75cac6a3ea2330a6f3672a525')
  oracle=json.loads(data)
  self.assertEqual(oracle['image_sha256'],'7dc177231f52e7032cd10ddb255c3a8fd6f62c33c5e4d731eda7e5e0a8b8fdcd')
  self.assertEqual(oracle['case_count'],72)
  self.assertEqual(len(oracle['cases']),72)
  for row in oracle['cases']:
   encoded=row['encoded_samples'];scaled=row['scaled_samples']
   self.assertEqual(scaled,[x*5000>>12 for x in encoded])
   r=subprocess.run([str(self.exe),*[str(x) for x in scaled]],check=True,capture_output=True,text=True)
   self.assertEqual(list(map(int,r.stdout.split())),[0,row['source_type'],row['divider_id']])
if __name__=='__main__':unittest.main(verbosity=2)
