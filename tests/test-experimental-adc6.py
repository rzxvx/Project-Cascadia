from pathlib import Path
import json,shutil,subprocess,tempfile,unittest
HERE=Path(__file__).resolve().parents[1]/"experiments/p105-adc6"
STUB=r'''
#ifndef STUB_H
#define STUB_H
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <asm-generic/errno.h>
typedef uint8_t u8;typedef uint16_t u16;typedef uint32_t u32;typedef uint64_t u64;
typedef struct{u64 n;}atomic64_t;typedef struct{int n;}atomic_t;
static void atomic64_set(atomic64_t*a,u64 n){a->n=n;}static u64 atomic64_inc_return(atomic64_t*a){return ++a->n;}static u64 atomic64_read(const atomic64_t*a){return a->n;}
static void atomic_set(atomic_t*a,int n){a->n=n;}static int atomic_read(const atomic_t*a){return a->n;}
struct mutex{bool held;};static void mutex_init(struct mutex*m){m->held=false;}static void mutex_lock(struct mutex*m){assert(!m->held);m->held=true;}static void mutex_unlock(struct mutex*m){assert(m->held);m->held=false;}
static void*memchr_inv(const void*p,int c,size_t n){const unsigned char*b=p;for(size_t i=0;i<n;i++)if(b[i]!=(unsigned char)c)return(void*)(b+i);return NULL;}
#define IS_ERR_OR_NULL(p) (!(p)||(uintptr_t)(p)>=(uintptr_t)-4095)
#define ERR_PTR(e) ((void*)(intptr_t)(e))
struct regmap{u8 r[256];};int regmap_read(struct regmap*,unsigned int,unsigned int*);int regmap_write(struct regmap*,unsigned int,unsigned int);int regmap_bulk_read(struct regmap*,unsigned int,void*,size_t);
u64 ktime_get_ns(void);void usleep_range(unsigned long,unsigned long);
#endif
'''
FIXTURE=r'''
#include "p105-adc6-poll.c"
static struct p105_adc6*active;static struct regmap pmu;static unsigned int calls,fail_io,second_fail_io,epoch_io,holds,polls,busy_polls,starts,stops,sleeps,valids,valid_error_at,valid_invalidate_at;
static int valid_error,hold_error;static bool cleanup_mismatch,invalid_control,frozen_time,expire_after_sample,wrapped_time;
static u64 fake_ns,raw_delay_ns;static u16 fake_raw;
static int io(void){calls++;if(calls==epoch_io)p105_adc6_invalidate(active);return calls==fail_io||calls==second_fail_io?-EIO:0;}
int regmap_read(struct regmap*m,unsigned int r,unsigned int*v){assert(r==0x43);int e=io();if(!e){if(m->r[r]&0x20){polls++;if(polls>busy_polls)m->r[r]&=~0x20;}*v=m->r[r];if(invalid_control&&starts)*v=5;if(cleanup_mismatch&&stops)*v=6;}return e;}
int regmap_write(struct regmap*m,unsigned int r,unsigned int v){assert(r==0x43&&(v==0||v==0x26));if(v)starts++;else stops++;int e=io();if(!e)m->r[r]=v;return e;}
int regmap_bulk_read(struct regmap*m,unsigned int r,void*v,size_t n){(void)m;assert(r==0x44&&n==2);int e=io();if(!e){u8*p=v;p[0]=(fake_raw&15)|0x80;p[1]=fake_raw>>4;}fake_ns+=raw_delay_ns;if(expire_after_sample)p105_adc6_invalidate(active);return e;}
u64 ktime_get_ns(void){if(wrapped_time&&sleeps)return 0;return fake_ns;}
void usleep_range(unsigned long low,unsigned long high){assert(low==1000&&high==1100);sleeps++;if(!frozen_time)fake_ns+=1000000;}
static int valid(void*c,const struct p105_adc6_lease*l){(void)c;assert(l->owner_generation==123);valids++;if(valids==valid_invalidate_at)p105_adc6_invalidate(active);return !valid_error_at||valids==valid_error_at?valid_error:0;}
static int hold(void*c,const struct p105_adc6_lease*l,int e){(void)c;assert(l->owner_generation==123);assert(e);holds++;return hold_error;}
static const struct p105_adc6_ops ops={valid,hold};
static const struct p105_adc6_lease good={123,true,true};
static void setup(struct p105_adc6*a){memset(a,0,sizeof(*a));assert(!p105_adc6_init(a));active=a;a->pmu=&pmu;a->ops=&ops;memset(&pmu,0,sizeof(pmu));calls=fail_io=second_fail_io=epoch_io=holds=polls=busy_polls=starts=stops=sleeps=valids=valid_error_at=valid_invalidate_at=0;valid_error=hold_error=0;cleanup_mismatch=invalid_control=frozen_time=expire_after_sample=wrapped_time=false;fake_ns=1000000;raw_delay_ns=0;fake_raw=0xabc;}
static int capture(struct p105_adc6*a,const struct p105_adc6_lease*l,struct p105_adc6_sample*out){memset(out,0xa5,sizeof(*out));int e=p105_adc6_read(a,l,out);if(a&&!IS_ERR_OR_NULL(a))assert(!a->io_lock.held);if(e){const u8*b=(void*)out;for(unsigned int i=0;i<sizeof(*out);i++)assert(!b[i]);}return e;}
static void defaults(void){struct p105_adc6 a;struct p105_adc6_sample o;setup(&a);assert(capture(&a,&good,&o)==-EACCES);assert(!calls&&!holds);a.enabled=true;struct p105_adc6_lease bad=good;bad.exclusive_all_adc=false;assert(capture(&a,&bad,&o)==-EOPNOTSUPP);bad=good;bad.quiescence_proven=false;assert(capture(&a,&bad,&o)==-EOPNOTSUPP);assert(!calls&&!holds);a.pmu=ERR_PTR(-ENODEV);assert(capture(&a,&good,&o)==-EOPNOTSUPP);a.pmu=&pmu;a.ops=ERR_PTR(-ENODEV);assert(capture(&a,&good,&o)==-EOPNOTSUPP);}
static void success(void){struct p105_adc6 a;struct p105_adc6_sample o;for(unsigned int raw=0;raw<4096;raw+=257){setup(&a);a.enabled=true;fake_raw=raw;assert(!capture(&a,&good,&o));assert(o.raw==raw&&o.generation==1&&o.started_ns==1000000&&o.completed_ns==2000000);assert(calls==6&&starts==1&&stops==1&&!holds&&pmu.r[0x43]==0);}
 setup(&a);a.enabled=true;busy_polls=49;assert(!capture(&a,&good,&o));assert(sleeps==50&&o.completed_ns-o.started_ns==50000000);
}
static void errors(void){struct p105_adc6 a;struct p105_adc6_sample o;for(unsigned int i=1;i<=6;i++){setup(&a);a.enabled=true;fail_io=i;assert(capture(&a,&good,&o)==-EIO);assert(atomic_read(&a.unknown)&&holds==1&&a.retained_lease_valid&&a.retained_lease.owner_generation==123);assert(a.first_error==-EIO);if(i>1)assert(stops==1);unsigned int n=calls;assert(capture(&a,&good,&o)==-EACCES);assert(calls==n&&holds==1);}
 setup(&a);a.enabled=true;fail_io=2;second_fail_io=3;assert(capture(&a,&good,&o)==-EIO);assert(a.first_error==-EIO&&a.cleanup_error==-EIO&&a.retained_lease_valid);
 setup(&a);a.enabled=true;cleanup_mismatch=true;assert(capture(&a,&good,&o)==-EIO);assert(a.cleanup_error==-EIO);
 setup(&a);a.enabled=true;invalid_control=true;assert(capture(&a,&good,&o)==-ESTALE);assert(stops==1);
 setup(&a);a.enabled=true;valid_error=-ESTALE;assert(capture(&a,&good,&o)==-ESTALE);assert(!calls&&!holds);
}
static void timeout(void){struct p105_adc6 a;struct p105_adc6_sample o;setup(&a);a.enabled=true;busy_polls=100;assert(capture(&a,&good,&o)==-ETIMEDOUT);assert(sleeps==51&&stops==1&&pmu.r[0x43]==0&&atomic_read(&a.unknown));assert(a.retained_lease_valid&&holds==1);
 setup(&a);a.enabled=true;raw_delay_ns=50000000;assert(capture(&a,&good,&o)==-ETIMEDOUT);assert(stops==1&&holds==1);
 setup(&a);a.enabled=true;busy_polls=100;frozen_time=true;assert(capture(&a,&good,&o)==-ETIMEDOUT);assert(sleeps==64&&stops==1);
 setup(&a);a.enabled=true;wrapped_time=true;assert(capture(&a,&good,&o)==-ETIMEDOUT);assert(stops==1);
 setup(&a);a.enabled=true;pmu.r[0x43]=0x26;assert(capture(&a,&good,&o)==-EBUSY);assert(!starts&&!stops&&holds==1&&atomic_read(&a.unknown));
}
static void invalidation(void){struct p105_adc6 a;struct p105_adc6_sample o;for(unsigned int i=1;i<=6;i++){setup(&a);a.enabled=true;epoch_io=i;assert(capture(&a,&good,&o)==-ESTALE);if(i>1)assert(stops==1&&holds==1);}
 setup(&a);a.enabled=true;expire_after_sample=true;assert(capture(&a,&good,&o)==-ESTALE);assert(stops==1&&holds==1);
}
static void wrap(void){struct p105_adc6 a;struct p105_adc6_sample o;setup(&a);a.enabled=true;atomic64_set(&a.generation,UINT64_MAX);p105_adc6_invalidate(&a);assert(!atomic64_read(&a.generation)&&atomic_read(&a.unknown));assert(capture(&a,&good,&o)==-EACCES);assert(!calls);}
static void reinit(void){struct p105_adc6 a;struct p105_adc6_sample o;setup(&a);a.enabled=true;fail_io=3;hold_error=-EBUSY;assert(capture(&a,&good,&o)==-EIO);assert(a.hold_error==-EBUSY);struct p105_adc6 before=a;assert(p105_adc6_init(&a)==-EALREADY);assert(!memcmp(&a,&before,sizeof(a)));assert(capture(&a,&good,&o)==-EACCES);
 memset(&a,0,sizeof(a));a.ctx=(void*)1;before=a;assert(p105_adc6_init(&a)==-EINVAL);assert(!memcmp(&a,&before,sizeof(a)));assert(p105_adc6_init(NULL)==-EINVAL);assert(p105_adc6_init(ERR_PTR(-ENODEV))==-EINVAL);
}
static void positive_provider_errors(void){struct p105_adc6 a;struct p105_adc6_sample o;setup(&a);a.enabled=true;valid_error=1;assert(capture(&a,&good,&o)==-EPROTO);assert(!calls&&!holds);setup(&a);a.enabled=true;valid_error=1;valid_error_at=3;assert(capture(&a,&good,&o)==-EPROTO);assert(starts==1&&stops==1&&a.first_error==-EPROTO&&atomic_read(&a.unknown)&&holds==1);setup(&a);a.enabled=true;fail_io=2;hold_error=1;assert(capture(&a,&good,&o)==-EIO&&a.first_error==-EIO&&a.hold_error==-EPROTO);}
static void validator_epoch(void){struct p105_adc6 a;struct p105_adc6_sample o;setup(&a);a.enabled=true;valid_invalidate_at=6;assert(capture(&a,&good,&o)==-ESTALE);assert(atomic_read(&a.unknown)&&holds==1&&stops==1);}
int main(int argc,char**argv){assert(argc==2);switch(atoi(argv[1])){case 0:defaults();break;case 1:success();break;case 2:errors();break;case 3:timeout();break;case 4:invalidation();break;case 5:reinit();break;case 6:wrap();break;case 7:positive_provider_errors();break;case 8:validator_epoch();break;default:abort();}return 0;}
'''
class C(unittest.TestCase):
 @classmethod
 def setUpClass(c):
  if not shutil.which('gcc'):raise unittest.SkipTest('GCC required')
  c.tmp=tempfile.TemporaryDirectory();p=Path(c.tmp.name);(p/'linux').mkdir();(p/'stub.h').write_text(STUB)
  for h in ('atomic.h','mutex.h','regmap.h','types.h','errno.h','string.h','delay.h','err.h','ktime.h'):(p/'linux'/h).write_text('#include "stub.h"\n')
  (p/'fixture.c').write_text(FIXTURE);c.exe=p/'fixture';subprocess.run(['gcc','-std=gnu11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie','-I',str(p),'-I',str(HERE),str(p/'fixture.c'),'-o',str(c.exe)],check=True,text=True)
 @classmethod
 def tearDownClass(c):c.tmp.cleanup()
 def execute(self,n):subprocess.run([str(self.exe),str(n)],check=True,text=True)
 def test_invalidation_during_final_validator(self):self.execute(8)
 def test_default_denied_and_invalid_pointers(self):self.execute(0)
 def test_raw_success_and_exact_deadline(self):self.execute(1)
 def test_every_io_error_and_cleanup_mismatch(self):self.execute(2)
 def test_timeout_cleanup_and_unknown_latch(self):self.execute(3)
 def test_every_epoch_and_after_sample_invalidation(self):self.execute(4)
 def test_reinit_preserves_unknown_and_failed_lease(self):self.execute(5)
 def test_positive_provider_codes_before_after_start_and_hold(self):self.execute(7)
 def test_wrapped_generation_denies_without_io(self):self.execute(6)
if __name__=='__main__':unittest.main(verbosity=2)
