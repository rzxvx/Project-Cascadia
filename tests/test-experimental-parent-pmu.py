"""Actual parent transport + Linux regmap lock/RMW + unchanged child callbacks, fake I2C."""
import subprocess,tempfile,unittest,json,hashlib
from pathlib import Path
R=Path(__file__).resolve().parents[1];P=R/'experiments/p105-parent-pmu'
def function(data,name):
 import re
 m=re.search(r'(?:static\s+)?(?:int|void|bool)\s+'+name+r'\s*\([^;{]*\)\s*\{',data);assert m,name;start=m.start();a=m.end()-1;depth=1;i=a+1
 while depth:
  if data[i]=='{':depth+=1
  elif data[i]=='}':depth-=1
  i+=1
 return data[start:i]+'\n'
STUB=r'''
#ifndef STUB_H
#define STUB_H
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <asm-generic/errno.h>
typedef uint8_t u8;typedef uint16_t u16;typedef uint32_t u32;
typedef struct{int n;}atomic_t;static void atomic_set(atomic_t*a,int v){__atomic_store_n(&a->n,v,__ATOMIC_SEQ_CST);}static int atomic_read(const atomic_t*a){return __atomic_load_n(&a->n,__ATOMIC_SEQ_CST);}static int atomic_cmpxchg(atomic_t*a,int old,int v){__atomic_compare_exchange_n(&a->n,&old,v,false,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);return old;}
struct mutex{pthread_mutex_t p;};static void mutex_init(struct mutex*m){assert(!pthread_mutex_init(&m->p,NULL));}static void mutex_lock(struct mutex*m){assert(!pthread_mutex_lock(&m->p));}static void mutex_unlock(struct mutex*m){assert(!pthread_mutex_unlock(&m->p));}
static void*memchr_inv(const void*p,int c,size_t n){const u8*b=p;for(size_t i=0;i<n;i++)if(b[i]!=(u8)c)return(void*)(b+i);return NULL;}
#define IS_ERR_OR_NULL(p) (!(p)||(uintptr_t)(p)>=(uintptr_t)-4095)
#define ERR_PTR(e) ((void*)(intptr_t)(e))
#define lockdep_assert_held(p) ((void)(p))
#define IS_ALIGNED(x,a) (!((x)&((a)-1)))
#define I2C_LOCK_SEGMENT 1
#define I2C_M_RD 1
#define I2C_FUNC_I2C 1
#define REGCACHE_NONE 0
#define IS_ENABLED(x) (x)
#define CONFIG_APPLE_PMU_D1946_BANK0_EXPERIMENT 1
#define NOTIFY_DONE 0
#define pr_emerg(...) ((void)0)
#define SYSTEM_RUNNING 0
#define SYSTEM_POWER_OFF 1
static int system_state;static bool irq_off;
static bool irqs_disabled(void){return irq_off;}
struct device{int dummy;};struct sys_off_data{void*cb_data;};struct i2c_msg{unsigned short addr,flags,len;u8*buf;};struct i2c_adapter;
struct i2c_algorithm{int(*master_xfer_atomic)(struct i2c_adapter*,struct i2c_msg*,int);};
struct i2c_adapter{pthread_mutex_t p;const struct i2c_algorithm*algo;unsigned int retries;bool functional;};struct i2c_client{struct i2c_adapter*adapter;unsigned short addr,flags;};
static int i2c_check_functionality(struct i2c_adapter*a,unsigned int v){assert(v==I2C_FUNC_I2C);return a->functional;}
static void i2c_lock_bus(struct i2c_adapter*a,unsigned int flags){assert(flags==I2C_LOCK_SEGMENT);assert(!pthread_mutex_lock(&a->p));}
static void i2c_unlock_bus(struct i2c_adapter*a,unsigned int flags){assert(flags==I2C_LOCK_SEGMENT);assert(!pthread_mutex_unlock(&a->p));}
static int i2c_trylock_bus(struct i2c_adapter*a,unsigned int flags){assert(flags==I2C_LOCK_SEGMENT);return !pthread_mutex_trylock(&a->p);}
int __i2c_transfer(struct i2c_adapter*,struct i2c_msg*,int);
struct i2c_client*to_i2c_client(struct device*);int i2c_smbus_read_byte_data(struct i2c_client*,unsigned int);int i2c_smbus_write_byte_data(struct i2c_client*,unsigned int,unsigned int);
static unsigned int mdelay_calls;static void mdelay(unsigned int n){assert(n==1000);mdelay_calls++;}
struct regmap_bus{int(*write)(void*,const void*,size_t);int(*read)(void*,const void*,size_t,void*,size_t);size_t max_raw_read,max_raw_write;};
struct regmap_config{unsigned int reg_bits,val_bits,max_register,cache_type;void(*lock)(void*);void(*unlock)(void*);void*lock_arg;bool can_sleep;bool(*precious_reg)(struct device*,unsigned int);};
struct regmap{void(*lock)(void*);void(*unlock)(void*);void*lock_arg;void*bus_context;const struct regmap_bus*bus;unsigned int reg_stride;bool async,force_write_field;int(*reg_update_bits)(void*,unsigned int,unsigned int,unsigned int);};
int regmap_read(struct regmap*,unsigned int,unsigned int*);int regmap_write(struct regmap*,unsigned int,unsigned int);int regmap_update_bits_base(struct regmap*,unsigned int,unsigned int,unsigned int,bool*,bool,bool);
static int regmap_update_bits(struct regmap*m,unsigned int r,unsigned int mask,unsigned int v){return regmap_update_bits_base(m,r,mask,v,NULL,false,false);}
static bool regmap_volatile(struct regmap*m,unsigned int r){(void)m;(void)r;return false;}static unsigned int regmap_reg_addr(struct regmap*m,unsigned int r){(void)m;return r;}
struct apple_pmu_i2c{struct device*dev;struct regmap*regmap;struct regmap_config config;struct d1946_bank0_bus*bank0;};
struct gpio_chip{void*data;};static void*gpiochip_get_data(struct gpio_chip*g){return g->data;}
#define GPIO_LINE_DIRECTION_IN 1
#define GPIO_LINE_DIRECTION_OUT 0
struct apple_wled{struct regmap*map;u32 level,enable,enables;};struct backlight_device{void*data;int brightness;};static void*bl_get_data(struct backlight_device*b){return b->data;}static int backlight_get_brightness(struct backlight_device*b){return b->brightness;}
struct apple_pmu_i2c_pwrsw{struct device*dev;unsigned int base,mask,enable;};struct regulator_dev{void*reg_data;struct regmap*map;};static struct regmap*rdev_get_regmap(struct regulator_dev*r){return r->map;}
#define dev_info(...) ((void)0)
#define dev_err(...) ((void)0)
static void msleep(unsigned int n){(void)n;}
#endif
'''
PRE=r'''
#include "apple-pmu-d1946-bank0.c"
static struct d1946_bank0_bus active;static struct i2c_adapter adapter;static struct i2c_client client;static struct regmap map;static u8 banks[3][256];static unsigned int bank,calls,fail_at,short_at,atomic_calls,stop_at;static bool partial_error;static unsigned int wire_reg[1024],wire_value[1024],wire_n[1024];static unsigned int legacy_calls;static u8 wire_bytes[1024][256];static unsigned int wire_len[1024];
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;static pthread_cond_t cond=PTHREAD_COND_INITIALIZER;static bool pause_bus,entered,resume,raw_started,raw_done;
static int atomic_marker(struct i2c_adapter*a,struct i2c_msg*m,int n){(void)a;(void)m;(void)n;return 0;}
static const struct i2c_algorithm algorithm={atomic_marker};
struct i2c_client*to_i2c_client(struct device*d){(void)d;return &client;}
int i2c_smbus_read_byte_data(struct i2c_client*c,unsigned int r){assert(c==&client&&r==0x1b);legacy_calls++;return banks[bank][r];}
int i2c_smbus_write_byte_data(struct i2c_client*c,unsigned int r,unsigned int v){assert(c==&client&&r==0x1b);legacy_calls++;banks[bank][r]=v;return 0;}
int __i2c_transfer(struct i2c_adapter*a,struct i2c_msg*m,int n){assert(a==&adapter);assert(pthread_mutex_trylock(&a->p)!=0);assert(n==1||n==2);assert(m[0].addr==0x3c&&!m[0].flags);assert(calls<1024);unsigned int i=calls++;wire_reg[i]=m[0].buf[0];wire_n[i]=n;wire_value[i]=n==1&&m[0].len>1?m[0].buf[1]:0;
 if(pause_bus&&calls==1){pthread_mutex_lock(&gate);entered=true;pthread_cond_broadcast(&cond);while(!resume)pthread_cond_wait(&cond,&gate);pthread_mutex_unlock(&gate);}
 if(irq_off){assert(system_state>SYSTEM_RUNNING&&a->algo->master_xfer_atomic);atomic_calls++;}
 int ret=calls==fail_at?-ENXIO:calls==short_at?0:n;
 if(n==1){assert(m[0].len>=2);if(ret==n||partial_error){if(m[0].buf[0]==0x1e){assert(m[0].len==2&&m[0].buf[1]==0);bank=0;}else memcpy(banks[bank]+m[0].buf[0],m[0].buf+1,m[0].len-1);}}
 else{assert(m[0].len==1&&m[1].flags==I2C_M_RD&&m[1].addr==0x3c);memcpy(m[1].buf,banks[bank]+m[0].buf[0],m[1].len);}
 if(calls==stop_at)atomic_set(&active.state,BANK0_STOPPED);
 wire_len[i]=n==1?m[0].len-1:m[1].len;memcpy(wire_bytes[i],n==1?m[0].buf+1:m[1].buf,wire_len[i]);
 return ret;
}
static int _regmap_read(struct regmap*m,unsigned int r,unsigned int*v){if(r>255)return-EINVAL;u8 reg=r,out=0;int e=m->bus->read(m->bus_context,&reg,1,&out,1);*v=out;return e;}
static int _regmap_write(struct regmap*m,unsigned int r,unsigned int v){if(r>255||v>255)return-EINVAL;u8 wire[]={r,v};return m->bus->write(m->bus_context,wire,2);}
#define D1946_GPIO_CFG 0x61
#define D1946_GPIO_IS_INPUT(v) ((v)>=0x60)
#define D1946_GPIO_LEVEL 2
#define D1946_STANDBY 0x1b
#define D1946_WLED_ENABLE 0xd2
#define D1946_EVENTS 1
#define D1946_EVENT_MASKS 0x12
#define D1946_NEVENTS 9
#define D1946_ALARM_CTRL 0x5b
#define D1946_REG_A1 0xa1
#define D1946_BOOT_FLAGS 0xaf
#define D1946_GPIO_WAKE 2
#define D1946_NGPIO 18
static const u8 d1946_off_masks[9]={0x88,0xff,0xff,0xff,0xff,0xff,0xae,0xff,0xff};
'''
POST=r'''
static void setup(void){memset(&active,0,sizeof(active));memset(&adapter,0,sizeof(adapter));assert(!pthread_mutex_init(&adapter.p,NULL));adapter.algo=&algorithm;adapter.functional=true;client=(struct i2c_client){&adapter,0x3c,0};assert(!d1946_bank0_init(&active,&client));struct regmap_config c={};d1946_bank0_config(&active,&c);assert(c.reg_bits==8&&c.val_bits==8&&c.max_register==255&&c.cache_type==REGCACHE_NONE&&c.can_sleep);assert(c.precious_reg(NULL,1)&&c.precious_reg(NULL,9)&&!c.precious_reg(NULL,10));map=(struct regmap){.lock=c.lock,.unlock=c.unlock,.lock_arg=c.lock_arg,.bus_context=&active,.bus=d1946_bank0_regmap_bus(),.reg_stride=1};bank=2;calls=fail_at=short_at=atomic_calls=legacy_calls=mdelay_calls=stop_at=0;partial_error=false;irq_off=false;system_state=SYSTEM_RUNNING;pause_bus=entered=resume=raw_started=raw_done=false;for(unsigned int b=0;b<3;b++)for(unsigned int r=0;r<256;r++)banks[b][r]=(b*64+r)&255;}
static void cleanup(void){assert(!pthread_mutex_destroy(&active.map_lock.p));assert(!pthread_mutex_destroy(&adapter.p));}
static void normal_children(void){setup();struct apple_pmu_i2c pmu={.regmap=&map,.bank0=&active};struct gpio_chip gc={&pmu};banks[0][0x63]=9;assert(!apple_pmu_gpio_direction_output(&gc,2,1));assert(banks[0][0x63]==11);assert(apple_pmu_gpio_get(&gc,2)==1);assert(apple_pmu_gpio_direction_input(&gc,2)==-EPERM);struct apple_pmu_i2c_pwrsw pw={NULL,0x22,6,6};struct regulator_dev rd={&pw,&map};assert(!apple_pmu_i2c_pwrsw_set(&rd,true));assert((banks[0][0x22]&6)==6);pw.base=0x23;pw.mask=pw.enable=0x40;assert(!apple_pmu_i2c_pwrsw_set(&rd,true));pw.base=0x64;pw.mask=pw.enable=2;assert(!apple_pmu_i2c_pwrsw_set(&rd,false));pw.base=0x6c;pw.mask=0xc0;pw.enable=0x40;assert(!apple_pmu_i2c_pwrsw_set(&rd,true));struct apple_wled w={&map,0xd0,0xd2,0x3f};struct backlight_device bl={&w,1234};assert(!apple_wled_update(&bl));assert(banks[0][0xd0]==154&&banks[0][0xd1]==2&&banks[0][0xd2]==0x3f);for(unsigned int i=0;i<calls;i+=2){assert(wire_reg[i]==0x1e&&wire_value[i]==0);assert(wire_reg[i+1]!=0x1e);}cleanup();}
static void errors(void){for(unsigned int f=1;f<=2;f++)for(unsigned int sh=0;sh<2;sh++){setup();if(sh)short_at=f;else fail_at=f;partial_error=true;unsigned int v=0xa5;assert(regmap_read(&map,0xd0,&v)==(sh?-EIO:-ENXIO));assert(!v&&atomic_read(&active.fault)&&active.first_error==(sh?-EIO:-ENXIO));unsigned int n=calls;assert(regmap_write(&map,0xd2,0)==-EIO&&calls==n);assert(d1946_bank0_seal(&active,0)==-EIO&&!atomic_read(&active.off_ready));cleanup();}}
static void bounds_and_retries(void){setup();unsigned int v;assert(regmap_read(&map,0x1e,&v)==-EINVAL&&!calls);assert(regmap_write(&map,0x1e,2)==-EINVAL&&!calls);u8 r=0xff,b[4];map.lock(map.lock_arg);assert(map.bus->read(&active,&r,1,b,2)==-EINVAL);r=0x1d;assert(map.bus->read(&active,&r,1,b,2)==-EINVAL);r=0x1b;assert(map.bus->read(&active,&r,1,b,4)==-EINVAL);u8 forbidden[]={0x1b,1,2,3,4};assert(map.bus->write(&active,forbidden,sizeof(forbidden))==-EINVAL);map.unlock(map.lock_arg);adapter.retries=1;assert(regmap_read(&map,0xd0,&v)==-EOPNOTSUPP&&!calls);cleanup();setup();struct d1946_bank0_bus fresh={};adapter.retries=1;assert(d1946_bank0_init(&fresh,&client)==-EINVAL);adapter.retries=0;assert(d1946_bank0_init(&active,&client)==-EALREADY);cleanup();}
static void poweroff_success(void){setup();struct apple_pmu_i2c pmu={.regmap=&map,.bank0=&active};for(unsigned int r=0x61;r<=0x72;r++)banks[0][r]=0x63;banks[0][0x1b]=0xa4;assert(!apple_pmu_d1946_prepare_checked(&pmu));assert(!d1946_bank0_seal(&active,0));unsigned int n=calls,v;assert(regmap_read(&map,0x1b,&v)==-ESHUTDOWN&&calls==n);irq_off=true;system_state=SYSTEM_POWER_OFF;assert(!d1946_bank0_power_off_atomic(&active));assert(atomic_calls==4&&calls==n+4&&banks[0][0x1b]==0xa5);assert(wire_reg[n]==0x1e&&wire_reg[n+1]==0x1b&&wire_reg[n+2]==0x1e&&wire_reg[n+3]==0x1b);assert(d1946_bank0_power_off_atomic(&active)==-ESHUTDOWN&&calls==n+4);cleanup();}
static void poweroff_errors(void){for(unsigned int f=1;f<=4;f++){setup();assert(!d1946_bank0_seal(&active,0));irq_off=true;system_state=SYSTEM_POWER_OFF;fail_at=f;assert(d1946_bank0_power_off_atomic(&active)==-ENXIO);assert(calls==f&&atomic_read(&active.fault)&&!atomic_read(&active.off_ready));unsigned int n=calls;assert(d1946_bank0_power_off_atomic(&active)==-ESHUTDOWN&&calls==n);cleanup();}for(unsigned int f=0;f<4;f++){setup();assert(!d1946_bank0_seal(&active,0));irq_off=true;system_state=SYSTEM_POWER_OFF;if(f==0)irq_off=false;if(f==1)system_state=SYSTEM_RUNNING;if(f==2){static const struct i2c_algorithm no_atomic={0};adapter.algo=&no_atomic;}if(f==3)adapter.retries=1;assert(d1946_bank0_power_off_atomic(&active)==-EOPNOTSUPP&&!calls&&!atomic_read(&active.off_ready));cleanup();}setup();assert(!d1946_bank0_seal(&active,0));irq_off=true;system_state=SYSTEM_POWER_OFF;i2c_lock_bus(&adapter,I2C_LOCK_SEGMENT);assert(d1946_bank0_power_off_atomic(&active)==-EBUSY&&!calls&&!atomic_read(&active.off_ready));i2c_unlock_bus(&adapter,I2C_LOCK_SEGMENT);assert(d1946_bank0_power_off_atomic(&active)==-ESHUTDOWN&&!calls);cleanup();}
static void atomic_stop_boundaries(void){for(unsigned int i=1;i<=4;i++){setup();assert(!d1946_bank0_seal(&active,0));irq_off=true;system_state=SYSTEM_POWER_OFF;stop_at=i;assert(d1946_bank0_power_off_atomic(&active)==-ESHUTDOWN);assert(calls==i&&!atomic_read(&active.off_ready));assert(d1946_bank0_power_off_atomic(&active)==-ESHUTDOWN&&calls==i);cleanup();}}
static void prepare_and_stop(void){setup();struct apple_pmu_i2c pmu={.regmap=&map,.bank0=&active};fail_at=3;int e=apple_pmu_d1946_prepare_checked(&pmu);assert(e==-ENXIO&&calls==3);assert(d1946_bank0_seal(&active,e)==e&&!atomic_read(&active.off_ready));cleanup();setup();d1946_bank0_stop(&active);unsigned int v;assert(regmap_read(&map,0x63,&v)==-ESHUTDOWN&&!calls);assert(d1946_bank0_seal(&active,0)==-EALREADY);cleanup();}
static void*worker(void*arg){(void)arg;assert(!regmap_update_bits(&map,0x63,2,2));return NULL;}
static void*raw_worker(void*arg){(void)arg;pthread_mutex_lock(&gate);raw_started=true;pthread_cond_broadcast(&cond);pthread_mutex_unlock(&gate);i2c_lock_bus(&adapter,I2C_LOCK_SEGMENT);assert(calls==4);bank=2;raw_done=true;i2c_unlock_bus(&adapter,I2C_LOCK_SEGMENT);return NULL;}
static void adapter_serialization(void){setup();banks[0][0x63]=9;pause_bus=true;pthread_t a,b;assert(!pthread_create(&a,NULL,worker,NULL));pthread_mutex_lock(&gate);while(!entered)pthread_cond_wait(&cond,&gate);pthread_mutex_unlock(&gate);assert(!pthread_create(&b,NULL,raw_worker,NULL));pthread_mutex_lock(&gate);while(!raw_started)pthread_cond_wait(&cond,&gate);assert(!raw_done);resume=true;pthread_cond_broadcast(&cond);pthread_mutex_unlock(&gate);pthread_join(a,NULL);pthread_join(b,NULL);assert(raw_done&&banks[0][0x63]==11&&bank==2);pause_bus=false;unsigned int v;assert(!regmap_read(&map,0x63,&v)&&v==11&&bank==0&&calls==6);cleanup();}
static void parent_poweroff_routes(void){setup();struct apple_pmu_i2c pmu={.regmap=&map,.bank0=&active};struct sys_off_data data={&pmu};assert(!d1946_bank0_seal(&active,0));irq_off=true;system_state=SYSTEM_POWER_OFF;assert(apple_pmu_d1946_power_off(&data)==NOTIFY_DONE);assert(calls==4&&!legacy_calls&&mdelay_calls==1);cleanup();setup();pmu.bank0=&active;data.cb_data=&pmu;assert(!d1946_bank0_seal(&active,0));irq_off=true;system_state=SYSTEM_POWER_OFF;fail_at=1;assert(apple_pmu_d1946_power_off(&data)==NOTIFY_DONE);assert(calls==1&&!legacy_calls&&!mdelay_calls);cleanup();setup();pmu.bank0=NULL;data.cb_data=&pmu;assert(apple_pmu_d1946_power_off(&data)==NOTIFY_DONE);assert(!calls&&legacy_calls==2&&mdelay_calls==1);cleanup();}
static atomic_t stop_done;static int stopped_result;static unsigned int stopped_value;
static void*stopped_reader(void*arg){(void)arg;stopped_value=0xa5;stopped_result=regmap_read(&map,0x63,&stopped_value);return NULL;}
static void*stop_worker(void*arg){(void)arg;d1946_bank0_stop(&active);atomic_set(&stop_done,1);return NULL;}
static void stop_drains_actual_callback(void){setup();pause_bus=true;atomic_set(&stop_done,0);pthread_t reader,stopper;assert(!pthread_create(&reader,NULL,stopped_reader,NULL));pthread_mutex_lock(&gate);while(!entered)pthread_cond_wait(&cond,&gate);pthread_mutex_unlock(&gate);assert(!pthread_create(&stopper,NULL,stop_worker,NULL));unsigned int i;for(i=0;i<100;i++){if(atomic_read(&active.state)==BANK0_STOPPED)break;usleep(1000);}assert(i<100&&!atomic_read(&stop_done)&&calls==1);pthread_mutex_lock(&gate);resume=true;pthread_cond_broadcast(&cond);pthread_mutex_unlock(&gate);pthread_join(reader,NULL);pthread_join(stopper,NULL);assert(stopped_result==-ESHUTDOWN&&!stopped_value&&calls==1&&atomic_read(&stop_done));cleanup();}
int main(int argc,char**argv){if(argc==5){setup();unsigned int r=strtoul(argv[2],NULL,0),n=strtoul(argv[3],NULL,0);bool write=atoi(argv[4]);assert(r<=255&&n>=1&&n<=18);u8 bytes[18],wire[19],reg=r;memset(bytes,0xa5,n);wire[0]=r;memset(wire+1,0xa5,n);map.lock(map.lock_arg);int e=write?map.bus->write(&active,wire,n+1):map.bus->read(&active,&reg,1,bytes,n);map.unlock(map.lock_arg);assert(!e);printf("{\"logical_register\":%u,\"count\":%u,\"write\":%s,\"calls\":[",r,n,write?"true":"false");for(unsigned int i=0;i<calls;i++){printf("%s{\"op\":\"%s\",\"prefix\":[%u],\"data\":[",i?",":"",wire_n[i]==1?"write":"read",wire_reg[i]);for(unsigned int j=0;j<wire_len[i];j++)printf("%s%u",j?",":"",wire_bytes[i][j]);printf("],\"bank\":0}");}printf("],\"output\":[");for(unsigned int j=0;j<n;j++)printf("%s%u",j?",":"",bytes[j]);printf("]}\n");cleanup();return 0;}assert(argc==2);switch(atoi(argv[1])){case 0:normal_children();break;case 1:errors();break;case 2:bounds_and_retries();break;case 3:poweroff_success();break;case 4:poweroff_errors();break;case 5:prepare_and_stop();break;case 6:adapter_serialization();break;case 7:parent_poweroff_routes();break;case 8:stop_drains_actual_callback();break;case 9:atomic_stop_boundaries();break;default:abort();}return 0;}
'''
class C(unittest.TestCase):
 @classmethod
 def setUpClass(c):
  fixture=(P/'regmap-entrypoints-fixture.h').read_bytes()
  if hashlib.sha256(fixture).hexdigest()!='6480dab7d41910c16898d324b9c28689f958d81ddcff89d4b935ca3f53b961e0':raise AssertionError('pinned Linux regmap fixture changed')
  provenance=json.loads((P/'source-provenance.json').read_text())
  if provenance['regmap_fixture_sha256']!=hashlib.sha256(fixture).hexdigest():raise AssertionError('fixture provenance hash mismatch')
  for name,sha in provenance['normal_child_sources'].items():
   if hashlib.sha256((R/name).read_bytes()).hexdigest()!=sha:raise AssertionError('normal child source changed: '+name)
  current=R/'build/linux/drivers/base/regmap/regmap.c'
  if current.exists():
   names=('regmap_read','regmap_write','_regmap_update_bits','regmap_update_bits_base')
   expected=''.join(function(fixture.decode(),n)for n in names);actual=''.join(function(current.read_text(),n)for n in names)
   if actual!=expected:raise AssertionError('current Linux regmap bodies differ from pinned fixture')
  c.tmp=tempfile.TemporaryDirectory();p=Path(c.tmp.name);(p/'linux').mkdir();(p/'stub.h').write_text(STUB)
  for h in ('atomic.h','i2c.h','mutex.h','regmap.h','err.h','errno.h','interrupt.h','kernel.h','lockdep.h','string.h'):(p/'linux'/h).write_text('#include "stub.h"\n')
  reg=(P/'regmap-entrypoints-fixture.h').read_text();mfd=(P/'apple-pmu-i2c.c').read_text();wled=(R/'patches/files/drivers/video/backlight/apple-pmu-wled.c').read_text();pw=(R/'patches/files-sandcastle-port/drivers/regulator/apple-pmu-pwrsw.c').read_text();body=PRE+''.join(function(reg,n)for n in ('regmap_read','regmap_write','_regmap_update_bits','regmap_update_bits_base'))+''.join(function(mfd,n)for n in ('apple_pmu_gpio_cfg','apple_pmu_gpio_get_direction','apple_pmu_gpio_get','apple_pmu_gpio_set','apple_pmu_gpio_direction_output','apple_pmu_gpio_direction_input','apple_pmu_d1946_prepare_checked','apple_pmu_d1946_power_off'))+function(wled,'apple_wled_update')+function(pw,'apple_pmu_i2c_pwrsw_set')+POST
  (p/'fixture.c').write_text(body);c.exe=p/'fixture';subprocess.run(['gcc','-std=gnu11','-pthread','-Wall','-Wextra','-Werror','-Wno-unused-function','-fsanitize=address,undefined','-fno-pie','-no-pie','-I',str(p),'-I',str(P),str(p/'fixture.c'),'-o',str(c.exe)],check=True)
 @classmethod
 def tearDownClass(c):c.tmp.cleanup()
 def run_case(self,n):subprocess.run([str(self.exe),str(n)],check=True)
 def test_stop_at_every_atomic_transfer_blocks_following_io(self):self.run_case(9)
 def test_actual_parent_poweroff_never_falls_back_in_optin(self):self.run_case(7)
 def test_actual_GPIO_regulator_WLED_shared_parent(self):self.run_case(0)
 def test_selector_data_short_failure_latches(self):self.run_case(1)
 def test_selector_bounds_and_no_core_retries(self):self.run_case(2)
 def test_actual_checked_prepare_atomic_final_sequence(self):self.run_case(3)
 def test_each_atomic_error_missing_hook_trylock_and_no_retry(self):self.run_case(4)
 def test_prepare_failure_and_stop_no_normal_io(self):self.run_case(5);self.run_case(8)
 def test_actual_regmap_RMW_excludes_raw_adapter_interleave(self):self.run_case(6)
 def test_normal_logicals_against_full_original12_wrappers(self):
  data=(P/'native-parent-oracle.json').read_bytes();self.assertEqual(hashlib.sha256(data).hexdigest(),'f87764d51e58c404de87d30185a4835945a2caa3591bac3b146ad881748ffd2c');o=json.loads(data);self.assertEqual(o['case_count'],84)
  for row in o['cases']:
   r=subprocess.run([str(self.exe),'oracle',str(row['logical_register']),str(row['count']),str(int(row['write']))],capture_output=True,text=True,check=True);self.assertEqual(json.loads(r.stdout),row)
if __name__=='__main__':unittest.main(verbosity=2)
