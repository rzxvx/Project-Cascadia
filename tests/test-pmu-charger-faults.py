#!/usr/bin/env python3
"""Actual upstream charger functions with fake I/O; no device access.

CASCADIA_PMU_CHARGER_SOURCE can select a pinned historical negative control.
These tests establish software error handling, not electrical shutdown.
"""
import os
from pathlib import Path
import re
import resource
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = Path(os.environ.get('CASCADIA_PMU_CHARGER_SOURCE',
    ROOT / 'patches/files/drivers/power/supply/apple-pmu-charger.c'))


def function(text, name):
    m = re.search(r'^static [^\n]*\b' + name + r'\(', text, re.M)
    assert m, name
    start = text.index('{', m.start())
    end, depth = start + 1, 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[m.start():end]


class Faults(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        src = SOURCE.read_text()
        fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
typedef uint8_t u8;
#define BIT(n) (1U << (n))
#define READ_ONCE(x) (x)
#define min(a,b) ((a)<(b)?(a):(b))
#define min_t(t,a,b) min((t)(a),(t)(b))
#define max_t(t,a,b) ((t)(a)>(t)(b)?(t)(a):(t)(b))
#define clamp(x,lo,hi) ((x)<(lo)?(lo):((x)>(hi)?(hi):(x)))
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define dev_warn(dev,...) ((void)(dev),(void)fprintf(stderr,__VA_ARGS__))
#define dev_info(dev,...) dev_warn(dev,__VA_ARGS__)
#define dev_err(dev,...) dev_warn(dev,__VA_ARGS__)
struct device {int unused;}; struct regmap {int unused;};
struct power_supply {int unused;};
struct work_struct {int unused;};
struct delayed_work {struct work_struct work;};
struct mutex {int held;};
union power_supply_propval {int intval;};
#define POWER_SUPPLY_PROP_TEMP 1
static struct delayed_work *to_delayed_work(struct work_struct *w) {return container_of(w,struct delayed_work,work);}
static void mutex_lock(struct mutex *m) {assert(!m->held);m->held=1;}
static void mutex_unlock(struct mutex *m) {assert(m->held);m->held=0;}
static int writes, reads, fail_write, fail_read, missing, temp_error;
static int temperature=250, put_count, scheduled;
static u8 regs[256];
static struct power_supply battery;
static int regmap_write(struct regmap *m,unsigned int r,unsigned int v) {
 (void)m;assert(r==0x3d);writes++;if(fail_write)return -EIO;regs[r]=v;return 0;
}
static int regmap_read(struct regmap *m,unsigned int r,unsigned int *v) {
 (void)m;assert(r==0x3d||r==0x0a);reads++;if(fail_read)return -EIO;*v=regs[r];return 0;
}
static struct power_supply *power_supply_get_by_name(const char *name) {(void)name;return missing?NULL:&battery;}
static int power_supply_get_property(struct power_supply *p,int prop,union power_supply_propval *v) {
 assert(p==&battery && prop==POWER_SUPPLY_PROP_TEMP);if(temp_error)return -EIO;v->intval=temperature;return 0;
}
static void power_supply_put(struct power_supply *p) {assert(p==&battery);put_count++;}
static void power_supply_changed(struct power_supply *p) {(void)p;}
static void msleep(unsigned int ms) {assert(ms==10);}
static unsigned int msecs_to_jiffies(unsigned int ms) {return ms;}
static void schedule_delayed_work(struct delayed_work *w,unsigned int delay) {(void)w;assert(delay==1000);scheduled++;}
static bool armed;
static int max_input_ma=1000;
'''
        fixture += '\n'.join(re.findall(r'^#define .*$', src, re.M)) + '\n'
        fixture += re.search(r'struct pmu_chg \{.*?\n\};', src, re.S).group() + '\n'
        for name in ['pmu_chg_ma_to_code', 'pmu_chg_code_to_ma', 'pmu_chg_write',
                     'pmu_chg_read', 'pmu_chg_sagged', 'pmu_chg_ramp',
                     'pmu_chg_battery_hot', 'pmu_chg_target', 'pmu_chg_work']:
            fixture += function(src, name) + '\n'
        fixture += r'''
int main(int argc,char **argv) {
 assert(argc==2);int which=atoi(argv[1]);
 struct pmu_chg c={.req_ma=500,.ceiling=0xff,.probe_code=2};
 regs[0x0a]=0x40;regs[0x3d]=2;
 switch(which) {
 case 0: /* Disarmed must never ramp. */
  pmu_chg_work(&c.work.work);assert(writes==0);break;
 case 1: /* Qualified synthetic temperature: unchanged nominal 500mA encoding. */
  armed=true;pmu_chg_work(&c.work.work);assert(regs[0x3d]==0x22 && writes==4);break;
 case 2: /* A missing battery cannot permit raising the input limit. */
  armed=true;missing=1;pmu_chg_work(&c.work.work);assert(writes==0 && regs[0x3d]==2);break;
 case 3: /* Temperature read error withdraws a previously raised target. */
  armed=true;temp_error=1;regs[0x3d]=0x22;pmu_chg_work(&c.work.work);
  assert(writes==1 && regs[0x3d]==2 && put_count==1);break;
 case 4: /* A failed restore is not acknowledged as completed. */
  c.was_armed=true;c.last_written=0x22;c.logged_code=0x22;regs[0x3d]=0x22;fail_write=1;
  pmu_chg_work(&c.work.work);assert(writes==1 && c.was_armed && c.last_written==0x22 && c.logged_code==0x22);
  fail_write=0;pmu_chg_work(&c.work.work);assert(writes==2 && !c.was_armed && c.last_written==0 && regs[0x3d]==2);break;
 case 5: /* Restore does not depend on status/limit reads succeeding. */
  c.was_armed=true;regs[0x3d]=0x22;fail_read=1;
  pmu_chg_work(&c.work.work);assert(writes==1 && reads==0 && !c.was_armed && regs[0x3d]==2);break;
 case 6: /* Missing/failed temperature latches derating through the hysteresis band. */
  missing=1;assert(pmu_chg_target(&c)==2 && c.hot);
  missing=0;temperature=420;assert(pmu_chg_target(&c)==2 && c.hot);
  temperature=390;assert(pmu_chg_target(&c)==0x22 && !c.hot);break;
 default:assert(0);
 }
 assert(!c.lock.held);puts("PASS synthetic I/O; no physical cutoff claim");return 0;
}
'''
        cls.tmp = tempfile.TemporaryDirectory()
        path = Path(cls.tmp.name)
        (path / 'fixture.c').write_text(fixture)
        cls.exe = path / 'fixture'
        subprocess.run(['gcc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=undefined', '-fno-sanitize-recover=all',
                        str(path / 'fixture.c'), '-o', str(cls.exe)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def case(self, n):
        r = subprocess.run([str(self.exe), str(n)], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_disarmed(self): self.case(0)
    def test_normal_ramp(self): self.case(1)
    def test_missing_battery(self): self.case(2)
    def test_temperature_error(self): self.case(3)
    def test_failed_disarm_restore(self): self.case(4)
    def test_disarm_without_status_reads(self): self.case(5)
    def test_error_hysteresis(self): self.case(6)


if __name__ == '__main__':
    unittest.main()
