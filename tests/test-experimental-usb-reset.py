"""Private candidate C + fake I/O compared to pinned original reset instructions."""
import ctypes,json,subprocess,tempfile,unittest
from pathlib import Path
HERE=Path(__file__).resolve().parents[1]/'experiments/p105-usb-reset'
class Reset(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.tmp=tempfile.TemporaryDirectory();so=Path(cls.tmp.name)/'reset.so'
  subprocess.run(['cc','-shared','-fPIC','-fsanitize=undefined','-fno-sanitize-recover=all','-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(HERE/'test-include'),str(HERE/'cascadia-reset-client.c'),str(HERE/'reset-fixture.c'),'-o',str(so)],check=True)
  cls.lib=ctypes.CDLL(str(so));cls.lib.update.argtypes=[ctypes.c_uint64,ctypes.c_uint,ctypes.c_uint,ctypes.c_int];cls.lib.value.restype=ctypes.c_uint
  cls.oracle=json.loads((HERE/'oracle-reset-clients.json').read_text())
  assert cls.oracle['source']['kernelcache_sha256']=='db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c'
 @classmethod
 def tearDownClass(cls):cls.tmp.cleanup()
 def start(self,permit=1,value=0x3f):self.assertEqual(self.lib.start(permit,value),0)
 def effects(self):
  r=[]
  for i in range(self.lib.inspect(0,0)):
   t,a,b=self.lib.inspect(1,i),self.lib.inspect(2,i),self.lib.inspect(3,i)&0xffffffff
   r.append(['phy',a,b]if t==0 else ['delay',a]if t==1 else ['temporary_power_mask',a])
  return r
 def test_three_standard_kind_groups_against_original(self):
  scenarios=[[('add',1,k),('remove',1,k)]for k in(0,1,5)]+[[('add',1,0),('add',2,0),('remove',1,0),('remove',2,0)],[('add',1,0),('add',2,1),('add',3,5),('remove',1,0),('remove',2,1),('remove',3,5)]]
  for requests in scenarios:
   self.start();rows=[x['result']for x in self.oracle['standard_scenarios']if x['requests']==[list(y)for y in requests]]
   self.assertEqual(len(rows),1);states=rows[0]['states']
   for (op,owner,kind),state in zip(requests,states):
    self.assertEqual(self.lib.update(7,owner,kind,int(op=='add')),0);self.assertEqual(self.effects(),state['events']);self.assertEqual(self.lib.value(),state['reset'])
 def test_duplicate_add_and_unowned_remove_skip_temporary_power(self):
  self.start();self.assertEqual(self.lib.update(7,1,0,1),0)
  for owner,kind,add in((1,0,1),(2,0,0)):
   before=self.lib.inspect(7,0);self.assertEqual(self.lib.update(7,owner,kind,add),0);self.assertEqual(self.effects(),[]);self.assertEqual(self.lib.inspect(7,0),before)
  self.assertEqual(self.lib.update(7,1,0,0),0)
  before=self.lib.inspect(7,0);self.assertEqual(self.lib.update(7,1,0,0),0);self.assertEqual(self.lib.inspect(7,0),before)
 def test_owner_multiple_standard_kinds_count_separately(self):
  self.start();self.assertEqual(self.lib.update(7,1,0,1),0);self.assertEqual(self.lib.update(7,1,1,1),0);self.assertEqual(self.lib.inspect(4,1),3)
  self.assertEqual(self.lib.update(7,1,0,0),0);self.assertEqual(self.lib.value(),0x2f);self.assertEqual(self.lib.inspect(4,1),2)
  self.assertEqual(self.lib.update(7,1,1,0),0);self.assertEqual(self.lib.value(),0x3f)
 def test_default_flags_and_missing_providers_do_not_touch(self):
  self.start(0);self.assertEqual(self.lib.update(7,1,0,1),-13);self.assertEqual(self.lib.inspect(7,0),0)
  for field in range(5,11):
   self.start();self.lib.inject(field,0);self.assertEqual(self.lib.update(7,1,0,1),-19 if field>=9 else -95);self.assertEqual(self.lib.inspect(7,0),0)
 def test_no_nonstandard_hsic_reset_writes_or_invalid_owner(self):
  for kind in(2,3,4,6):
   self.start();self.assertEqual(self.lib.update(7,1,kind,1),-95);self.assertEqual(self.lib.inspect(7,0),0)
  self.start();self.assertEqual(self.lib.update(7,16,0,1),-22)
 def test_every_io_or_temporary_power_callback_failure_latches(self):
  for add in(1,0):
   self.start()
   if not add:self.assertEqual(self.lib.update(7,1,0,1),0)
   begin=self.lib.inspect(7,0);self.assertEqual(self.lib.update(7,1,0,add),0);count=self.lib.inspect(7,0)-begin
   for offset in range(1,count+1):
    self.start()
    if not add:self.assertEqual(self.lib.update(7,1,0,1),0)
    self.lib.inject(0,self.lib.inspect(7,0)+offset)
    old=self.lib.inspect(4,1);self.assertEqual(self.lib.update(7,1,0,add),-5);self.assertEqual(self.lib.inspect(5,0),1);self.assertEqual(self.lib.inspect(4,1),old)
    before=self.lib.inspect(7,0);self.assertEqual(self.lib.update(7,1,0,add),-5);self.assertEqual(self.lib.inspect(7,0),before)
 def test_late_applied_lease_errors_keep_unknown_ownership(self):
  for failed in(1,4):
   self.start();self.lib.inject(0,failed);self.lib.inject(3,1)
   self.assertEqual(self.lib.update(7,1,0,1),-5);self.assertEqual(self.lib.inspect(9,0),1);self.assertEqual(self.lib.inspect(10,0),0);self.assertEqual(self.lib.inspect(4,1),0)
   self.assertEqual(self.lib.inspect(11,0),16 if failed==1 else 0)
 def test_positive_temporary_provider_return_is_not_success(self):
  self.start();self.lib.inject(4,1);self.assertEqual(self.lib.update(7,1,0,1),-5);self.assertEqual(self.lib.inspect(5,0),1);self.assertEqual(self.lib.inspect(4,1),0)
 def test_each_io_epoch_and_final_check_refuse_commit(self):
  self.start();self.assertEqual(self.lib.update(7,1,0,1),0);iocount=self.lib.inspect(7,0);vcount=self.lib.inspect(8,0)
  for n in range(1,iocount+1):
   self.start();self.lib.inject(1,n);self.assertEqual(self.lib.update(7,1,0,1),-116);self.assertEqual(self.lib.inspect(4,1),0);self.assertEqual(self.lib.inspect(5,0),1)
  for n in range(1,vcount+1):
   self.start();self.lib.inject(2,n);self.assertEqual(self.lib.update(7,1,0,1),-116);self.assertEqual(self.lib.inspect(4,1),0)
 def test_new_valid_connector_epoch_cannot_reuse_old_owner(self):
  self.start();self.assertEqual(self.lib.update(7,1,0,1),0);self.lib.inject(11,1)
  before=self.lib.inspect(7,0);self.assertEqual(self.lib.update(8,1,0,0),-116);self.assertEqual(self.lib.inspect(7,0),before);self.assertEqual(self.lib.inspect(5,0),1);self.assertEqual(self.lib.inspect(4,1),1)
 def test_noop_final_epoch_error_latches_existing_owner(self):
  self.start();self.assertEqual(self.lib.update(7,1,0,1),0)
  self.lib.inject(2,self.lib.inspect(8,0)+2);self.assertEqual(self.lib.update(7,1,0,1),-116);self.assertEqual(self.lib.inspect(5,0),1);self.assertEqual(self.lib.inspect(4,1),1)
 def test_reset_preserves_high_and_other_domains(self):
  self.start(value=0x90000033);self.assertEqual(self.lib.update(7,1,0,1),0);self.assertEqual(self.lib.value(),0x90000023);self.assertEqual(self.lib.update(7,1,0,0),0);self.assertEqual(self.lib.value(),0x90000033)
 def test_reinit_preserves_member_and_fault(self):
  self.start();self.assertEqual(self.lib.update(7,1,0,1),0);self.assertEqual(self.lib.reinit(),-114);self.assertEqual(self.lib.inspect(4,1),1)
  self.lib.inject(0,self.lib.inspect(7,0)+1);self.assertEqual(self.lib.update(7,1,0,0),-5);self.assertEqual(self.lib.reinit(),-114);self.assertEqual(self.lib.inspect(5,0),1)
if __name__=='__main__':unittest.main(verbosity=2)
