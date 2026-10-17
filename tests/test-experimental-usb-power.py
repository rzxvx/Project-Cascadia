"""Private source-only staged power actual C + pinned original power oracle."""
import ctypes,json,subprocess,tempfile,unittest
from pathlib import Path
HERE=Path(__file__).resolve().parents[1]/'experiments/p105-usb-power'
class Power(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.tmp=tempfile.TemporaryDirectory();so=Path(cls.tmp.name)/'power.so'
  subprocess.run(['cc','-shared','-fPIC','-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(HERE/'test-include'),str(HERE/'cascadia-phy-power.c'),str(HERE/'power-fixture.c'),'-o',str(so)],check=True)
  cls.lib=ctypes.CDLL(str(so));cls.lib.start.argtypes=[ctypes.c_int,ctypes.c_uint,ctypes.c_ulong];cls.lib.transition.argtypes=[ctypes.c_uint64,ctypes.c_uint];cls.lib.regvalue.restype=ctypes.c_uint
  cls.oracle=json.loads((HERE/'oracle-staged-power.json').read_text())
  assert cls.oracle['source']['kernelcache_sha256']=='db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c'
 @classmethod
 def tearDownClass(cls):cls.tmp.cleanup()
 def start(self,level=0,hz=24000000,permit=1):self.assertEqual(self.lib.start(permit,level,hz),0)
 def calls(self):
  events=[]
  for i in range(self.lib.inspect(0,0)):
   kind,off,val=self.lib.inspect(1,i),self.lib.inspect(2,i),self.lib.inspect(3,i)&0xffffffff
   events.append(['phy',off,val]if kind==0 else ['complex',val]if kind==1 else ['delay',off]if kind==2 else ['clock_membership',off])
  return events
 def test_all_twenty_five_level_transitions_against_original(self):
  for initial in range(5):
   for target in range(5):
    self.start(initial);rows=[x['result']for x in self.oracle['level_scenarios']if x['initial_level']==initial and x['requests']==[target]]
    self.assertEqual(len(rows),1);actual=rows[0]['states'][0]
    self.assertEqual(self.lib.transition(7,target),0)
    expected=[e for e in actual['events']if e[0]!='powergate_membership']
    self.assertEqual(self.calls(),expected,(initial,target));self.assertEqual(self.lib.inspect(4,0),target)
    for off in(0,4,8,0x40,0x44,0x60):self.assertEqual(self.lib.regvalue(0,off),int(actual['regs'][hex(off)],16))
    self.assertEqual(self.lib.regvalue(1,0),int(actual['complex'],16))
 def test_repeated_complete_cycle_preserves_original_order(self):
  sequence=(1,2,3,4,3,2,1,0,4,4,0)
  rows=[x['result']for x in self.oracle['cycle_scenarios']if x['requests']==list(sequence)]
  self.assertEqual(len(rows),1);original=rows[0]['states'];self.start()
  for target,state in zip(sequence,original):
   self.assertEqual(self.lib.transition(7,target),0)
   self.assertEqual(self.calls(),[e for e in state['events']if e[0]!='powergate_membership'])
 def test_exact_rate_input_codes_from_real_clk_api_call(self):
  for hz,value in((12000000,0x10),(24000000,0x11),(48000000,0x12)):
   self.start(hz=hz);self.assertEqual(self.lib.transition(7,2),0);self.assertEqual(self.lib.regvalue(0,4),value);self.assertGreater(self.lib.inspect(10,0),0)
 def test_bad_rate_refused_before_any_mmio_or_membership(self):
  for hz in(0,12000001,24000001,96000000,0xffffffff):
   self.start(hz=hz);self.assertEqual(self.lib.transition(7,2),-22);self.assertEqual(self.lib.inspect(7,0),0);self.assertEqual(self.lib.inspect(4,0),0)
 def test_default_denial_and_every_required_binding_lease(self):
  self.start(permit=0);self.assertEqual(self.lib.transition(7,2),-13);self.assertEqual(self.lib.inspect(7,0),0);self.assertEqual(self.lib.inspect(10,0),0)
  for field in range(5,11):
   self.start();self.lib.inject(field,0);self.assertEqual(self.lib.transition(7,2),-95);self.assertEqual(self.lib.inspect(7,0),0);self.assertEqual(self.lib.inspect(10,0),0)
 def test_missing_actual_epoch_or_clock_membership_provider(self):
  for field in(11,12):
   self.start();self.lib.inject(field,0);self.assertEqual(self.lib.transition(7,2),-19);self.assertEqual(self.lib.inspect(7,0),0)
 def test_invalid_levels_and_epoch_before_io(self):
  self.start();self.assertEqual(self.lib.transition(7,5),-22);self.assertEqual(self.lib.inspect(7,0),0)
  self.start();self.assertEqual(self.lib.transition(8,2),-116);self.assertEqual(self.lib.inspect(7,0),0);self.assertEqual(self.lib.inspect(5,0),0)
 def test_adopted_active_level_requires_explicit_matching_frequency(self):
  self.start(2);self.lib.inject(13,0);self.assertEqual(self.lib.transition(7,3),-95);self.assertEqual(self.lib.inspect(7,0),0)
  self.start(2);self.lib.inject(13,48000000);self.assertEqual(self.lib.transition(7,3),-116);self.assertEqual(self.lib.inspect(5,0),1);self.assertEqual(self.lib.inspect(7,0),0)
 def test_every_io_and_membership_failure_is_latched_no_false_target(self):
  for initial,target in((0,4),(4,0)):
   self.start(initial);self.assertEqual(self.lib.transition(7,target),0);count=self.lib.inspect(7,0)
   for n in range(1,count+1):
    self.start(initial);self.lib.inject(0,n)
    self.assertEqual(self.lib.transition(7,target),-5,(initial,target,n))
    self.assertEqual(self.lib.inspect(5,0),1);self.assertNotEqual(self.lib.inspect(4,0),target)
    before=self.lib.inspect(7,0);self.assertEqual(self.lib.transition(7,target),-5);self.assertEqual(self.lib.inspect(7,0),before)
 def test_each_io_epoch_change_refused_after_provider_and_before_commit(self):
  for initial,target in((0,4),(4,0)):
   self.start(initial);self.assertEqual(self.lib.transition(7,target),0);count=self.lib.inspect(7,0)
   for n in range(1,count+1):
    self.start(initial);self.lib.inject(1,n);self.assertEqual(self.lib.transition(7,target),-116);self.assertEqual(self.lib.inspect(5,0),1);self.assertNotEqual(self.lib.inspect(4,0),target)
 def test_every_epoch_check_and_final_commit_failure_is_not_success(self):
  for initial,target in((0,4),(4,0),(3,4),(4,3)):
   self.start(initial);self.assertEqual(self.lib.transition(7,target),0);count=self.lib.inspect(8,0)
   for n in range(1,count+1):
    self.start(initial);self.lib.inject(2,n);self.assertEqual(self.lib.transition(7,target),-116)
    # A final guard can fail after earlier stage metadata completed; fault
    # marks that disposition, rather than reporting success based on level.
    if self.lib.inspect(9,0) or initial:self.assertEqual(self.lib.inspect(5,0),1)
 def test_real_rate_changes_after_stage_are_latched_even_without_own_mmio(self):
  for initial,target in((0,4),(4,0),(3,4),(4,3)):
   self.start(initial);self.assertEqual(self.lib.transition(7,target),0);count=self.lib.inspect(10,0)
   for n in range(2,count+1):
    self.start(initial);self.lib.inject(3,n);self.lib.inject(4,48000000)
    self.assertEqual(self.lib.transition(7,target),-116);self.assertEqual(self.lib.inspect(5,0),1);self.assertNotEqual(self.lib.inspect(4,0),target)
 def test_active_invalid_rate_or_epoch_cannot_reuse_adoption(self):
  self.start(2,hz=96000000);self.assertEqual(self.lib.transition(7,3),-22);self.assertEqual(self.lib.inspect(5,0),1)
  self.start(2);self.assertEqual(self.lib.transition(8,3),-116);self.assertEqual(self.lib.inspect(5,0),1)
 def test_unrelated_high_bits_rmw_preserved_and_cold_fullwrites_explicit(self):
  self.start(1);self.lib.seed(0,0,0xa500001f);self.lib.seed(0,8,0x8000003f);self.lib.seed(1,0,0x800001e4)
  self.assertEqual(self.lib.transition(7,2),0);self.assertEqual(self.lib.regvalue(0,0),0xa500001e);self.assertEqual(self.lib.regvalue(0,8),0x8000003e);self.assertEqual(self.lib.regvalue(1,0),0x800001e4)
  self.start();self.lib.seed(0,0,0xa500001f);self.assertEqual(self.lib.transition(7,1),0);self.assertEqual(self.lib.regvalue(0,0),0xf)
 def test_reinit_cannot_erase_active_or_fault_state(self):
  self.start();self.assertEqual(self.lib.transition(7,2),0);self.assertEqual(self.lib.reinit(),-114);self.assertEqual(self.lib.inspect(4,0),2)
  self.lib.inject(0,self.lib.inspect(7,0)+1);self.assertEqual(self.lib.transition(7,3),-5);self.assertEqual(self.lib.reinit(),-114);self.assertEqual(self.lib.inspect(5,0),1)
 def test_error_resources_refused_before_init(self):
  for which in range(4):self.assertEqual(self.lib.bad_init(which),-22)
if __name__=='__main__':unittest.main(verbosity=2)
