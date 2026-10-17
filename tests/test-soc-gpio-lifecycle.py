#!/usr/bin/env python3
"""Actual Linux GPIO request/configure/free versus real S5L callbacks, offline.

Compile current runtime source unchanged and a hash-pinned historical control.
The MMIO, descriptor lookup, ownership locks and module references are simulated.
No hardware, source downloads or production driver writes occur.
Set CASCADIA_LINUX_SOURCE to a locally available pinned Linux 6.12 tree when
build/linux is absent. Historical S5L bytes come from a pinned upstream blob
available in the base tree even when its earlier commit is absent in a shallow
clone, plus a hash-pinned selector-mask-only fixture patch. No baseline
callback is reimplemented or derived by reversing the candidate input fix.
"""
from pathlib import Path
import hashlib,importlib.util,os,re,subprocess,tempfile,unittest

REPO=Path(__file__).resolve().parents[1]
SOURCE=REPO/'patches/files-sandcastle-port/drivers/pinctrl/pinctrl-s5l8940x-gpio.c'
spec=importlib.util.spec_from_file_location('baseline',REPO/'tests/test-usb-phy-core-lifecycle.py')
baseline=importlib.util.module_from_spec(spec);spec.loader.exec_module(baseline)
HASHES={
 'drivers/gpio/gpiolib.c':'5d102e34d0c4817cf047f50a43cae4e6335c1383911558f613e2e353770837c1',
 'drivers/gpio/gpiolib.h':'08317191b2a29e9621a4581d114e1ed8a6a693142c270b365f12f2f481ef7b85',
 'drivers/pinctrl/pinmux.c':'a8a05945f4563566e91f2011cc85674a18cdc036d0b4e8d04efc79482e1711ff',
 'drivers/pinctrl/core.c':'dd75fbed0d2e5692bb5ac21426a5cfe0c8ac1b22e4558a3355037dad81b7e513',
 'include/linux/gpio/consumer.h':'d33fa1c69aa32bfadcd5c0412f3ad2f452176fdbb3f6565b036379bad685c426',
 'include/linux/irq.h':'fdc47c7ebc4ba2e561fef2930140a44419f4a6c6b6568372ccb7cb30e41a08b2',
 'include/linux/gpio/machine.h':'7992f9786fb0359cb596e1440c5423580b62954e06833928aaa2f87248709ff0',
}
GPIO_BLOB='b38607939ad5cc668d0551772945b1000a189837'
GPIO_BASELINE_SHA='e71fc77818ad2a6358b55ee8233da79c30b2f88985decd9e34bcadb01456c53f'

def historical_gpio_blob(repo=REPO):
 # Full or shallow base-tree clones carry the same file object. An absent
 # object is an explicit source prerequisite. Request no lazy fetching and
 # independently forbid transport, including on older Git implementations.
 result=subprocess.run(['git','-c','protocol.allow=never','-C',str(repo),
                        'cat-file','blob',GPIO_BLOB],capture_output=True,
                       check=False,timeout=30,
                       env=dict(os.environ,GIT_NO_LAZY_FETCH='1'))
 if result.returncode or hashlib.sha256(result.stdout).hexdigest()!=GPIO_BASELINE_SHA:
  raise ValueError('requires local pinned upstream S5L blob '+GPIO_BLOB+'; network transport disabled')
 return result.stdout

def extract(text,name):return baseline.extract(text,name)
def enum(text,name):
 start=text.index('enum '+name+' {');end=text.index('};',start)+2
 return text[start:end]
def harness(legacy=False):
 sources={name:baseline.baseline_source(name,sha).decode() for name,sha in HASHES.items()}
 driver=SOURCE.read_text()
 if legacy:
  archived=historical_gpio_blob()
  fixture=REPO/'tests/fixtures/soc-gpio-mux-only.patch'
  if hashlib.sha256(fixture.read_bytes()).hexdigest()!='f8563b2727be5035f4a1995bd8663d205b164a793487f0e47dfd58b3ea06e78e':
   raise ValueError('historical selector-mask-only fixture SHA256 mismatch')
  with tempfile.TemporaryDirectory(prefix='gpio-historical-control-') as directory:
   output=Path(directory)/SOURCE.relative_to(REPO)
   output.parent.mkdir(parents=True);output.write_bytes(archived)
   subprocess.run(['git','-c','protocol.allow=never','apply','--whitespace=error',str(fixture)],
                  cwd=directory,capture_output=True,check=True,timeout=30)
   archived=output.read_bytes()
  if hashlib.sha256(archived).hexdigest()!='d0486b6e605be4770fcc6a6d24d48e147e7e1fa4409a9cdc2272bd00e453664d':
   raise ValueError('requires exact historical S5L input callback source')
  driver=archived.decode()
 gpio=sources['drivers/gpio/gpiolib.c'];pinmux=sources['drivers/pinctrl/pinmux.c'];pinctrl=sources['drivers/pinctrl/core.c']
 macros='\n'.join(line for line in driver.splitlines() if re.match(r'#define\s+(PINCFG_STAT_|REG_GPIO|REG_IRQ)',line))
 flags='\n'.join(line for line in sources['drivers/gpio/gpiolib.h'].splitlines() if line.startswith('#define FLAG_'))
 dflags='\n'.join(line for line in sources['include/linux/gpio/consumer.h'].splitlines() if line.startswith('#define GPIOD_FLAGS_BIT'))
 prefix=r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
typedef uint32_t u32;
#define __iomem
#define ENOTSUPP 524
#define CONFIG_PINCTRL 1
#define BIT(n) (1ul<<(n))
#define READ_ONCE(v) (v)
#define WRITE_ONCE(v,x) ((v)=(x))
#define IS_ERR(v) ((intptr_t)(v)<0)
#define IS_ERR_OR_NULL(v) (!(v)||IS_ERR(v))
#define PTR_ERR(v) ((intptr_t)(v))
#define GFP_KERNEL 0
#define dev_err(...) ((void)0)
#define dev_dbg(...) ((void)0)
#define dev_err_probe(...) ((void)0)
#define pr_warn(...) ((void)0)
#define WARN_ON(v) (v)
#define gpiod_warn(...) gpio_log(__VA_ARGS__)
#define gpiod_dbg(...) gpio_log(__VA_ARGS__)
#define gpiod_err(...) gpio_log(__VA_ARGS__)
#define trace_gpio_direction(...) ((void)0)
#define trace_gpio_value(...) ((void)0)
#define EXPORT_SYMBOL_GPL(...) /* empty */
static void might_sleep(void) {}
struct device { int unused; };
struct apple_s5l8940x_gpio_pincfg { uint8_t irqtype,stat; };
struct apple_s5l8940x_gpio_pinctrl { void *base; struct apple_s5l8940x_gpio_pincfg *pin_cfgs;
 unsigned npins; const char **pin_names; };
struct gpio_chip;struct pinctrl_dev;struct pinctrl_gpio_range;
struct pinmux_ops {
 int (*get_functions_count)(struct pinctrl_dev *);
 const char *(*get_function_name)(struct pinctrl_dev *,unsigned);
 int (*get_function_groups)(struct pinctrl_dev *,unsigned,const char *const **,unsigned *);
 int (*set_mux)(struct pinctrl_dev *,unsigned,unsigned);
 int (*gpio_request_enable)(struct pinctrl_dev *,struct pinctrl_gpio_range *,unsigned);
 void (*gpio_disable_free)(struct pinctrl_dev *,struct pinctrl_gpio_range *,unsigned);
 int (*request)(struct pinctrl_dev *,unsigned); void (*free)(struct pinctrl_dev *,unsigned);
 bool strict;
};
struct pinctrl_desc { const struct pinmux_ops *pmxops; };
struct pinctrl_dev { struct apple_s5l8940x_gpio_pinctrl *data; struct pinctrl_desc *desc;
 int mutex; void *owner; struct device *dev; };
struct pinctrl_gpio_range { const char *name; unsigned base,pin_base; const unsigned *pins; };
struct pin_desc { const char *name,*mux_owner,*gpio_owner; unsigned mux_usecount; void *mux_setting; };
struct fake_list { bool empty; };
struct gpio_device { struct fake_list pin_ranges; };
struct gpio_chip { struct apple_s5l8940x_gpio_pinctrl *data; int base;
 struct gpio_device *gpiodev;
 int (*request)(struct gpio_chip *,unsigned); void (*free)(struct gpio_chip *,unsigned);
 int (*get_direction)(struct gpio_chip *,unsigned); int (*get)(struct gpio_chip *,unsigned);
 void (*set)(struct gpio_chip *,unsigned,int);
 int (*direction_input)(struct gpio_chip *,unsigned);
 int (*direction_output)(struct gpio_chip *,unsigned,int);
};
struct gpio_desc { struct gpio_chip *gc; unsigned offset; unsigned long flags; const char *label; };
static void gpio_log(struct gpio_desc *d,const char *fmt,...) { (void)d;(void)fmt; }
struct gpio_chip_guard { struct gpio_chip *gc; };
static struct gpio_chip_guard chip_guard(struct gpio_desc *d) { return (struct gpio_chip_guard){d?d->gc:NULL}; }
#define CLASS(type,var) struct gpio_chip_guard var=chip_guard
static struct pin_desc pins[224];
static struct pinctrl_dev *requested_pinctrl;
static struct pinctrl_gpio_range range={.name="fake",.base=0,.pin_base=0};
static unsigned module_refs,count;static uint32_t registers[1024],writes[32],write_offsets[32];
struct irq_data { struct gpio_chip *gc; unsigned hwirq; };
static unsigned irq_handler_kind;
static struct gpio_chip *irq_data_get_irq_chip_data(struct irq_data *d) { return d->gc; }
static void handle_level_irq(void) {}
static void handle_edge_irq(void) {}
static void irq_set_handler_locked(struct irq_data *d,void (*handler)(void)) {
 (void)d;irq_handler_kind=handler==handle_level_irq?2:1;assert(handler==handle_level_irq||handler==handle_edge_irq); }
static uint32_t readl(void *p) { uintptr_t off=(unsigned char *)p-(unsigned char *)registers;
 assert(off<sizeof(registers)&&!(off%4));return registers[off/4]; }
static void writel(uint32_t v,void *p) { uintptr_t off=(unsigned char *)p-(unsigned char *)registers;
 assert(off<sizeof(registers)&&!(off%4)&&count<32);write_offsets[count]=off;writes[count++]=v;registers[off/4]=v; }
static struct apple_s5l8940x_gpio_pinctrl *gpiochip_get_data(struct gpio_chip *gc) { return gc->data; }
static struct apple_s5l8940x_gpio_pinctrl *pinctrl_dev_get_drvdata(struct pinctrl_dev *pc) { return pc->data; }
static struct pin_desc *pin_desc_get(struct pinctrl_dev *pc,unsigned pin) { (void)pc;return pin<224?&pins[pin]:NULL; }
static bool try_module_get(void *owner) { (void)owner;module_refs++;return true; }
static void module_put(void *owner) { (void)owner;assert(module_refs);module_refs--; }
static char *kasprintf(int flag,const char *fmt,...) { char b[64];va_list args;(void)flag;
 va_start(args,fmt);vsnprintf(b,sizeof(b),fmt,args);va_end(args);return strdup(b); }
static void kfree(const void *p) { free((void *)p); }
static void mutex_lock(int *m) { assert(!*m);*m=1; }
static void mutex_unlock(int *m) { assert(*m);*m=0; }
static bool list_empty(struct fake_list *l) { return l->empty; }
static int pinctrl_get_device_gpio_range(struct gpio_chip *gc,unsigned off,struct pinctrl_dev **pc,struct pinctrl_gpio_range **r) {
 (void)gc;if(off>=224)return -EINVAL;*pc=requested_pinctrl;*r=&range;return 0; }
static bool pinctrl_ready_for_gpio_range(struct gpio_chip *gc,unsigned off) { (void)gc;(void)off;return false; }
static unsigned gpio_chip_hwgpio_desc(struct gpio_desc *d) { return d->offset; }
#define gpio_chip_hwgpio(d) gpio_chip_hwgpio_desc(d)
static bool gpiochip_line_is_valid(struct gpio_chip *gc,unsigned off) { (void)gc;return off<224; }
static bool test_bit(unsigned n,const unsigned long *v) { return !!(*v&BIT(n)); }
static void set_bit(unsigned n,unsigned long *v) { *v|=BIT(n); }
static void clear_bit(unsigned n,unsigned long *v) { *v&=~BIT(n); }
static bool test_and_set_bit(unsigned n,unsigned long *v) { bool old=test_bit(n,v);set_bit(n,v);return old; }
static void assign_bit(unsigned n,unsigned long *v,bool value) { if(value)set_bit(n,v);else clear_bit(n,v); }
static int desc_set_label(struct gpio_desc *d,const char *label) { d->label=label;return 0; }
static int gpio_set_bias(struct gpio_desc *d) { (void)d;return 0; }
static int gpiod_set_transitory(struct gpio_desc *d,bool value) { (void)d;(void)value;return 0; }
static const char *function_name_or_default(const char *name) { return name?name:"?"; }
#define GPIOLINE_CHANGED_RELEASED 0
static void gpiod_line_state_notify(struct gpio_desc *d,int state) { (void)d;(void)state; }
'''
 callbacks=('apple_s5l8940x_gpio_set_reg','apple_s5l8940x_gpio_refresh_reg','apple_s5l8940x_gpio_get_reg',
 'apple_s5l8940x_gpio_init_reg','apple_s5l8940x_gpio_pinmux_get_functions_count',
 'apple_s5l8940x_gpio_pinmux_get_function_name','apple_s5l8940x_gpio_pinmux_get_function_groups',
 'apple_s5l8940x_gpio_pinmux_enable','apple_s5l8940x_gpio_gpio_get_direction','apple_s5l8940x_gpio_gpio_get',
 'apple_s5l8940x_gpio_gpio_set','apple_s5l8940x_gpio_gpio_direction_input','apple_s5l8940x_gpio_gpio_direction_output',
 'apple_s5l8940x_gpio_gpio_irq_unmask','apple_s5l8940x_gpio_gpio_irq_startup','apple_s5l8940x_gpio_gpio_irq_set_type')
 def driver_func(name):
  match=re.search(r'static (?:(?:void|int|uint32_t|unsigned int) |const char \*)'+re.escape(name)+r'\(',driver)
  if not match:raise ValueError(name)
  return extract(driver,driver[match.start():match.end()-1])
 body='\n'.join(driver_func(name) for name in callbacks)
 ops_start=driver.index('static const struct pinmux_ops apple_s5l8940x_gpio_pinmux_ops = {')
 ops_end=driver.index('};',ops_start)+2
 body+='\n'+driver[ops_start:ops_end]
 irq_types='enum {\n'+'\n'.join(line for line in sources['include/linux/irq.h'].splitlines() if re.match(r'\s+IRQ_TYPE_\w+\s*=',line))+'\n};'
 code=prefix+'\n'+irq_types+'\n'+macros+'\n'+flags+'\n'+dflags+'\n'+enum(sources['include/linux/gpio/consumer.h'],'gpiod_flags')+'\n'+enum(sources['include/linux/gpio/machine.h'],'gpio_lookup_flags')+'\n'+body
 for decl in ('static int pin_request','static const char *pin_free','int pinmux_request_gpio','void pinmux_free_gpio'):
  code+='\n'+extract(pinmux,decl)
 for decl in ('static inline int gpio_to_pin','int pinctrl_gpio_request','void pinctrl_gpio_free'):
  code+='\n'+extract(pinctrl,decl)
 for decl in ('int gpiochip_generic_request','void gpiochip_generic_free','int gpiod_get_direction','static int gpiod_request_commit','static int validate_desc'):
  code+='\n'+extract(gpio,decl)
 start=gpio.index('#define VALIDATE_DESC(desc)');end=gpio.index('\n\n',start)
 code+='\n'+gpio[start:end]
 for decl in ('int gpiod_direction_input','static int gpiod_direction_output_raw_commit'):
  code+='\n'+extract(gpio,decl)
 code+='\nstatic int gpiod_direction_output(struct gpio_desc *d,int value) { return gpiod_direction_output_raw_commit(d,value); }\n'
 code+='\n'+extract(gpio,'int gpiod_configure_flags')+'\n'+extract(gpio,'static void gpiod_free_commit')
 assigns=re.findall(r'pctl->gpio_chip\.(request|free|get_direction|direction_input|direction_output|get|set) = ([^;]+);',driver)
 assert len(assigns)==7
 code+='''
static void setup(unsigned pin,uint32_t initial,struct gpio_desc *d,struct gpio_chip *gc,
 struct pinctrl_dev *pc,struct apple_s5l8940x_gpio_pinctrl *control,
 struct apple_s5l8940x_gpio_pincfg *cfg,struct gpio_device *gdev,struct pinctrl_desc *pd) {
 memset(registers,0,sizeof(registers));memset(cfg,0,224*sizeof(*cfg));memset(pins,0,sizeof(pins));count=module_refs=irq_handler_kind=0;
 memset(gc,0,sizeof(*gc));memset(pc,0,sizeof(*pc));memset(d,0,sizeof(*d));
 registers[pin]=initial;control->base=registers;control->pin_cfgs=cfg;control->npins=224;
 pc->data=control;pd->pmxops=&apple_s5l8940x_gpio_pinmux_ops;pc->desc=pd;requested_pinctrl=pc;
 gdev->pin_ranges.empty=false;gc->data=control;gc->gpiodev=gdev;d->gc=gc;d->offset=pin;
 pins[pin].name="synthetic";
'''
 code+='\n'.join(' gc->'+name+'='+value+';' for name,value in assigns)
 code+='\n apple_s5l8940x_gpio_init_reg(control,pin);\n}\n'
 return code

class Lifecycle(unittest.TestCase):
 def run_c(self,body,legacy=False):
  prefix='''
 (void)apple_s5l8940x_gpio_gpio_irq_startup;
 (void)apple_s5l8940x_gpio_gpio_irq_set_type;
 struct apple_s5l8940x_gpio_pincfg cfg[224];struct apple_s5l8940x_gpio_pinctrl control={0};
 struct gpio_desc d;struct gpio_chip gc;struct pinctrl_dev pc;struct gpio_device gd;struct pinctrl_desc pd;
'''
  text=harness(legacy)+'\nint main(void) {\n'+prefix+body+'\nreturn 0;\n}\n'
  with tempfile.TemporaryDirectory(prefix='gpio-input-contract-') as td:
   c,binary=Path(td)/'gpio.c',Path(td)/'gpio';c.write_text(text)
   compile=subprocess.run(['cc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-fsanitize=undefined',str(c),'-o',str(binary)],text=True,capture_output=True,timeout=30)
   self.assertEqual(compile.returncode,0,compile.stderr)
   result=subprocess.run([str(binary)],text=True,capture_output=True,timeout=30)
   self.assertEqual(result.returncode,0,result.stderr)

 def test_actual_request_input_free_switches_inherited_peripheral_to_gpio(self):
  self.run_c('''
 setup(22,0x2b0,&d,&gc,&pc,&control,cfg,&gd,&pd);
 assert(!gpiod_request_commit(&d,"fake-input") && !count && module_refs==1);
 assert(!gpiod_configure_flags(&d,"fake-input",0,GPIOD_IN));
 assert(registers[22]==0x29e && !(cfg[22].stat&PINCFG_STAT_PERIPH));
 assert(!test_bit(FLAG_IS_OUT,&d.flags) && gpiod_get_direction(&d)==1);
 unsigned before=count;gpiod_free_commit(&d);assert(!module_refs && !pins[22].gpio_owner && count==before);
''')

 def test_exact_historical_input_claim_leaves_peripheral_mux_active(self):
  self.run_c('''
 setup(22,0x2b0,&d,&gc,&pc,&control,cfg,&gd,&pd);
 assert(!gpiod_request_commit(&d,"fake-input") && !count);
 assert(!gpiod_configure_flags(&d,"fake-input",0,GPIOD_IN));
 assert(registers[22]==0x2b0 && (cfg[22].stat&PINCFG_STAT_PERIPH));
 assert(!test_bit(FLAG_IS_OUT,&d.flags) && gpiod_get_direction(&d)==1);
 gpiod_free_commit(&d);assert(!module_refs);
''',legacy=True)

 def test_asis_request_and_free_leave_inherited_mux_and_unknown_bit40_untouched(self):
  for legacy in (False,True):
   self.run_c('''
 const uint32_t initial[]={0x2b0,0x2d2,0x2f0};
 for(unsigned i=0;i<3;i++) {
  setup(196,initial[i],&d,&gc,&pc,&control,cfg,&gd,&pd);
  unsigned stat=cfg[196].stat;
  assert(!gpiod_request_commit(&d,"fake-asis"));
  assert(!gpiod_configure_flags(&d,"fake-asis",0,GPIOD_ASIS));
  assert(!count && registers[196]==initial[i] && cfg[196].stat==stat);
  assert(!pd.pmxops->gpio_request_enable && !pd.pmxops->request);
  gpiod_free_commit(&d);
  assert(!count && !module_refs && !pins[196].gpio_owner && registers[196]==initial[i]);
 }
''',legacy=legacy)

 def test_hsic_shaped_asis_then_output_sequence_remains_unchanged(self):
  for legacy in (False,True):
   self.run_c('''
 setup(50,0x2f0,&d,&gc,&pc,&control,cfg,&gd,&pd);
 registers[196]=0xa50002d0;
 assert(!gpiod_request_commit(&d,"fake-host-ready"));
 assert(!gpiod_configure_flags(&d,"fake-host-ready",0,GPIOD_ASIS) && !count);
 assert(!gpiod_direction_output(&d,0) && registers[50]==0x292);
 assert(!gpiod_direction_output(&d,1) && registers[50]==0x293);
 assert(test_bit(FLAG_IS_OUT,&d.flags) && !(cfg[50].stat&PINCFG_STAT_PERIPH));
 assert(registers[196]==0xa50002d0);
 unsigned before=count;gpiod_free_commit(&d);
 assert(count==before && registers[50]==0x293 && !module_refs);
''',legacy=legacy)

 def test_input_after_explicit_peripheral_selection_clears_both_cached_flags(self):
  self.run_c('''
 setup(22,0x292,&d,&gc,&pc,&control,cfg,&gd,&pd);
 assert(!apple_s5l8940x_gpio_pinmux_enable(&pc,1,22));
 assert((cfg[22].stat&(PINCFG_STAT_OUTEN|PINCFG_STAT_PERIPH))==0x22);
 assert(!gpiod_request_commit(&d,"fake-input"));
 assert(!gpiod_configure_flags(&d,"fake-input",0,GPIOD_IN));
 assert(!(cfg[22].stat&(PINCFG_STAT_OUTEN|PINCFG_STAT_PERIPH)) && registers[22]==0x29e);
 assert(!test_bit(FLAG_IS_OUT,&d.flags));gpiod_free_commit(&d);
''')

 def test_input_retains_existing_irq_cached_policy_and_upper_register_fields(self):
  self.run_c('''
 setup(22,0x294,&d,&gc,&pc,&control,cfg,&gd,&pd);
 assert(cfg[22].irqtype==4 && cfg[22].stat==PINCFG_STAT_IRQEN);
 assert(!apple_s5l8940x_gpio_pinmux_enable(&pc,1,22));
 assert(!gpiod_request_commit(&d,"fake-irq-input"));
 assert(!gpiod_configure_flags(&d,"fake-irq-input",0,GPIOD_IN));
 assert(cfg[22].irqtype==4 && cfg[22].stat==PINCFG_STAT_IRQEN && registers[22]==0x294);
 gpiod_free_commit(&d);
 setup(22,0xa50002b1,&d,&gc,&pc,&control,cfg,&gd,&pd);
 assert(!gpiod_request_commit(&d,"fake-input"));
 assert(!gpiod_configure_flags(&d,"fake-input",GPIO_ACTIVE_LOW,GPIOD_IN));
 assert(registers[22]==0xa500029e && test_bit(FLAG_ACTIVE_LOW,&d.flags));
 assert(!test_bit(FLAG_IS_OUT,&d.flags));gpiod_free_commit(&d);
''')

 def test_normal_gpio_inputs_are_unchanged_and_duplicate_request_is_busy(self):
  for legacy in (False,True):
   self.run_c('''
 setup(0,0x290,&d,&gc,&pc,&control,cfg,&gd,&pd);
 assert(!gpiod_request_commit(&d,"fake-key"));
 assert(!gpiod_configure_flags(&d,"fake-key",GPIO_ACTIVE_LOW,GPIOD_IN));
 assert(registers[0]==0x29e && cfg[0].stat==0 && !test_bit(FLAG_IS_OUT,&d.flags));
 unsigned before=count;assert(gpiod_request_commit(&d,"duplicate")==-EBUSY);
 assert(count==before && module_refs==1);gpiod_free_commit(&d);
 assert(!module_refs && !test_bit(FLAG_REQUESTED,&d.flags));
''',legacy=legacy)

 def test_gpiolib_error_before_direction_callback_leaves_mux_unchanged(self):
  self.run_c('''
 setup(22,0x2b0,&d,&gc,&pc,&control,cfg,&gd,&pd);
 assert(!gpiod_request_commit(&d,"fake-input"));gc.get=NULL;
 assert(gpiod_configure_flags(&d,"fake-input",0,GPIOD_IN)==-EIO);
 assert(!count && registers[22]==0x2b0 && cfg[22].stat==PINCFG_STAT_PERIPH);
 gpiod_free_commit(&d);assert(!module_refs);
''')


 def test_asis_irq_request_startup_selects_gpio_and_existing_falling_trigger(self):
  self.run_c('''
 setup(22,0xa50302b0,&d,&gc,&pc,&control,cfg,&gd,&pd);
 assert(!gpiod_request_commit(&d,"fake-touch-irq"));
 assert(!gpiod_configure_flags(&d,"fake-touch-irq",GPIO_ACTIVE_LOW,GPIOD_ASIS) && !count);
 struct irq_data irq={.gc=&gc,.hwirq=22};
 assert(!apple_s5l8940x_gpio_gpio_irq_set_type(&irq,IRQ_TYPE_EDGE_FALLING));
 assert(irq_handler_kind==1 && cfg[22].irqtype==0xa);
 assert(!apple_s5l8940x_gpio_gpio_irq_startup(&irq));
 assert(registers[22]==0xa500029a && cfg[22].stat==PINCFG_STAT_IRQEN);
 assert(write_offsets[count-1]==0x840 && writes[count-1]==(1u<<22));
 assert(!test_bit(FLAG_IS_OUT,&d.flags));gpiod_free_commit(&d);assert(!module_refs);
''')

 def test_historical_asis_irq_startup_leaves_peripheral_mux_and_loses_trigger(self):
  self.run_c('''
 setup(22,0xa50302b0,&d,&gc,&pc,&control,cfg,&gd,&pd);
 assert(!gpiod_request_commit(&d,"fake-touch-irq"));
 assert(!gpiod_configure_flags(&d,"fake-touch-irq",GPIO_ACTIVE_LOW,GPIOD_ASIS) && !count);
 struct irq_data irq={.gc=&gc,.hwirq=22};
 assert(!apple_s5l8940x_gpio_gpio_irq_set_type(&irq,IRQ_TYPE_EDGE_FALLING));
 assert(!apple_s5l8940x_gpio_gpio_irq_startup(&irq));
 assert(registers[22]==0xa50002b0 && cfg[22].stat==(PINCFG_STAT_PERIPH|PINCFG_STAT_IRQEN));
 assert(cfg[22].irqtype==0xa && !(registers[22]&0xe));
 assert(write_offsets[count-1]==0x840 && writes[count-1]==(1u<<22));
 assert(!test_bit(FLAG_IS_OUT,&d.flags));gpiod_free_commit(&d);assert(!module_refs);
''',legacy=True)


 def test_historical_blob_control_needs_no_commit_or_parent_history(self):
  actual=historical_gpio_blob()
  with tempfile.TemporaryDirectory(prefix='gpio-blob-only-') as directory:
   subprocess.run(['git','init','--quiet',directory],check=True,capture_output=True,timeout=30)
   result=subprocess.run(['git','-C',directory,'hash-object','-w','--stdin'],input=actual,
                         check=True,capture_output=True,timeout=30)
   self.assertEqual(result.stdout.decode().strip(),GPIO_BLOB)
   for commit in ('1118174a1d19ecc10ce1bc43b33a452dc7bb48e6',
                  'a9bf28f20e5ce908fd5805e7d58b3ce9d9f0e4cf'):
    self.assertNotEqual(subprocess.run(['git','-c','protocol.allow=never','-C',directory,
                                      'cat-file','-e',commit],capture_output=True,timeout=30).returncode,0)
   self.assertEqual(historical_gpio_blob(directory),actual)

 def test_absent_pinned_local_blob_is_refused_with_network_transport_disabled(self):
  with tempfile.TemporaryDirectory(prefix='gpio-blob-missing-') as directory:
   subprocess.run(['git','init','--quiet',directory],check=True,capture_output=True,timeout=30)
   with self.assertRaisesRegex(ValueError,'requires local pinned.*network transport disabled'):
    historical_gpio_blob(directory)

if __name__=='__main__':unittest.main()
