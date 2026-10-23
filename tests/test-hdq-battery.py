#!/usr/bin/env python3
"""Compile the actual driver's HDQ/cache/property functions against fake I/O.

No device, kernel module, Docker daemon or root privileges are used. The tests
exercise interrupted refreshes, cache lifetime, negative current, lock scope
and a stuck HDQ break; they are not a second implementation of the driver.
"""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'patches/files/drivers/power/supply/apple-hdq-battery.c'


def function(source, name):
    start = source.rfind('\nstatic ', 0, source.index(name + '(')) + 1
    brace = source.index('{', source.index(name + '('))
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def main():
    source = SOURCE.read_text()
    defs = source[source.index('#define ULCON'):source.index('static void hdq_drain')]
    c = r'''
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;
typedef int16_t s16;
#define __iomem
#define BIT(n) (1U << (n))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define HZ 100
#define msecs_to_jiffies(n) ((n) / 10 + 1)
#define time_before(a,b) ((long)((b) - (a)) > 0)
#define dev_warn_ratelimited(...) ((void)0)
struct device { int unused; };
struct mutex { int locked; };
struct power_supply { void *data; };
union power_supply_propval { int intval; };
enum power_supply_property {
 POWER_SUPPLY_PROP_TECHNOLOGY, POWER_SUPPLY_PROP_PRESENT,
 POWER_SUPPLY_PROP_STATUS, POWER_SUPPLY_PROP_CAPACITY,
 POWER_SUPPLY_PROP_VOLTAGE_NOW, POWER_SUPPLY_PROP_CURRENT_NOW,
 POWER_SUPPLY_PROP_TEMP, POWER_SUPPLY_PROP_CHARGE_NOW,
 POWER_SUPPLY_PROP_CHARGE_FULL, POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
 POWER_SUPPLY_PROP_CYCLE_COUNT
};
enum { POWER_SUPPLY_TECHNOLOGY_LIPO, POWER_SUPPLY_STATUS_FULL,
 POWER_SUPPLY_STATUS_CHARGING, POWER_SUPPLY_STATUS_DISCHARGING,
 POWER_SUPPLY_STATUS_NOT_CHARGING };
static unsigned long jiffies;
static unsigned char uart[128];
static unsigned int ucon, rx_index, rx_count, tx_count, phase;
static bool stuck_break;
static uint8_t response = 0xa6;
static unsigned int readl(void *addr) {
 unsigned offset = (unsigned)((unsigned char *)addr - uart);
 if (offset == 4) return stuck_break ? ucon : ucon & ~BIT(4);
 if (offset == 0x10) return phase == 2 && rx_index < rx_count;
 if (offset == 0x24) {
  unsigned bit = rx_index < 8 ? 0 : (response >> (rx_index - 8)) & 1;
  rx_index++;
  return bit ? 0xfe : 0xe0;
 }
 return 0;
}
static void writel(unsigned value, void *addr) {
 unsigned offset = (unsigned)((unsigned char *)addr - uart);
 if (offset == 4) ucon = value;
 if (offset == 0x20) { assert(value == 0xfe || value == 0xe0); tx_count++; }
}
static void udelay(unsigned n) { (void)n; jiffies++; }
static void usleep_range(unsigned a, unsigned b) { (void)a; (void)b; phase = 2; }
static int fake_read16(struct hdq_battery *b, u8 reg);
static void mutex_lock(struct mutex *lock) { assert(!lock->locked); lock->locked=1; }
static void mutex_unlock(struct mutex *lock);
static void *power_supply_get_drvdata(struct power_supply *s) { return s->data; }
'''
    # Forward declaration is needed before the fake-read prototype.
    c = c.replace('static int fake_read16(', 'struct hdq_battery;\nstatic int fake_read16(')
    c += defs + '\n'
    for name in ('hdq_drain', 'hdq_read8', 'hdq_read16'):
        c += function(source, name) + '\n'
    c += 'static int real_read16(struct hdq_battery *b, u8 reg) { return hdq_read16(b, reg); }\n'
    c += '#define hdq_read16 fake_read16\n'
    for name in ('hdq_refresh', 'hdq_get_property'):
        c += function(source, name) + '\n'
    c += r'''
static int calls, fail_at=-1, new_sample[9];
static struct hdq_battery *change_on_unlock;
static int fake_read16(struct hdq_battery *b, u8 reg) {
 static const u8 regs[] = { BQ_SOC,BQ_VOLT,BQ_AI,BQ_TEMP,BQ_RM,BQ_FCC,BQ_FLAGS,BQ_CYCT,BQ_DCAP };
 int n=calls++;
 assert(b->lock.locked);
 assert(reg == regs[n % 9]);
 return n == fail_at ? -EIO : new_sample[n % 9];
}
static void mutex_unlock(struct mutex *lock) {
 assert(lock->locked); lock->locked=0;
 if (change_on_unlock) change_on_unlock->ai=777;
}
static void reset_bus(void) {
 ucon=0x405; rx_index=tx_count=phase=0; rx_count=16; stuck_break=false;
}
static void reset_sample(void) {
 int sample[] = { 64,3900,65536-250,2981,2500,4000,0,25,4400 };
 memcpy(new_sample,sample,sizeof(sample)); calls=0; fail_at=-1;
}
static struct hdq_battery battery(void) {
 struct hdq_battery b = { .base=uart, .soc=12,.volt=3700,.ai=-300,
  .temp=2931,.rm=500,.fcc=4100,.flags=BQ_FLAG_DSG,.cyct=26,.dcap=4500,
  .valid=true,.stamp=0 };
 return b;
}
int main(void) {
 struct hdq_battery b;
 union power_supply_propval value;
 struct power_supply psy;
 reset_bus(); b=battery();
 assert(hdq_read8(&b,0x14)==response && tx_count==8);
 reset_bus(); rx_count=15; assert(hdq_read8(&b,0x14)==-EIO);
 reset_bus(); stuck_break=true;
 assert(hdq_read8(&b,0x14)==-ETIMEDOUT && tx_count==0);
 /* A transport failure is returned immediately, without retrying I/O. */
 reset_bus(); rx_count=0;
 assert(real_read16(&b,0x14)==-EIO && tx_count==8);
 for (int error=0; error<9; error++) {
  b=battery(); struct hdq_battery old=b; reset_sample(); fail_at=error;
  jiffies=5*HZ; mutex_lock(&b.lock);
  assert(hdq_refresh(&b)==-EIO);
  mutex_unlock(&b.lock);
  assert(!memcmp(&b,&old,sizeof(b)));
 }
 b=battery(); reset_sample(); jiffies=5*HZ;
 mutex_lock(&b.lock); assert(hdq_refresh(&b)==0); mutex_unlock(&b.lock);
 assert(calls==9 && b.soc==64 && b.ai==-250 && b.stamp==jiffies);
 mutex_lock(&b.lock); assert(hdq_refresh(&b)==0); mutex_unlock(&b.lock);
 assert(calls==9);
 /* Cache lifetime also works across unsigned-jiffies wraparound. */
 b.stamp=ULONG_MAX-100; jiffies=25; calls=0;
 mutex_lock(&b.lock); assert(hdq_refresh(&b)==0); mutex_unlock(&b.lock);
 assert(calls==0);
 b=battery(); b.stamp=jiffies; b.ai=-1; b.flags=0; psy.data=&b;
 assert(hdq_get_property(&psy,POWER_SUPPLY_PROP_STATUS,&value)==0);
 assert(value.intval==POWER_SUPPLY_STATUS_DISCHARGING);
 change_on_unlock=&b;
 assert(hdq_get_property(&psy,POWER_SUPPLY_PROP_CURRENT_NOW,&value)==0);
 assert(value.intval==-1000 && b.ai==777);
 change_on_unlock=NULL;
 b=battery(); b.valid=false; reset_sample(); fail_at=3;
 assert(hdq_get_property(&psy,POWER_SUPPLY_PROP_PRESENT,&value)==0 && value.intval==0);
 assert(!b.lock.locked && !b.valid);
 reset_sample(); fail_at=4;
 assert(hdq_get_property(&psy,POWER_SUPPLY_PROP_CURRENT_NOW,&value)==-EIO);
 assert(!b.lock.locked);
 reset_sample();
 assert(hdq_get_property(&psy,POWER_SUPPLY_PROP_TEMP,&value)==0 && value.intval==250);
 assert(hdq_get_property(&psy,(enum power_supply_property)999,&value)==-EINVAL);
 assert(!b.lock.locked);
 /* Reject an invalid percentage instead of publishing a corrupt sample. */
 unsigned invalid_soc[] = { 101, 65535 };
 for (unsigned i=0; i<ARRAY_SIZE(invalid_soc); i++) {
  b=battery(); struct hdq_battery old=b; psy.data=&b;
  reset_sample(); new_sample[0]=invalid_soc[i]; jiffies=5*HZ; value.intval=1234;
  assert(hdq_get_property(&psy,POWER_SUPPLY_PROP_CAPACITY,&value)==-ERANGE);
  assert(value.intval==1234 && !memcmp(&b,&old,sizeof(b)));
 }
 for (unsigned percent=0; percent<=100; percent+=100) {
  b=battery(); psy.data=&b; reset_sample(); new_sample[0]=percent; jiffies=5*HZ;
  assert(hdq_get_property(&psy,POWER_SUPPLY_PROP_CAPACITY,&value)==0);
  assert(value.intval==(int)percent && b.valid && calls==9 && !b.lock.locked);
 }
 puts("HDQ battery: framing/timeouts, nine injected read failures, cache/wraparound, SOC bounds, units and lock scope passed");
}
'''
    with tempfile.TemporaryDirectory(prefix='cascadia-hdq-test-') as temp:
        path = Path(temp)
        (path / 'test.c').write_text(c)
        subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra',
                        '-Werror', '-fsanitize=undefined', '-g', str(path / 'test.c'),
                        '-o', str(path / 'test')], check=True)
        subprocess.run([str(path / 'test')], check=True)


if __name__ == '__main__':
    main()
