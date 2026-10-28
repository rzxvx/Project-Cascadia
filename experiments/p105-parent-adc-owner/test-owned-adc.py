# SPDX-License-Identifier: GPL-2.0-only
from pathlib import Path
import hashlib,json,os,re,subprocess,tempfile,unittest
P=Path(__file__).resolve().parent

def function(s,name):
 m=re.search(r'(?:static\s+)?(?:int|void|bool)\s+(?:__maybe_unused\s+)?'+name+r'\s*\([^;{]*\)\s*\{',s);assert m,name
 start=m.start();i=m.end();depth=1
 while depth:
  depth += (s[i]=='{')-(s[i]=='}');i+=1
 return s[start:i]+'\n'

class C(unittest.TestCase):
 @classmethod
 def setUpClass(c):
  provenance=json.loads((P/'source-provenance.json').read_text())
  for name,sha in provenance['fixture_hashes'].items():
   if hashlib.sha256((P/name).read_bytes()).hexdigest()!=sha:raise AssertionError('fixture changed: '+name)
  pairs={'a5-address-command-fixture.h':('i2c-s5l8940x.c',('apple_s5l8940x_i2c_start_write',)), 'a5-entrypoints-fixture.h':('i2c-s5l8940x.c',('apple_s5l8940x_i2c_xfer','apple_s5l8940x_i2c_xfer_atomic')),'mfd-parent-constructor-fixture.h':('apple-pmu-i2c.c',('apple_pmu_d1946_owned_parent_init',)), 'system-boundary-fixture.h':('apple-pmu-i2c.c',('apple_pmu_d1946_power_off_prepare','apple_pmu_p105_suspend')), 'normal-child-fixture.h':('apple-pmu-i2c.c',('apple_pmu_gpio_cfg','apple_pmu_gpio_get_direction','apple_pmu_gpio_get','apple_pmu_gpio_set','apple_pmu_gpio_direction_output','apple_pmu_gpio_direction_input','apple_pmu_d1946_prepare_checked'))}
  for fixture,(source,names) in pairs.items():
   for name in names:
    if function((P/fixture).read_text(),name)!=function((P/source).read_text(),name):raise AssertionError('actual production body differs: '+name)
  source_root=Path(os.environ['CASCADIA_TEST_SOURCE_ROOT']) if os.environ.get('CASCADIA_TEST_SOURCE_ROOT') else None
  if source_root:
   for name,sha in provenance['normal_child_sources'].items():
    if hashlib.sha256((source_root/name).read_bytes()).hexdigest()!=sha:raise AssertionError('original source changed: '+name)
   for name in ('apple_s5l8940x_i2c_do_xfer','apple_s5l8940x_i2c_xfer_combined','apple_s5l8940x_i2c_xfer_msg'):
    if function((source_root/'patches/files-sandcastle-port/drivers/i2c/busses/i2c-s5l8940x.c').read_text(),name)!=function((P/'i2c-s5l8940x.c').read_text(),name):raise AssertionError('existing A5 transfer body changed: '+name)
   for file,name in [('patches/files/drivers/video/backlight/apple-pmu-wled.c','apple_wled_update'),('patches/files-sandcastle-port/drivers/regulator/apple-pmu-pwrsw.c','apple_pmu_i2c_pwrsw_set')]:
    if function((source_root/file).read_text(),name)!=function((P/'normal-child-fixture.h').read_text(),name):raise AssertionError('unchanged child body mismatch: '+name)
   linux=source_root/'build/linux'
   for file,names in [('drivers/base/regmap/regmap.c',('regmap_read','regmap_write','_regmap_update_bits','regmap_update_bits_base')),('drivers/i2c/i2c-core-base.c',('__i2c_transfer',))]:
    fixture='regmap-entrypoints-fixture.h' if 'regmap' in file else 'core-retry-fixture.h'
    if (linux/file).exists():
     for name in names:
      if function((linux/file).read_text(),name)!=function((P/fixture).read_text(),name):raise AssertionError('current Linux body differs: '+name)
  c.tmp=tempfile.TemporaryDirectory();p=Path(c.tmp.name);(p/'linux').mkdir()
  for h in ('atomic.h','i2c.h','mutex.h','regmap.h','err.h','errno.h','interrupt.h','kernel.h','lockdep.h','string.h','sched.h','types.h','device.h','slab.h','delay.h','ktime.h'):(p/'linux'/h).write_text('#include "fake-linux.h"\n')
  bodies=(P/'regmap-entrypoints-fixture.h').read_text();(p/'regmap-bodies.h').write_text(''.join(function(bodies,n)for n in ('regmap_read','regmap_write','_regmap_update_bits','regmap_update_bits_base')))
  c.exe=p/'fixture';subprocess.run(['gcc','-std=gnu11','-pthread','-Wall','-Wextra','-Werror','-Wno-unused-function','-fsanitize=address,undefined','-fno-pie','-no-pie','-I',str(p),'-I',str(P),str(P/'fixture.c'),'-o',str(c.exe)],check=True)
 @classmethod
 def tearDownClass(c):c.tmp.cleanup()
 def case(self,n):subprocess.run([str(self.exe),str(n)],check=True)
 def test_actual_client_reservation_blocks_force_raw_until_drained_stop(self):self.case(9)
 def test_actual_unchanged_GPIO_regulator_WLED_and_poweroff(self):self.case(10);self.case(11)
 def test_actual_MFD_constructor_every_devres_failure_rolls_back_reservation(self):self.case(12);self.case(13)
 def test_actual_system_sleep_and_poweroff_prepare_close_before_IO(self):self.case(14)
 def test_actual_A5_fifo_high_address_alias_is_rejected_before_any_msg(self):self.case(15)
 def test_default_unknown_physical_quiescence(self):self.case(0)
 def test_actual_owned_sequence_fake_quiescence_only(self):self.case(1)
 def test_each_physical_selector_data_error_and_short(self):self.case(2)
 def test_epoch_invalidation_and_wrap(self):self.case(3)
 def test_timeout_unknown_never_rearmed(self):self.case(4)
 def test_full_conversion_excludes_parent_children_and_raw_i2c(self):self.case(5)
 def test_close_and_parent_stop_drain_actual_conversion(self):self.case(6)
 def test_actual_core_retry_race_negative_control_vs_once_leaf(self):self.case(7)
 def test_once_leaf_identity_suspended_atomic_and_message_bounds(self):self.case(8)
 def test_existing84_native_parent_transactions_unchanged(self):
  data=(P/'native-parent-oracle.json').read_bytes();self.assertEqual(hashlib.sha256(data).hexdigest(),'f87764d51e58c404de87d30185a4835945a2caa3591bac3b146ad881748ffd2c')
  o=json.loads(data);self.assertEqual(o['case_count'],84)
  for row in o['cases']:
   r=subprocess.run([str(self.exe),'oracle',str(row['logical_register']),str(row['count']),str(int(row['write']))],capture_output=True,text=True,check=True);self.assertEqual(json.loads(r.stdout),row)
if __name__=='__main__':unittest.main(verbosity=2)
