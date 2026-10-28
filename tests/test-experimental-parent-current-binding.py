#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Actual new C accessor/health/current probe-prefix; explicit driver-core fakes, no hardware."""
from pathlib import Path
import re,subprocess,tempfile,json,os,hashlib,resource
P=Path(__file__).resolve().parent;R=Path(os.environ.get('CASCADIA_BINDING_ROOT',str(P.parent)));C=R
def fn(s,n):
 m=re.search(r'^[A-Za-z_][^;{}]*\b'+n+r'\s*\([^;{}]*\)\s*\{',s,re.M);assert m,n
 i=m.end();level=1
 while level:
  if s[i]=='{':level+=1
  elif s[i]=='}':level-=1
  i+=1
 return s[m.start():i]+'\n'
def gate(s,n):
 a=fn(s,'d_probe');a=a[:a.index('\td = devm_kzalloc(')];a=a.replace('d_probe(',n+'(')
 return a+'\t(void)d; (void)cfg; (void)i; (void)control; (void)ret;\n\treturn 717; /* test stops before first allocation, not a complete probe */\n}\n'
PRE=r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#define EPROBE_DEFER 517
#define READ_ONCE(x) (x)
#define IS_ENABLED(x) (x)
#define ERR_PTR(e) ((void*)(intptr_t)(e))
#define PTR_ERR(p) ((intptr_t)(p))
#define IS_ERR(p) ((uintptr_t)(p)>=(uintptr_t)-4095)
#define IS_ERR_OR_NULL(p) (!(p)||IS_ERR(p))
#define DL_FLAG_AUTOREMOVE_CONSUMER 2
#define lockdep_assert_held(x) ((void)(x))
typedef uint8_t u8;
typedef struct{int n;}atomic_t;
static int atomic_read(const atomic_t*a){return a->n;}
enum device_link_state {DL_STATE_DORMANT,DL_STATE_CONSUMER_PROBE,DL_STATE_ACTIVE,DL_STATE_SUPPLIER_UNBIND};
enum {DL_DEV_NO_DRIVER,DL_DEV_PROBING,DL_DEV_DRIVER_BOUND,DL_DEV_UNBINDING};
struct device_driver{int id;};struct i2c_driver{struct device_driver driver;};
struct regmap{int id;};struct i2c_client;struct of_device_id;
struct device{struct device*parent;struct device_driver*driver;struct{int status;}links;bool platform,i2c;void*data;struct i2c_client*client;struct regmap*map;const struct of_device_id*match;void*of_node;};
struct i2c_client{struct device dev;int id;};struct platform_device{struct device dev;};
struct device_link{enum device_link_state status;};
struct apple_pmu_i2c_info{int id;};struct of_device_id{const void*data;};
struct d1946_bank0_bus{struct i2c_client*client;int map_lock;atomic_t state,fault;};
struct apple_pmu_i2c{struct device*dev;struct regmap*regmap;struct d1946_bank0_bus*bank0;};
struct power_supply_config{int unused;};struct d1946_current{int unused;};
static struct i2c_driver apple_pmu_i2c_driver={{1}};static struct device_driver foreign_driver={2};
static struct apple_pmu_i2c_info apple_pmu_i2c_d1946_info={1},d2333_info={2};
static const struct of_device_id apple_pmu_i2c_of_match[]={{&apple_pmu_i2c_d1946_info},{&d2333_info}};
static struct device_link link;static bool board,link_fail,drop_parent,swap_driver,stop_at_lookup;
static unsigned int casts,link_calls,map_lookups;static bool allow_experiment,retained_analog_confirmed,owned_usb2_source;
static bool dev_is_platform(struct device*d){return d->platform;}
static struct i2c_client*i2c_verify_client(struct device*d){return d->i2c?d->client:NULL;}
static const struct of_device_id*of_match_device(const struct of_device_id*t,struct device*d){assert(t==apple_pmu_i2c_of_match);return d->match;}
static bool of_machine_is_compatible(const char*s){assert(!strcmp(s,"apple,p105ap"));return board;}
static struct device_link*device_link_add(struct device*c,struct device*p,unsigned int flags){assert(c->parent==p&&flags==DL_FLAG_AUTOREMOVE_CONSUMER);link_calls++;if(drop_parent)p->links.status=DL_DEV_UNBINDING;if(swap_driver)p->driver=&foreign_driver;return link_fail?NULL:&link;}
static void*dev_get_drvdata(struct device*d){casts++;return d->data;}
static struct regmap*dev_get_regmap(struct device*d,const char*n){assert(!n);map_lookups++;if(stop_at_lookup){struct apple_pmu_i2c*p=d->data;p->bank0->state.n=2;}return d->map;}
static bool of_property_read_bool(void*n,const char*s){(void)n;assert(!strcmp(s,"apple,p105-current-only-experiment")||!strcmp(s,"apple,no-input-calibration"));return true;}
'''
POST=r'''
static struct platform_device child;static struct i2c_client client,other;static struct apple_pmu_i2c pmu;static struct d1946_bank0_bus bus;static struct regmap map,other_map;static int foreign_context;
static void setup(void){memset(&child,0,sizeof(child));memset(&client,0,sizeof(client));memset(&other,0,sizeof(other));memset(&pmu,0,sizeof(pmu));memset(&bus,0,sizeof(bus));child.dev.parent=&client.dev;child.dev.platform=true;child.dev.links.status=DL_DEV_PROBING;client.dev.driver=&apple_pmu_i2c_driver.driver;client.dev.i2c=true;client.dev.client=&client;client.dev.links.status=DL_DEV_DRIVER_BOUND;client.dev.match=&apple_pmu_i2c_of_match[0];client.dev.data=&pmu;client.dev.map=&map;pmu.dev=&client.dev;pmu.regmap=&map;pmu.bank0=&bus;bus.client=&client;bus.state.n=BANK0_RUNNING;link.status=DL_STATE_CONSUMER_PROBE;board=true;link_fail=drop_parent=swap_driver=stop_at_lookup=false;casts=link_calls=map_lookups=0;allow_experiment=retained_analog_confirmed=true;}
static void denied(int err,unsigned int maxcasts){struct regmap*p=apple_pmu_d1946_bank0_get_for_child(&child.dev);assert(IS_ERR(p)&&PTR_ERR(p)==err);assert(casts<=maxcasts);}
static void identities(void){setup();assert(apple_pmu_d1946_bank0_get_for_child(&child.dev)==&map&&casts==1&&link_calls==1&&map_lookups==1);setup();link.status=DL_STATE_ACTIVE;assert(apple_pmu_d1946_bank0_get_for_child(&child.dev)==&map);setup();client.dev.driver=&foreign_driver;client.dev.data=&foreign_context;denied(-ENODEV,0);setup();client.dev.driver=NULL;denied(-EPROBE_DEFER,0);setup();client.dev.i2c=false;denied(-ENODEV,0);setup();client.dev.match=&apple_pmu_i2c_of_match[1];denied(-EOPNOTSUPP,0);setup();client.dev.match=NULL;denied(-EOPNOTSUPP,0);setup();board=false;denied(-EOPNOTSUPP,0);setup();pmu.regmap=NULL;denied(-ENODEV,1);setup();pmu.regmap=ERR_PTR(-EIO);denied(-ENODEV,1);setup();pmu.regmap=&other_map;denied(-ENODEV,1);setup();pmu.dev=&other.dev;denied(-ENODEV,1);setup();pmu.bank0=NULL;denied(-EOPNOTSUPP,1);setup();bus.client=&other;denied(-ENODEV,1);setup();client.dev.data=NULL;denied(-ENODEV,1);}
static void lifetimes(void){for(int st=DL_DEV_NO_DRIVER;st<=DL_DEV_UNBINDING;st++){if(st==DL_DEV_DRIVER_BOUND)continue;setup();client.dev.links.status=st;denied(-EPROBE_DEFER,0);assert(!link_calls);}for(int st=DL_DEV_NO_DRIVER;st<=DL_DEV_UNBINDING;st++){if(st==DL_DEV_PROBING)continue;setup();child.dev.links.status=st;denied(-EINVAL,0);}setup();child.dev.platform=false;denied(-EINVAL,0);setup();child.dev.parent=NULL;denied(-EINVAL,0);setup();link_fail=true;denied(-EPROBE_DEFER,0);setup();drop_parent=true;denied(-EPROBE_DEFER,0);setup();swap_driver=true;denied(-EPROBE_DEFER,0);setup();link.status=DL_STATE_DORMANT;denied(-EPROBE_DEFER,0);setup();link.status=DL_STATE_SUPPLIER_UNBIND;denied(-EPROBE_DEFER,0);}
static void health(void){setup();bus.state.n=BANK0_SEALED;denied(-ESHUTDOWN,1);setup();bus.state.n=BANK0_STOPPED;denied(-ESHUTDOWN,1);setup();bus.fault.n=1;denied(-EIO,1);assert(d1946_bank0_check_running(NULL,&client)==-EINVAL);assert(d1946_bank0_check_running(&bus,NULL)==-EINVAL);setup();stop_at_lookup=true;assert(apple_pmu_d1946_bank0_get_for_child(&child.dev)==&map);assert(bank0_admit(&bus)==-ESHUTDOWN);/* getter snapshot can be revoked, real callback admission still denies */}
static void probe_routes(void){for(unsigned int owned=0;owned<2;owned++){owned_usb2_source=owned;setup();pmu.bank0=NULL;assert(candidate_probe_gate(&child)==-EOPNOTSUPP);assert(map_lookups==0);setup();client.dev.links.status=DL_DEV_PROBING;assert(candidate_probe_gate(&child)==-EPROBE_DEFER);assert(!casts&&!map_lookups);setup();client.dev.driver=&foreign_driver;client.dev.data=&foreign_context;assert(candidate_probe_gate(&child)==-ENODEV);assert(!casts);setup();assert(candidate_probe_gate(&child)==717);/* first allocation would be next, no allocation/I/O has executed */}}
static void default_off(void){setup();assert(candidate_probe_gate(&child)==-EOPNOTSUPP);assert(!casts&&!link_calls&&!map_lookups);allow_experiment=false;assert(candidate_probe_gate(&child)==-EPERM);assert(!casts&&!link_calls&&!map_lookups);}
static int historical(void){setup();pmu.bank0=NULL;int got=historical_probe_gate(&child);assert(got==717&&casts==1);printf("old raw-parent prefix accepts ordinary unbanked map: %d\n",got);setup();pmu.bank0=NULL;assert(candidate_probe_gate(&child)==-EOPNOTSUPP);return got==-EOPNOTSUPP?0:23;}
int main(int argc,char**argv){assert(argc==2);int n=atoi(argv[1]);switch(n){case0:break;default:break;}if(n==0)identities();else if(n==1)lifetimes();else if(n==2)health();else if(n==3)probe_routes();else if(n==4)default_off();else if(n==5)return historical();else abort();return 0;}
'''.replace('case0:break;','case 0:break;')
def main():
 resource.setrlimit(resource.RLIMIT_CORE,(0,0));mfd=(C/'experiments/p105-parent-pmu/apple-pmu-i2c.c').read_text();bus=(C/'experiments/p105-parent-pmu/apple-pmu-d1946-bank0.c').read_text();cur=(C/'experiments/p105-current/apple-d1946-current-only.c').read_text();old=(R/'experiments/p105-current/historical-d-probe-unbanked.c').read_text()
 assert hashlib.sha256(old.encode()).hexdigest()=='276419df5efe6ee73d3abeb7400405672cb4be72825b2404555bd035d7376cec','historical actual probe fixture changed'
 probe=fn(cur,'d_probe');assert probe.index('map = d_parent_map(&pdev->dev)')<probe.index('d = devm_kzalloc(')<probe.index('ret = d_providers_acquire(d)')<probe.index('ret = d_read(d, 0x12')
 assert 'pmu = dev_get_drvdata(pdev->dev.parent)' not in cur
 defines='\n'.join(re.findall(r'^#define BANK0_(?:RUNNING|SEALED|STOPPED) .*$',bus,re.M))+'\n'
 body=PRE+defines+fn(bus,'d1946_bank0_check_running')+fn(bus,'bank0_admit')+fn(mfd,'apple_pmu_d1946_bank0_get_for_child')+fn(cur,'d_parent_map')+gate(cur,'candidate_probe_gate')+gate(old,'historical_probe_gate')+POST
 out=[]
 with tempfile.TemporaryDirectory() as td:
  t=Path(td);(t/'fixture.c').write_text(body)
  for config,cases in [(1,[0,1,2,3,5]),(0,[4])]:
   exe=t/('binding-'+str(config));cmd=['gcc','-std=gnu11','-Wall','-Wextra','-Werror','-g','-O1','-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie','-DCONFIG_APPLE_PMU_D1946_BANK0_EXPERIMENT='+str(config),str(t/'fixture.c'),'-o',str(exe)];subprocess.run(cmd,check=True)
   for n in cases:
    r=subprocess.run([str(exe),str(n)],text=True,capture_output=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1:abort_on_error=1','UBSAN_OPTIONS':'halt_on_error=1'});expected=23 if n==5 else 0;assert r.returncode==expected,(config,n,r.stdout,r.stderr);out.append({'config':config,'group':n,'exit':r.returncode,'expected_exit':expected,'historical_denial_contract_counterexample':n==5,'stdout':r.stdout});print('PASS config',config,'group',n,'expected_exit',expected,flush=True)
 result_path=os.environ.get('CASCADIA_BINDING_RESULTS');result={'groups':out,'actual_functions':['apple_pmu_d1946_bank0_get_for_child','d1946_bank0_check_running','bank0_admit','d_parent_map'],'probe_scope':'actual prefix through all admission gates, stops at first allocation; no hardware/I/O/provider factory','stubs':'driver-core link states/type/OF/map lookup, atomic reads; explicit valid fake devices; no unrelated pointer dereference','sanitizers':'ASan/UBSan; Wall/Wextra/Werror'}
 if result_path:Path(result_path).write_text(json.dumps(result,indent=2)+'\n')
if __name__=='__main__':main()
