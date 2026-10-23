// SPDX-License-Identifier: GPL-2.0-only
/* Explicit pthread/RAM/devres/time stand-ins, no device operations. */

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
typedef uint8_t u8;typedef uint16_t u16;typedef uint32_t u32;typedef uint64_t u64;
typedef struct{int n;}atomic_t;static void atomic_set(atomic_t*a,int v){__atomic_store_n(&a->n,v,__ATOMIC_SEQ_CST);}static int atomic_read(const atomic_t*a){return __atomic_load_n(&a->n,__ATOMIC_SEQ_CST);}static int atomic_cmpxchg(atomic_t*a,int old,int v){__atomic_compare_exchange_n(&a->n,&old,v,false,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);return old;}
struct mutex{pthread_mutex_t p;};static void mutex_init(struct mutex*m){assert(!pthread_mutex_init(&m->p,NULL));}static void mutex_lock(struct mutex*m){assert(!pthread_mutex_lock(&m->p));}static void mutex_unlock(struct mutex*m){assert(!pthread_mutex_unlock(&m->p));}
static void*memchr_inv(const void*p,int c,size_t n){const u8*b=p;for(size_t i=0;i<n;i++)if(b[i]!=(u8)c)return(void*)(b+i);return NULL;}
#define IS_ERR_OR_NULL(p) (!(p)||(uintptr_t)(p)>=(uintptr_t)-4095)
#define ERR_PTR(e) ((void*)(intptr_t)(e))
#define lockdep_assert_held(p) ((void)(p))
#define IS_ALIGNED(x,a) (!((x)&((a)-1)))
#define I2C_LOCK_SEGMENT 1
#define I2C_M_RD 1
#define I2C_M_TEN 0x10
#define I2C_M_NOSTART 0x4000
#define I2C_FUNC_I2C 1
#define REGCACHE_NONE 0
#define IS_ENABLED(x) (x)
#define CONFIG_APPLE_PMU_D1946_BANK0_EXPERIMENT 1
#define CONFIG_APPLE_PMU_D1946_ADC6_OWNER_EXPERIMENT 1
#define NOTIFY_DONE 0
#define pr_emerg(...) ((void)0)
#define SYSTEM_RUNNING 0
#define SYSTEM_POWER_OFF 1
static int system_state;static bool irq_off;
static bool irqs_disabled(void){return irq_off;}
struct device{int dummy;void*drvdata;};
static void*dev_get_drvdata(struct device*d){return d->drvdata;}
#define __maybe_unused
struct sys_off_data{void*cb_data;};struct i2c_msg{unsigned short addr,flags,len;u8*buf;};struct i2c_adapter;
struct i2c_algorithm{int(*master_xfer_atomic)(struct i2c_adapter*,struct i2c_msg*,int);int(*master_xfer)(struct i2c_adapter*,struct i2c_msg*,int);};
struct i2c_adapter{pthread_mutex_t p;const struct i2c_algorithm*algo;int retries;int bus_lock;bool functional;void*algo_data;void*quirks;unsigned long locked_flags;unsigned long timeout;struct device dev;};struct i2c_client{struct i2c_adapter*adapter;unsigned short addr,flags;struct device dev;};
static int i2c_check_functionality(struct i2c_adapter*a,unsigned int v){assert(v==I2C_FUNC_I2C);return a->functional;}
static void i2c_lock_bus(struct i2c_adapter*a,unsigned int flags){assert(flags==I2C_LOCK_SEGMENT);assert(!pthread_mutex_lock(&a->p));}
static void i2c_unlock_bus(struct i2c_adapter*a,unsigned int flags){assert(flags==I2C_LOCK_SEGMENT);assert(!pthread_mutex_unlock(&a->p));}
static int i2c_trylock_bus(struct i2c_adapter*a,unsigned int flags){assert(flags==I2C_LOCK_SEGMENT);return !pthread_mutex_trylock(&a->p);}
int __i2c_transfer(struct i2c_adapter*,struct i2c_msg*,int);
struct i2c_client*to_i2c_client(struct device*);int i2c_smbus_read_byte_data(struct i2c_client*,unsigned int);int i2c_smbus_write_byte_data(struct i2c_client*,unsigned int,unsigned int);
static unsigned int mdelay_calls;static void mdelay(unsigned int n){assert(n==1000);mdelay_calls++;}
struct regmap_bus{int(*write)(void*,const void*,size_t);int(*read)(void*,const void*,size_t,void*,size_t);size_t max_raw_read,max_raw_write;};
struct regmap_config{unsigned int reg_bits,val_bits,max_register,cache_type;void(*lock)(void*);void(*unlock)(void*);void*lock_arg;bool can_sleep,disable_locking;bool(*precious_reg)(struct device*,unsigned int);};
struct regmap{void(*lock)(void*);void(*unlock)(void*);void*lock_arg;void*bus_context;const struct regmap_bus*bus;unsigned int reg_stride;bool async,force_write_field;int(*reg_update_bits)(void*,unsigned int,unsigned int,unsigned int);};
int regmap_read(struct regmap*,unsigned int,unsigned int*);int regmap_write(struct regmap*,unsigned int,unsigned int);int regmap_update_bits_base(struct regmap*,unsigned int,unsigned int,unsigned int,bool*,bool,bool);
static int regmap_update_bits(struct regmap*m,unsigned int r,unsigned int mask,unsigned int v){return regmap_update_bits_base(m,r,mask,v,NULL,false,false);}
static bool regmap_volatile(struct regmap*m,unsigned int r){(void)m;(void)r;return false;}static unsigned int regmap_reg_addr(struct regmap*m,unsigned int r){(void)m;return r;}
struct apple_pmu_i2c{struct device*dev;struct regmap*regmap;struct regmap_config config;struct d1946_bank0_bus*bank0;struct p105_owned_adc6*adc6;};
struct gpio_chip{void*data;};static void*gpiochip_get_data(struct gpio_chip*g){return g->data;}
#define GPIO_LINE_DIRECTION_IN 1
#define GPIO_LINE_DIRECTION_OUT 0
struct apple_wled{struct regmap*map;u32 level,enable,enables;};struct backlight_device{void*data;int brightness;};static void*bl_get_data(struct backlight_device*b){return b->data;}static int backlight_get_brightness(struct backlight_device*b){return b->brightness;}
struct apple_pmu_i2c_pwrsw{struct device*dev;unsigned int base,mask,enable;};struct regulator_dev{void*reg_data;struct regmap*map;};static struct regmap*rdev_get_regmap(struct regulator_dev*r){return r->map;}
#define dev_info(...) ((void)0)
#define dev_err(...) ((void)0)
static void msleep(unsigned int n){(void)n;}
typedef struct{u64 n;}atomic64_t;
static void atomic64_set(atomic64_t*a,u64 n){__atomic_store_n(&a->n,n,__ATOMIC_SEQ_CST);}static u64 atomic64_read(const atomic64_t*a){return __atomic_load_n(&a->n,__ATOMIC_SEQ_CST);}static u64 atomic64_inc_return(atomic64_t*a){return __atomic_add_fetch(&a->n,1,__ATOMIC_SEQ_CST);}
struct task_struct{int dummy;};static _Thread_local struct task_struct task;
#define current (&task)
static bool fake_warn(bool x){return x;}
#define WARN_ON(x) fake_warn(!!(x))
#define IS_ERR(x) ((uintptr_t)(x)>=(uintptr_t)-4095)
#define PTR_ERR(x) ((int)(intptr_t)(x))
#define ERR_CAST(x) ((void*)(x))
#define GFP_KERNEL 0
#define EXPORT_SYMBOL_GPL(x)
#define I2C_ALF_IS_SUSPENDED 0
static bool test_bit(unsigned int bit,unsigned long*p){return !!(*p&(1UL<<bit));}
static bool in_atomic(void){return irq_off;}
u64 ktime_get_ns(void);void usleep_range(unsigned long,unsigned long);
int regmap_bulk_read(struct regmap*,unsigned int,void*,size_t);
void*devm_kzalloc(struct device*,size_t,int);
struct regmap*devm_regmap_init(struct device*,const struct regmap_bus*,void*,const struct regmap_config*);
struct regmap*regmap_init(struct device*,const struct regmap_bus*,void*,const struct regmap_config*);
void regmap_exit(struct regmap*);
int devm_add_action_or_reset(struct device*,void(*)(void*),void*);
#define static_branch_unlikely(x) false
#define WARN(x,...) ((void)0)
#define dev_dbg(...) ((void)0)
#define time_after(a,b) ((long)((b)-(a))<0)
static unsigned long jiffies;
static int __i2c_check_suspended(struct i2c_adapter*a){return test_bit(0,&a->locked_flags)?-ESHUTDOWN:0;}
static int i2c_check_for_quirks(struct i2c_adapter*a,struct i2c_msg*m,int n){(void)a;(void)m;(void)n;return 0;}
static bool i2c_in_atomic_xfer_mode(void){return irq_off&&system_state>SYSTEM_RUNNING;}
#define trace_i2c_read(...) ((void)0)
#define trace_i2c_write(...) ((void)0)
#define trace_i2c_reply(...) ((void)0)
#define trace_i2c_result(...) ((void)0)
#endif
