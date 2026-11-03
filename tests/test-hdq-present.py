#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compile the actual HDQ getter with a refresh-result stub, without UART I/O.

Only the getter is under test. Refresh/cache implementation and PR9's other
changes are not prerequisites. No kernel, device, firmware or service is used.
"""
import os
from pathlib import Path
import resource
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = Path(os.environ.get('CASCADIA_HDQ_SOURCE', str(
    ROOT / 'patches/files/drivers/power/supply/apple-hdq-battery.c')))


def getter(source):
    start = source.index('static int hdq_get_property(')
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


class Presence(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = os.environ.get('CC', 'cc')
        if not shutil.which(compiler):
            raise unittest.SkipTest('a C compiler is required')
        cls.temp = tempfile.TemporaryDirectory(prefix='cascadia-hdq-present-')
        path = Path(cls.temp.name)
        source = SOURCE.read_text()
        body = getter(source)
        flags = '\n'.join(line for line in source.splitlines()
                          if line.startswith('#define') and 'BQ_FLAG_' in line)
        fixture = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#define BIT(n) (1U << (n))
struct mutex { int held; };
struct hdq_battery {
 struct mutex lock;
 int soc, volt, ai, temp, rm, fcc, flags, cyct, dcap;
};
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
static int refresh_result, refresh_calls, locks, unlocks;
static void *power_supply_get_drvdata(struct power_supply *p) { return p->data; }
static void mutex_lock(struct mutex *m) { assert(!m->held); m->held=1; locks++; }
static void mutex_unlock(struct mutex *m) { assert(m->held); m->held=0; unlocks++; }
static int hdq_refresh(struct hdq_battery *b)
{
 assert(b->lock.held);
 refresh_calls++;
 return refresh_result;
}
'''
        fixture += flags + '\n' + body + r'''
int main(int argc, char **argv)
{
 struct hdq_battery b = { .soc=64, .volt=3900, .ai=-250, .temp=2981 };
 struct power_supply p = { .data=&b };
 union power_supply_propval value;
 assert(argc==2);
 if (!strcmp(argv[1], "errors")) {
  const int errors[] = { -EIO, -ETIMEDOUT, -EREMOTEIO, -ERANGE };
  for (unsigned int i=0; i<sizeof(errors)/sizeof(errors[0]); i++) {
   refresh_result=errors[i]; refresh_calls=locks=unlocks=0; value.intval=1234;
   assert(hdq_get_property(&p,POWER_SUPPLY_PROP_PRESENT,&value)==errors[i]);
   assert(value.intval==1234 && refresh_calls==1 && locks==1 && unlocks==1 && !b.lock.held);
  }
 } else if (!strcmp(argv[1], "present")) {
  value.intval=1234;
  assert(hdq_get_property(&p,POWER_SUPPLY_PROP_PRESENT,&value)==0 && value.intval==1);
  assert(refresh_calls==1 && locks==1 && unlocks==1 && !b.lock.held);
 } else if (!strcmp(argv[1], "other")) {
  refresh_result=-EIO; value.intval=1234;
  assert(hdq_get_property(&p,POWER_SUPPLY_PROP_CURRENT_NOW,&value)==-EIO && value.intval==1234);
  assert(refresh_calls==1 && locks==1 && unlocks==1 && !b.lock.held);
  refresh_result=0;
  assert(hdq_get_property(&p,POWER_SUPPLY_PROP_CURRENT_NOW,&value)==0 && value.intval==-250000);
  assert(hdq_get_property(&p,POWER_SUPPLY_PROP_VOLTAGE_NOW,&value)==0 && value.intval==3900000);
  assert(hdq_get_property(&p,POWER_SUPPLY_PROP_TEMP,&value)==0 && value.intval==250);
  assert(!b.lock.held && locks==unlocks);
 } else {
  refresh_result=-EIO;
  assert(hdq_get_property(&p,POWER_SUPPLY_PROP_TECHNOLOGY,&value)==0);
  assert(value.intval==POWER_SUPPLY_TECHNOLOGY_LIPO && refresh_calls==0 && locks==0 && unlocks==0);
 }
 puts("actual HDQ getter fixture PASS");
}
'''
        (path / 'fixture.c').write_text(fixture)
        cls.binary = path / 'fixture'
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        subprocess.run([compiler, '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=undefined', '-fno-sanitize-recover=all',
                        str(path / 'fixture.c'), '-o', str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_fixture(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True,
                                text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('fixture PASS', result.stdout)

    def test_refresh_errors_are_not_false_absence(self):
        self.run_fixture('errors')

    def test_success_reports_present(self):
        self.run_fixture('present')

    def test_other_properties_keep_error_and_unit_behavior(self):
        self.run_fixture('other')

    def test_static_technology_needs_no_refresh(self):
        self.run_fixture('technology')


if __name__ == '__main__':
    unittest.main(verbosity=2)
