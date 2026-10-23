"""Actual candidate C, fake providers and normalized original-instruction oracle."""
import ctypes,json,subprocess,tempfile,unittest
from pathlib import Path
HERE=Path(__file__).resolve().parents[1]/'experiments/p105-usb-controller'
class Controller(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.tmp=tempfile.TemporaryDirectory();so=Path(cls.tmp.name)/'controller.so'
  subprocess.run(['cc','-shared','-fPIC','-fsanitize=undefined','-fno-sanitize-recover=all','-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(HERE/'test-include'),str(HERE/'cascadia-controller.c'),str(HERE/'controller-fixture.c'),'-o',str(so)],check=True)
  cls.lib=ctypes.CDLL(str(so));cls.lib.start.argtypes=[ctypes.c_int,ctypes.c_uint,ctypes.c_uint];cls.lib.operation.argtypes=[ctypes.c_uint64,ctypes.c_int,ctypes.c_int];cls.lib.regvalue.restype=ctypes.c_uint
  cls.oracle=json.loads((HERE/'oracle-controller.json').read_text())
  assert cls.oracle['source']['kernelcache_sha256']=='db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c'
 @classmethod
 def tearDownClass(cls):cls.tmp.cleanup()
 def start(self,gate=0x1ff,mux=0xa50000e1,permit=1):self.assertEqual(self.lib.start(permit,gate,mux),0)
 def events(self):return [[self.lib.inspect(j,i)&0xffffffff for j in(1,2,3)]for i in range(self.lib.inspect(0,0))]
 def test_mux_exact_original_leaf_all_high_bits_preserved(self):
  for case in self.oracle['mux_cases']:
   self.start(mux=case['saved']);self.assertEqual(self.lib.operation(7,0,case['host']),0)
   self.assertEqual(self.events(),[[0,0,value]for value in case['writes']]);self.assertEqual(self.lib.regvalue(0),case['final']);self.assertEqual(self.lib.inspect(8,0),1)
 def test_ehci_reset_exact_original_timing_and_restoration(self):
  for case in self.oracle['reset_cases']:
   if case['index']!=0x22 or not case['argument'] or case['initial']&0x80000000 or not case['init_return'] or case['initial']&0xff!=0xff:continue
   self.start(gate=case['initial']);self.assertEqual(self.lib.operation(7,1,0),0)
   expected=[]
   for event in case['events']:
    if event[0]=='fake_pmgr_write':expected.append([1,int(event[1],16),event[2]])
    if event[0]=='fake_IODelay_us':expected.append([2,event[1],0])
   self.assertEqual(self.events(),expected);self.assertEqual(self.lib.regvalue(1),case['final'])
 def test_default_denial_every_owner_quiesce_power_certificate(self):
  for op in(0,1):
   self.start(permit=0);self.assertEqual(self.lib.operation(7,op,1),-13);self.assertEqual(self.lib.inspect(4,0),0)
   for flag in range(4,12):
    self.start();self.lib.inject(flag,0);self.assertEqual(self.lib.operation(7,op,1),-95);self.assertEqual(self.lib.inspect(4,0),0);self.assertFalse(self.events())
 def test_gate_target_ack_and_reset_assertion_are_checked_without_write(self):
  for gate,code in((0,-112),(0xf,-112),(0xf0,-112),(0x15f,-112),(0x800000ff,-16)):
   for op in(0,1):
    self.start(gate=gate);self.assertEqual(self.lib.operation(7,op,1),code);self.assertFalse(self.events());self.assertEqual(self.lib.regvalue(1),gate);self.assertEqual(self.lib.inspect(6,0),0)
 def test_missing_epoch_provider_and_mismatched_retained_lease(self):
  self.start();self.lib.inject(12,0);self.assertEqual(self.lib.operation(7,1,0),-19);self.assertEqual(self.lib.inspect(4,0),0)
  self.start();self.lib.inject(13,8);self.assertEqual(self.lib.operation(7,1,0),-116);self.assertEqual(self.lib.inspect(4,0),0)
  self.start();self.assertEqual(self.lib.operation(7,1,0),0);self.assertEqual(self.lib.operation(8,1,0),-116);self.assertEqual(self.lib.inspect(8,0),1)
 def test_every_regmap_early_and_late_failure_is_latched_without_success(self):
  for op in(0,1):
   self.start();self.assertEqual(self.lib.operation(7,op,1),0);count=self.lib.inspect(4,0)
   for kind in(0,1):
    for n in range(1,count+1):
     self.start();self.lib.inject(kind,n);self.assertEqual(self.lib.operation(7,op,1),-5,(op,kind,n));self.assertEqual(self.lib.inspect(6,0),1);self.assertEqual(self.lib.inspect(8,0),0)
     before=self.lib.inspect(4,0);self.assertEqual(self.lib.operation(7,op,1),-5);self.assertEqual(self.lib.inspect(4,0),before)
 def test_every_io_epoch_change_is_latched_including_final_write(self):
  for op in(0,1):
   self.start();self.assertEqual(self.lib.operation(7,op,1),0);count=self.lib.inspect(4,0)
   for n in range(1,count+1):
    self.start();self.lib.inject(2,n);self.assertEqual(self.lib.operation(7,op,1),-116);self.assertEqual(self.lib.inspect(6,0),1);self.assertEqual(self.lib.inspect(8,0),0)
 def test_every_validation_change_before_delay_and_success_commit(self):
  for op in(0,1):
   self.start();self.assertEqual(self.lib.operation(7,op,1),0);count=self.lib.inspect(5,0)
   for n in range(1,count+1):
    self.start();self.lib.inject(3,n);self.assertEqual(self.lib.operation(7,op,1),-116);self.assertEqual(self.lib.inspect(8,0),0)
    self.assertEqual(self.lib.inspect(6,0),int(n!=1))
 def test_reset_failure_leaves_latched_assertion_without_blind_restore(self):
  self.start();self.lib.inject(0,3);self.assertEqual(self.lib.operation(7,1,0),-5);self.assertEqual(self.lib.regvalue(1),0x800001ff);self.assertEqual(self.events(),[[1,0x1088,0x800001ff],[2,1,0]])
 def test_mux_failure_retains_partial_clear_and_refuses_other_operation(self):
  self.start();self.lib.inject(0,5);self.assertEqual(self.lib.operation(7,0,1),-5);self.assertEqual(self.lib.regvalue(0),0xa50000e0)
  before=self.lib.inspect(4,0);self.assertEqual(self.lib.operation(7,1,0),-5);self.assertEqual(self.lib.inspect(4,0),before)
 def test_completed_context_actual_epoch_failure_is_latched_before_new_io(self):
  for op in(0,1):
   self.start();self.assertEqual(self.lib.operation(7,op,1),0);before=self.lib.inspect(4,0)
   self.lib.inject(14,-116);self.assertEqual(self.lib.operation(7,op,1),-116);self.assertEqual(self.lib.inspect(6,0),1);self.assertEqual(self.lib.inspect(4,0),before)
   self.lib.inject(14,0);self.assertEqual(self.lib.operation(7,op,1),-116);self.assertEqual(self.lib.inspect(4,0),before);self.assertEqual(self.lib.inspect(8,0),1)
 def test_reinit_cannot_erase_success_touched_or_fault_state(self):
  self.start();self.assertEqual(self.lib.operation(7,1,0),0);self.assertEqual(self.lib.reinit(),-114);self.assertEqual(self.lib.inspect(8,0),1)
  self.lib.inject(0,self.lib.inspect(4,0)+1);self.assertEqual(self.lib.operation(7,0,1),-5);self.assertEqual(self.lib.reinit(),-114);self.assertEqual(self.lib.inspect(6,0),1)
 def test_bad_resource_before_initialization_and_positive_callback_error(self):
  for which in range(4):self.assertEqual(self.lib.bad_init(which),-22)
  self.start();self.lib.inject(14,1);self.assertEqual(self.lib.operation(7,1,0),-5);self.assertEqual(self.lib.inspect(4,0),0)
if __name__=='__main__':unittest.main(verbosity=2)
