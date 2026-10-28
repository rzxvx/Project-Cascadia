from pathlib import Path
import hashlib,json,shutil,subprocess,tempfile,unittest
H=Path(__file__).resolve().parents[1]/"experiments/p105-bank0"
STUB=r'''
#ifndef STUB_H
#define STUB_H
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <asm-generic/errno.h>
typedef uint8_t u8;typedef uint16_t u16;typedef uint32_t u32;typedef uint64_t u64;
typedef struct{u64 n;}atomic64_t;typedef struct{int n;}atomic_t;
static void atomic64_set(atomic64_t*a,u64 n){__atomic_store_n(&a->n,n,__ATOMIC_SEQ_CST);}static u64 atomic64_inc_return(atomic64_t*a){return __atomic_add_fetch(&a->n,1,__ATOMIC_SEQ_CST);}static void atomic64_inc(atomic64_t*a){atomic64_inc_return(a);}static u64 atomic64_read(const atomic64_t*a){return __atomic_load_n(&a->n,__ATOMIC_SEQ_CST);}
static void atomic_set(atomic_t*a,int n){__atomic_store_n(&a->n,n,__ATOMIC_SEQ_CST);}static int atomic_read(const atomic_t*a){return __atomic_load_n(&a->n,__ATOMIC_SEQ_CST);}
struct mutex{pthread_mutex_t lock;bool held;};static void mutex_init(struct mutex*m){assert(!pthread_mutex_init(&m->lock,NULL));m->held=false;}static void mutex_lock(struct mutex*m){assert(!pthread_mutex_lock(&m->lock));assert(!m->held);m->held=true;}static void mutex_unlock(struct mutex*m){assert(m->held);m->held=false;assert(!pthread_mutex_unlock(&m->lock));}
static void*memchr_inv(const void*p,int c,size_t n){const u8*b=p;for(size_t i=0;i<n;i++)if(b[i]!=(u8)c)return(void*)(b+i);return NULL;}
#define IS_ERR_OR_NULL(p) (!(p)||(uintptr_t)(p)>=(uintptr_t)-4095)
#define ERR_PTR(e) ((void*)(intptr_t)(e))
#define I2C_M_RD 1
#define I2C_CLIENT_TEN 0x10
struct i2c_adapter{int tag;};struct i2c_client{struct i2c_adapter*adapter;unsigned short addr,flags;};struct i2c_msg{unsigned short addr,flags,len;u8*buf;};int i2c_transfer(struct i2c_adapter*,struct i2c_msg*,int);
#endif
'''
FIXTURE=r'''
#include "p105-bank0.c"
static struct p105_bank0*active;static struct i2c_adapter adapter={123};static struct i2c_client client={&adapter,0x3c,0};
static u8 banks[3][256];static unsigned int bank,calls,selectors,fail_at,short_at,epoch_at,holds,valids,valid_invalidate_at;static bool side_effect_on_error;
static u64 expected_owner_generation;static int valid_error,hold_error;static u8 prefixes[8];
struct wire{bool write;u8 reg,bytes[9];unsigned int n;};static struct wire records[8];
static pthread_mutex_t pause_lock=PTHREAD_MUTEX_INITIALIZER;static pthread_cond_t pause_cond=PTHREAD_COND_INITIALIZER;static bool paused_bus,entered,resume;
int i2c_transfer(struct i2c_adapter*a,struct i2c_msg*m,int n){assert(a==&adapter&&active->io_lock.held);calls++;unsigned int ordinal=calls;assert(n==1||n==2);assert(m[0].addr==0x3c&&!m[0].flags);prefixes[ordinal-1]=m[0].buf[0];
 if(paused_bus&&ordinal==1){pthread_mutex_lock(&pause_lock);entered=true;pthread_cond_broadcast(&pause_cond);while(!resume)pthread_cond_wait(&pause_cond,&pause_lock);pthread_mutex_unlock(&pause_lock);}
 if(ordinal==epoch_at)p105_bank0_invalidate(active);
 int ret=ordinal==fail_at?-EIO:ordinal==short_at?0:n;
 if(n==1){assert(m[0].len>=2&&m[0].len<=10);if(m[0].buf[0]==0x1e){assert(m[0].len==2&&m[0].buf[1]==0);selectors++;if(ret==n||side_effect_on_error)bank=0;}else if(ret==n||side_effect_on_error){unsigned int reg=m[0].buf[0];memcpy(banks[bank]+reg,m[0].buf+1,m[0].len-1);}}
 else{assert(m[0].len==1&&m[1].addr==0x3c&&m[1].flags==I2C_M_RD&&m[1].len>=1&&m[1].len<=9);memcpy(m[1].buf,banks[bank]+m[0].buf[0],m[1].len);}
 records[ordinal-1].write=n==1;records[ordinal-1].reg=m[0].buf[0];records[ordinal-1].n=n==1?m[0].len-1:m[1].len;memcpy(records[ordinal-1].bytes,n==1?m[0].buf+1:m[1].buf,records[ordinal-1].n);
 return ret;
}
static int valid(void*c,const struct p105_bank0_lease*l){(void)c;assert(l->owner_generation==expected_owner_generation);if(++valids==valid_invalidate_at)p105_bank0_invalidate(active);return valid_error;}
static int hold(void*c,const struct p105_bank0_lease*l,int e){(void)c;assert(l->owner_generation==expected_owner_generation&&e);holds++;return hold_error;}
static const struct p105_bank0_ops ops={valid,hold};static const struct p105_bank0_lease good={123,true,true};
static void setup(struct p105_bank0*b){memset(b,0,sizeof(*b));assert(!p105_bank0_init(b));active=b;b->client=&client;b->ops=&ops;client.addr=0x3c;client.flags=0;bank=2;calls=selectors=fail_at=short_at=epoch_at=holds=valids=valid_invalidate_at=0;valid_error=hold_error=0;expected_owner_generation=123;side_effect_on_error=paused_bus=entered=resume=false;memset(prefixes,0,sizeof(prefixes));for(unsigned int p=0;p<3;p++)for(unsigned int r=0;r<256;r++)banks[p][r]=(p*64+r)&255;}
static int read_sample(struct p105_bank0*b,const struct p105_bank0_lease*l,unsigned int reg,unsigned int n,u8*out){memset(out,0xa5,n);int e=p105_bank0_transfer(b,l,false,reg,out,n);if(e&&reg<=255&&n<=9)for(unsigned int i=0;i<n;i++)assert(!out[i]);return e;}
static void defaults(void){struct p105_bank0 b;u8 out[9];setup(&b);assert(read_sample(&b,&good,0x3d,1,out)==-EACCES);assert(!calls);b.enabled=true;struct p105_bank0_lease bad=good;bad.exclusive_all_pmu=false;assert(read_sample(&b,&bad,0x3d,1,out)==-EOPNOTSUPP);assert(!calls);b.client=ERR_PTR(-ENODEV);assert(read_sample(&b,&good,0x3d,1,out)==-EOPNOTSUPP);b.client=&client;b.ops=ERR_PTR(-ENODEV);assert(read_sample(&b,&good,0x3d,1,out)==-EOPNOTSUPP);}
static void packets(void){struct p105_bank0 b;u8 out[9];setup(&b);b.enabled=true;assert(!read_sample(&b,&good,0x0a,6,out));assert(bank==0&&calls==2&&selectors==1&&prefixes[0]==0x1e&&prefixes[1]==0x0a);for(unsigned int i=0;i<6;i++)assert(out[i]==0x0a+i);assert(atomic_read(&b.page0_known));assert(!read_sample(&b,&good,0x44,2,out));assert(calls==3&&selectors==1&&out[0]==0x44&&out[1]==0x45);u8 value=0x40;assert(!p105_bank0_transfer(&b,&good,true,0x3f,&value,1));assert(banks[0][0x3f]==0x40&&calls==4&&selectors==1);}
static void faults(void){struct p105_bank0 b;u8 out[9];for(unsigned int e=1;e<=2;e++)for(unsigned int shortread=0;shortread<2;shortread++){setup(&b);b.enabled=true;if(shortread)short_at=e;else fail_at=e;side_effect_on_error=true;assert(read_sample(&b,&good,0x3d,1,out)==-EIO);assert(atomic_read(&b.fault)&&!atomic_read(&b.page0_known)&&holds==1&&b.retained_lease_valid&&b.retained_lease.owner_generation==123);unsigned int n=calls;assert(read_sample(&b,&good,0x3d,1,out)==-EACCES);assert(calls==n);}
 setup(&b);b.enabled=true;fail_at=2;hold_error=-EBUSY;assert(read_sample(&b,&good,0x3d,1,out)==-EIO);assert(b.hold_error==-EBUSY);
 setup(&b);b.enabled=true;valid_error=-ESTALE;assert(read_sample(&b,&good,0x3d,1,out)==-ESTALE);assert(!calls&&!holds);
}
static void invalidation(void){struct p105_bank0 b;u8 out[9];for(unsigned int n=1;n<=2;n++){setup(&b);b.enabled=true;epoch_at=n;assert(read_sample(&b,&good,0x3d,1,out)==-ESTALE);assert(atomic_read(&b.fault)&&holds==1&&!atomic_read(&b.page0_known));}
 setup(&b);b.enabled=true;assert(!read_sample(&b,&good,0x3d,1,out));p105_bank0_invalidate(&b);assert(!atomic_read(&b.page0_known));assert(!read_sample(&b,&good,0x3d,1,out));assert(selectors==2);
 setup(&b);b.enabled=true;atomic64_set(&b.generation,UINT64_MAX);p105_bank0_invalidate(&b);assert(atomic_read(&b.fault));assert(read_sample(&b,&good,0x3d,1,out)==-EACCES);assert(!calls);
}
static void bounds(void){struct p105_bank0 b;u8 out[16]={0},v=1;setup(&b);b.enabled=true;struct p105_bank0_lease ordinary=good;ordinary.event_owner=false;
 assert(p105_bank0_transfer(&b,&ordinary,false,1,out,9)==-EINVAL);assert(p105_bank0_transfer(&b,&ordinary,true,0x12,&v,1)==-EINVAL);assert(!calls);
 for(unsigned int r=0;r<256;r++)if(r==0||r==0x1e||r==0x40||r==0x41||r==0x46||r==0x4c||r>=0x54)assert(p105_bank0_transfer(&b,&good,false,r,out,1)==-EINVAL);
 assert(p105_bank0_transfer(&b,&good,false,0x13d,out,1)==-EINVAL);assert(p105_bank0_transfer(&b,&good,false,0x0f,out,2)==-EINVAL);assert(p105_bank0_transfer(&b,&good,false,0x0a,out,0)==-EINVAL);assert(p105_bank0_transfer(&b,&good,false,0x12,out,10)==-EINVAL);assert(!calls);
 assert(!read_sample(&b,&good,1,9,out));assert(calls==2);assert(!p105_bank0_transfer(&b,&good,true,0x12,&v,1));assert(calls==3);
}
static void init_retains(void){struct p105_bank0 b;u8 out[9];setup(&b);b.enabled=true;fail_at=1;assert(read_sample(&b,&good,0x3d,1,out)==-EIO);struct p105_bank0 before=b;assert(p105_bank0_init(&b)==-EALREADY);assert(!memcmp(&b,&before,sizeof(b)));memset(&b,0,sizeof(b));b.ctx=(void*)1;before=b;assert(p105_bank0_init(&b)==-EINVAL);assert(!memcmp(&b,&before,sizeof(b)));}
struct task{struct p105_bank0*b;int error;u8 value;};static void*thread(void*ctx){struct task*t=ctx;u8 out[1];t->error=read_sample(t->b,&good,0x3d,1,out);t->value=out[0];return NULL;}
static void shared(bool invalidate){struct p105_bank0 b;setup(&b);b.enabled=true;paused_bus=true;struct task a={&b,99,0},c={&b,99,0};pthread_t ta,tc;assert(!pthread_create(&ta,NULL,thread,&a));pthread_mutex_lock(&pause_lock);while(!entered)pthread_cond_wait(&pause_cond,&pause_lock);pthread_mutex_unlock(&pause_lock);assert(!pthread_create(&tc,NULL,thread,&c));usleep(10000);assert(calls==1);if(invalidate)p105_bank0_invalidate(&b);pthread_mutex_lock(&pause_lock);resume=true;pthread_cond_broadcast(&pause_cond);pthread_mutex_unlock(&pause_lock);pthread_join(ta,NULL);pthread_join(tc,NULL);
 if(invalidate){assert(a.error==-ESTALE&&c.error==-EACCES&&calls==1);}else{assert(a.error==0&&c.error==0&&a.value==0x3d&&c.value==0x3d&&calls==3&&selectors==1);}
}
static void held_validator_and_invalid_output(void){struct p105_bank0 b;u8 out[9];setup(&b);b.enabled=true;assert(!read_sample(&b,&good,0x3d,1,out));valid_error=-ESTALE;assert(read_sample(&b,&good,0x3d,1,out)==-ESTALE);assert(atomic_read(&b.fault)&&!atomic_read(&b.page0_known)&&holds==1&&b.retained_lease_valid);unsigned int n=calls;valid_error=0;assert(read_sample(&b,&good,0x3d,1,out)==-EACCES);assert(calls==n&&holds==1);setup(&b);b.enabled=true;valid_error=1;assert(read_sample(&b,&good,0x3d,1,out)==-EPROTO);assert(!calls);memset(out,0xa5,9);assert(p105_bank0_transfer(NULL,&good,false,0x3d,out,1)==-EINVAL);assert(out[0]==0);memset(out,0xa5,9);assert(p105_bank0_transfer(&b,&good,false,0x1e,out,1)==-EINVAL);assert(out[0]==0);}
static void owner_change(void){struct p105_bank0 b;u8 out[9];struct p105_bank0_lease next=good;next.owner_generation=124;setup(&b);b.enabled=true;assert(!read_sample(&b,&good,0x3d,1,out));assert(b.selected_owner_generation==123);bank=2;expected_owner_generation=124;assert(!read_sample(&b,&next,0x3d,1,out));assert(selectors==2&&calls==4&&bank==0&&out[0]==0x3d&&b.selected_owner_generation==124);setup(&b);b.enabled=true;assert(!read_sample(&b,&good,0x3d,1,out));bank=2;expected_owner_generation=124;fail_at=3;assert(read_sample(&b,&next,0x3d,1,out)==-EIO);assert(calls==3&&holds==1&&atomic_read(&b.fault)&&!atomic_read(&b.page0_known)&&b.retained_lease.owner_generation==124);fail_at=0;assert(read_sample(&b,&next,0x3d,1,out)==-EACCES);assert(calls==3);}
static void validator_epoch(void){struct p105_bank0 b;u8 o[1];setup(&b);b.enabled=true;valid_invalidate_at=4;assert(read_sample(&b,&good,0x3d,1,o)==-ESTALE);assert(atomic_read(&b.fault)&&!atomic_read(&b.page0_known)&&holds==1);}
int main(int argc,char**argv){
 if(argc==5){struct p105_bank0 b;u8 bytes[9];setup(&b);b.enabled=true;unsigned int reg=strtoul(argv[2],NULL,0),n=strtoul(argv[3],NULL,0);bool write=atoi(argv[4]);assert(n>=1&&n<=9);memset(bytes,0xa5,n);int e=p105_bank0_transfer(&b,&good,write,reg,bytes,n);printf("{\"return_code\":%d,\"output_bytes\":[",e);for(unsigned int i=0;i<n;i++)printf("%s%u",i?",":"",bytes[i]);printf("],\"wire_calls\":[");for(unsigned int i=0;i<calls;i++){struct wire*r=&records[i];printf("%s{\"op\":\"%s\",\"prefix\":[%u],\"data\":[",i?",":"",r->write?"write":"read",r->reg);for(unsigned int j=0;j<r->n;j++)printf("%s%u",j?",":"",r->bytes[j]);printf("]}");}printf("]}\n");return 0;}
 assert(argc==2);switch(atoi(argv[1])){case 0:defaults();break;case 1:packets();break;case 2:faults();break;case 3:invalidation();break;case 4:bounds();break;case 5:init_retains();break;case 6:shared(false);shared(true);break;case 7:held_validator_and_invalid_output();break;case 8:owner_change();break;case 9:validator_epoch();break;default:abort();}return 0;}
'''
class Tests(unittest.TestCase):
 @classmethod
 def setUpClass(c):
  if not shutil.which('gcc'):raise unittest.SkipTest('GCC required')
  c.tmp=tempfile.TemporaryDirectory();p=Path(c.tmp.name);(p/'linux').mkdir();(p/'stub.h').write_text(STUB)
  for h in ('atomic.h','i2c.h','mutex.h','types.h','err.h','errno.h','string.h'):(p/'linux'/h).write_text('#include "stub.h"\n')
  (p/'fixture.c').write_text(FIXTURE);c.exe=p/'fixture';subprocess.run(['gcc','-std=gnu11','-pthread','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie','-I',str(p),'-I',str(H),str(p/'fixture.c'),'-o',str(c.exe)],check=True,text=True)
 @classmethod
 def tearDownClass(c):c.tmp.cleanup()
 def run_case(self,n):subprocess.run([str(self.exe),str(n)],check=True,text=True)
 def test_invalidation_during_final_validator(self):self.run_case(9)
 def test_default_denied_invalid_owner_resources(self):self.run_case(0)
 def test_page0_select_before_data_and_shared_cache(self):self.run_case(1)
 def test_every_transfer_fault_partial_effect_short_result(self):self.run_case(2)
 def test_epoch_generation_wrap_and_reselect(self):self.run_case(3)
 def test_known_page0_allowlist_and_event_owner(self):self.run_case(4)
 def test_reinit_retains_latch_failed_lease(self):self.run_case(5)
 def test_two_actual_callers_serialize_and_invalidate(self):self.run_case(6)
 def test_held_validator_loss_and_bounded_error_output(self):self.run_case(7)
 def test_changed_owner_reselects_and_failed_switch_holds(self):self.run_case(8)
 def test_pinned_original_native_wire_oracle(self):
  p=H/'native-page0-oracle.json';data=p.read_bytes()
  self.assertEqual(hashlib.sha256(data).hexdigest(),'04111b47cdc8fd60ec3014f6ed19d8c2bdc569ca641a234bcf15e1d13d7a138b')
  o=json.loads(data);self.assertEqual(o['case_count'],16)
  for row in o['cases']:
   r=subprocess.run([str(self.exe),'oracle',str(row['reg']),str(row['count']),str(int(row['write']))],check=True,capture_output=True,text=True)
   self.assertEqual(json.loads(r.stdout),{k:row[k]for k in ('return_code','output_bytes','wire_calls')})
if __name__=='__main__':unittest.main(verbosity=2)
