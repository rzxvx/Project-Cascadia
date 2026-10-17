"""Private candidate actual C + fake Regmap and original 12H321 instruction oracle."""
import ctypes,importlib.util,json,subprocess,tempfile,unittest
from pathlib import Path
HERE=Path(__file__).resolve().parents[1]/'experiments/p105-usb-registers'
class Registers(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.tmp=tempfile.TemporaryDirectory();so=Path(cls.tmp.name)/'candidate.so'
  subprocess.run(['cc','-shared','-fPIC','-std=gnu11','-Wall','-Wextra','-Werror','-fsanitize=undefined','-fno-sanitize-recover=all','-I'+str(HERE/'test-include'),str(HERE/'cascadia-usb-registers.c'),str(HERE/'backend-fixture.c'),'-o',str(so)],check=True)
  cls.lib=ctypes.CDLL(str(so));cls.lib.update.argtypes=[ctypes.c_uint,ctypes.c_uint64,ctypes.c_uint];cls.lib.tune.argtypes=[ctypes.c_uint,ctypes.c_uint64,ctypes.c_int,ctypes.c_uint,ctypes.c_int,ctypes.c_int,ctypes.c_int];cls.lib.regvalue.restype=ctypes.c_uint
  cls.oracle=json.loads((HERE/'oracle-register-backend.json').read_text())
  assert cls.oracle['source']['kernelcache_sha256']=='db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c'
 @classmethod
 def tearDownClass(cls):cls.tmp.cleanup()
 def start(self,permission=1,complete=1,release=1):self.assertEqual(self.lib.start(permission,complete,release),0)
 def events(self):
  return [[self.lib.inspect(1,i),self.lib.inspect(2,i),self.lib.inspect(3,i)&0xffffffff]for i in range(self.lib.inspect(0,0))]
 def test_clock_four_original_owner_scenarios(self):
  scenarios=[[(1,0x30012),(2,0x30002),(2,0),(1,0)],[(1,0x30011),(2,0x30012),(3,0x30002),(3,0),(2,0),(1,0)],[(1,0x30002),(1,0x30002),(1,0x10002),(1,0)],[(1,0x30006),(2,0x30002),(1,0),(2,0)],[(1,0x10),(2,0x10002),(1,0),(2,0)]]
  for requests in scenarios:
   rows=[x['results']for x in self.oracle['clock_scenarios']if x['requests']==[list(y)for y in requests]]
   self.assertEqual(len(rows),1);original=rows[0]
   self.start()
   for (owner,mask),state in zip(requests,original):
    with self.subTest(owner=owner,mask=hex(mask),scenario=requests):
     self.assertEqual(self.lib.update(owner,7,mask),0)
     result=[]
     for region,off,value in self.events():
      if region==0:result.append(['complex',value])
      elif region==1:result.append(['gate',[0x1084,0x1088,0x1090,0x1094].index(off),int(bool(value&15))])
      else:self.fail('unexpected nonclock event')
     expected=[e for e in state['events']if e[0]!='gate' or e[1]!=4]
     self.assertEqual(result,expected)
     self.assertEqual(self.lib.regvalue(0,0),int(state['control'],16))
 def test_default_off_and_incomplete_ownership_never_io(self):
  for permission,complete in((0,0),(0,1),(1,0)):
   self.start(permission,complete,0)
   self.assertEqual(self.lib.update(1,7,0x30002),-13)
   self.assertEqual(self.lib.tune(1,7,1,2,1,1,1),-13)
   self.assertEqual(self.lib.inspect(6,0),0)
 def test_unverified_final_release_stops_before_io_but_foreign_owner_holds(self):
  self.start(release=0)
  self.assertEqual(self.lib.update(1,7,0x30012),0)
  self.assertEqual(self.lib.update(2,7,0x30002),0)
  before=self.lib.inspect(6,0)
  self.assertEqual(self.lib.update(2,7,0),0)
  self.assertEqual(self.lib.inspect(6,0),before)
  self.assertEqual(self.lib.update(1,7,0),-95)
  self.assertEqual(self.lib.inspect(6,0),before)
  self.assertEqual(self.lib.inspect(4,1),0x30012)
 def test_refused_invalid_owner_mask_epoch_and_missing_validator(self):
  for owner,epoch,mask,expected in((16,7,0x30002,-22),(1,8,0x30002,-116),(1,7,0x40002,-22),(1,7,0x20,-22),(1,7,0x80000000,-22)):
   self.start();self.assertEqual(self.lib.update(owner,epoch,mask),expected);self.assertEqual(self.lib.inspect(6,0),0)
  self.start();self.lib.inject(4,0);self.assertEqual(self.lib.update(1,7,0x30002),-19);self.assertEqual(self.lib.inspect(6,0),0)
 def test_tune_matches_original_four_host_hsic_combinations(self):
  for host,hsic in((1,1),(0,1),(1,0),(0,0)):
   self.start();rows=[x['result']for x in self.oracle['host_scenarios']if x['host']==host and x['level']==2 and x['hsic']==hsic]
   self.assertEqual(len(rows),1);original=rows[0]
   self.assertEqual(self.lib.tune(1,7,host,2,hsic,1,1),0)
   expected=[e for e in original['events']if e[0]!='temporary_power_mask']
   result=[['phy',a,b]if kind==2 else ['delay',a]for kind,a,b in self.events()]
   self.assertEqual(result,expected)
   self.assertEqual(self.lib.regvalue(2,8),0x30)
   self.assertEqual(self.lib.regvalue(2,0x64),0xa400|(2 if host and not hsic else 0))
 def test_tune_requires_real_lease_quiescence_and_level_two_or_higher(self):
  for level,q,lease in((0,1,1),(1,1,1),(5,1,1),(2,0,1),(2,1,0)):
   self.start();self.assertEqual(self.lib.tune(1,7,1,level,1,q,lease),-95);self.assertEqual(self.lib.inspect(6,0),0)
 def test_every_clock_io_failure_is_checked_no_membership_commit(self):
  self.start();self.assertEqual(self.lib.update(1,7,0x30002),0);count=self.lib.inspect(6,0)
  for n in range(1,count+1):
   self.start();self.lib.inject(0,n)
   self.assertEqual(self.lib.update(1,7,0x30002),-5)
   self.assertEqual(self.lib.inspect(4,1),0)
   self.assertEqual(self.lib.inspect(10,0),0x30002)
   if self.lib.inspect(11,0):
    self.assertEqual(self.lib.inspect(5,0),1)
    io=self.lib.inspect(6,0);self.assertEqual(self.lib.update(1,7,0),-5);self.assertEqual(self.lib.inspect(6,0),io)
 def test_every_tune_io_failure_stops_and_latches_if_touched(self):
  self.start();self.assertEqual(self.lib.tune(1,7,1,2,0,1,1),0);count=self.lib.inspect(6,0)
  for n in range(1,count+1):
   self.start();self.lib.inject(0,n)
   self.assertEqual(self.lib.tune(1,7,1,2,0,1,1),-5)
   self.assertEqual(self.lib.inspect(5,0),1)
   io=self.lib.inspect(6,0);self.assertEqual(self.lib.tune(1,7,0,2,1,1,1),-5);self.assertEqual(self.lib.inspect(6,0),io)
 def test_epoch_change_after_each_clock_io_and_before_final_commit(self):
  self.start();self.assertEqual(self.lib.update(1,7,0x30002),0)
  count=self.lib.inspect(6,0);vcount=self.lib.inspect(7,0)
  for n in range(1,count+1):
   self.start();self.lib.inject(1,n)
   self.assertEqual(self.lib.update(1,7,0x30002),-116);self.assertEqual(self.lib.inspect(4,1),0)
   if self.lib.inspect(11,0):self.assertEqual(self.lib.inspect(5,0),1)
  self.start();self.lib.inject(2,vcount);self.assertEqual(self.lib.update(1,7,0x30002),-116);self.assertEqual(self.lib.inspect(4,1),0);self.assertEqual(self.lib.inspect(5,0),1)
 def test_epoch_change_after_each_tune_io_and_final_commit(self):
  self.start();self.assertEqual(self.lib.tune(1,7,1,2,0,1,1),0)
  count=self.lib.inspect(6,0);vcount=self.lib.inspect(7,0)
  for n in range(1,count+1):
   self.start();self.lib.inject(1,n);self.assertEqual(self.lib.tune(1,7,1,2,0,1,1),-116);self.assertEqual(self.lib.inspect(5,0),1)
  self.start();self.lib.inject(2,vcount);self.assertEqual(self.lib.tune(1,7,1,2,0,1,1),-116);self.assertEqual(self.lib.inspect(5,0),1)
 def test_gate_ack_timeout_is_bounded_and_retains_fault(self):
  self.start();self.lib.inject(3,1)
  self.assertEqual(self.lib.update(1,7,0x30002),-110)
  self.assertEqual(self.lib.inspect(5,0),1);self.assertEqual(self.lib.inspect(4,1),0)
  self.assertLess(self.lib.inspect(8,0),1100)
 def test_reinit_cannot_lose_owned_masks_or_fault(self):
  self.start();self.assertEqual(self.lib.update(1,7,0x30012),0)
  self.assertEqual(self.lib.reinit(),-114);self.assertEqual(self.lib.inspect(4,1),0x30012)
  self.lib.inject(0,self.lib.inspect(6,0)+1)
  self.assertEqual(self.lib.tune(1,7,1,2,1,1,1),-5)
  self.assertEqual(self.lib.reinit(),-114);self.assertEqual(self.lib.inspect(5,0),1)
 def test_unrelated_high_register_bits_preserved(self):
  self.start();self.lib.seed(0,0,0x81202460);self.lib.seed(2,8,0x90001030);self.lib.seed(2,0x64,0x8000a400)
  self.assertEqual(self.lib.update(1,7,0x30002),0);self.assertEqual(self.lib.regvalue(0,0),0x81202400)
  self.assertEqual(self.lib.tune(1,7,1,4,1,1,1),0)
  self.assertEqual(self.lib.regvalue(2,8),0x90001030);self.assertEqual(self.lib.regvalue(2,0x64),0x8000a400)
 def test_invalid_regmap_handles_fail_before_initializing(self):
  self.start()
  for index in range(3):
   for errptr in (0,1):
    self.assertEqual(self.lib.bad_init(index,errptr),-22)
  self.assertEqual(self.lib.inspect(6,0),0)
 def test_retained_ownership_validation_failure_cannot_recover_silently(self):
  for tune in (False,True):
   for failure in (-116,-19,1):
    self.start()
    self.assertEqual(self.lib.update(1,7,0x30002),0)
    io=self.lib.inspect(6,0)
    self.lib.inject(5,failure)
    expected=-5 if failure>0 else failure
    action=lambda: self.lib.tune(1,7,1,2,1,1,1) if tune else self.lib.update(1,7,0)
    self.assertEqual(action(),expected)
    self.assertEqual(self.lib.inspect(5,0),1)
    self.assertEqual(self.lib.inspect(4,1),0x30002)
    self.assertEqual(self.lib.inspect(6,0),io)
    self.lib.inject(5,0)
    self.assertEqual(action(),expected)
    self.assertEqual(self.lib.inspect(6,0),io)
  self.start();self.lib.inject(5,-116)
  self.assertEqual(self.lib.update(1,7,0x30002),-116)
  self.assertEqual(self.lib.inspect(5,0),0)
  self.lib.inject(5,0)
  self.assertEqual(self.lib.update(1,7,0x30002),0)
 def test_missing_validator_after_retention_latches_without_io(self):
  self.start();self.assertEqual(self.lib.update(1,7,0x30002),0)
  io=self.lib.inspect(6,0);self.lib.inject(4,0)
  self.assertEqual(self.lib.update(1,7,0),-19)
  self.assertEqual(self.lib.inspect(5,0),1)
  self.assertEqual(self.lib.inspect(6,0),io)
if __name__=='__main__':unittest.main(verbosity=2)
