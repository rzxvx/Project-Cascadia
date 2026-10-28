"""Actual checked native C against pinned normalized original-instruction traces."""
import ctypes, hashlib, json, subprocess, tempfile, unittest
from pathlib import Path
HERE=Path(__file__).resolve().parents[1]/'experiments/p105-native-clock'
class Native(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.tmp=tempfile.TemporaryDirectory();so=Path(cls.tmp.name)/'native.so'
  cls.cc=['cc','-std=gnu11','-Wall','-Wextra','-Werror','-I'+str(HERE/'include')]
  cls.src=[str(HERE/'source-wrapper.c'),str(HERE/'native-fixture.c')]
  subprocess.run(cls.cc+['-shared','-fPIC']+cls.src+['-o',str(so)],check=True)
  cls.lib=ctypes.CDLL(str(so));cls.lib.start.argtypes=[ctypes.c_int,ctypes.c_uint];cls.lib.request.argtypes=[ctypes.c_uint64,ctypes.c_uint,ctypes.c_uint,ctypes.c_int];cls.lib.zero_policy.argtypes=[ctypes.c_uint64];cls.lib.inspect.restype=ctypes.c_uint;cls.lib.get_reg.restype=ctypes.c_uint;cls.lib.baseline.argtypes=[ctypes.c_int,ctypes.c_uint]
  assert hashlib.sha256((HERE/'oracle-native-clock.json').read_bytes()).hexdigest()=='9ca8bc80f28aeb5e0ec5005d527ddde05afc95d7cc052877879ef1ce238e0a9c';cls.oracle=json.loads((HERE/'oracle-native-clock.json').read_text());assert cls.oracle['source']['kernelcache_sha256']=='db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c';assert cls.oracle['source']['native_min']==56 and cls.oracle['source']['native_max']==128
 @classmethod
 def tearDownClass(cls):cls.tmp.cleanup()
 def start(self,seed=0x24600000,permit=1,adopt=True):
  self.assertEqual(self.lib.start(permit,seed),0)
  if adopt:self.assertEqual(self.lib.adopt(),0)
 def events(self):return [[self.lib.inspect(1,i),self.lib.inspect(2,i)]for i in range(self.lib.inspect(0,0))]
 def req(self,root,on,owner=0):return self.lib.request(7,root,owner,on)
 def refs(self):return [self.lib.inspect(10,i)for i in range(6)]
 def test_actual_C_fixed_metadata_equals_full_pinned_ADT_closure(self):
  rows=self.oracle['source']['metadata'];self.assertEqual([r['id']for r in rows],[2,3,4,5,40,42,87,89])
  for n,row in enumerate(rows):
   w=row['words'];parents=[(w[2]>>(8*i))&255 for i in range(4)]
   self.assertEqual([self.lib.source_metadata(n,f)for f in range(7)],[row['id'],w[4],w[3],*parents])
   self.assertEqual(w[1],0);self.assertEqual(w[5:], [0,0])
  self.assertEqual(self.lib.source_metadata(8,0)&0xffffffff,0xffffffff)
 def test_native_positive_source19_requests_and_narrow_scope(self):
  cases=self.oracle['positive'];self.assertEqual(sum(len(c['states'])for c in cases),19)
  # Four independent normal native scenarios; duplicate/unowned calls are denied
  # instead of Apple's idempotent0, while their no-write semantics still match.
  for case in cases[:4]:
   self.start();owned=set()
   for st in case['states']:
    root,on=st['request'];expected=0
    if on and root in owned:expected=-114
    if not on and root not in owned:expected=-2
    self.assertEqual(self.req(root,on),expected)
    self.assertEqual(self.events(),st['writes'])
    if expected==0:
     (owned.add if on else owned.remove)(root)
    self.assertEqual(self.lib.inspect(10,4),st['counts']['40']);self.assertEqual(self.lib.inspect(10,5),st['counts']['42'])
    for i in range(4):self.assertEqual(bool(self.lib.inspect(10,i)),i+2 in st['active_ids'])
  # DWC2/domain and OHCI native positives are evidence, explicitly unsupported.
  for case in cases[4:]:
   self.start()
   for st in case['states']:
    if st['request'][0]in(88,90,91):self.assertEqual(self.req(*st['request']),-95);self.assertFalse(self.events())
 def test_real_shared_foreign40_refs_preserved_as_native_otg_scenario(self):
  st=self.oracle['positive'][4]['states'][0];self.assertEqual(st['request'],[88,1]);self.assertEqual(st['counts']['40'],1)
  self.start(adopt=False)
  for i in range(5):self.lib.baseline(i,1)
  for reg,value in st['writes']:
   # Fake acknowledgement of the original writes; OTGgate not claimed by broker.
   self.lib.set_reg(reg,((value&~0xf0)|((value&15)<<4))if reg>=0x1000 else value)
  self.assertEqual(self.lib.adopt(),0)
  for expected in self.oracle['positive'][4]['states'][1:3]:
   self.assertEqual(self.req(*expected['request']),0);self.assertEqual(self.events(),expected['writes']);self.assertEqual(self.lib.inspect(10,4),expected['counts']['40'])
  self.assertEqual(self.refs(),[1,1,1,1,1,0]);self.assertEqual(self.req(88,0),-95)
 def test_two_own_clients_first_last_and_peer87_aggregation(self):
  self.start();self.assertEqual(self.req(89,1,3),0);self.assertEqual(self.req(89,1,31),0);self.assertFalse(self.events());self.assertEqual(self.refs(),[1,1,1,1,1,0])
  self.assertEqual(self.req(87,1,0),0);self.assertEqual(self.refs(),[1,1,1,1,3,1])
  self.assertEqual(self.req(89,0,3),0);self.assertFalse(self.events());self.assertEqual(self.lib.inspect(11,1),1<<31)
  self.assertEqual(self.req(89,0,31),0);self.assertEqual([e[0]for e in self.events()],[0x1088]);self.assertEqual(self.refs(),[1,1,1,1,2,1])
  self.assertEqual(self.req(87,0),0);self.assertEqual(self.refs(),[0]*6)
 def test_protected_foreign87_and_global_parent_pins_never_released(self):
  self.start(adopt=False)
  for i,v in enumerate([2,2,2,2,2,1]):self.lib.baseline(i,v)
  self.lib.inject(8,1)
  for r in(0x48,0x4c,0x50,0x54,0x9c,0xec):self.lib.set_reg(r,0xa4600000)
  for r in(0x1080,0x1098):self.lib.set_reg(r,0x246000ff)
  self.assertEqual(self.lib.adopt(),0);self.assertEqual(self.req(87,1),0);self.assertFalse(self.events());self.assertEqual(self.req(87,0),0);self.assertFalse(self.events())
  self.assertEqual(self.req(89,1),0);self.assertEqual([e[0]for e in self.events()],[0x1088]);self.assertEqual(self.req(89,0),0);self.assertEqual([e[0]for e in self.events()],[0x1088]);self.assertEqual(self.refs(),[2,2,2,2,2,1]);self.assertEqual(self.lib.inspect(14,0),1)
 def test_source_negative7_requests_checked_ready_not_native_shortcut(self):
  negatives=self.oracle['negative'];self.assertEqual(sum(len(c['states'])for c in negatives),7)
  # clock0/C0 fixtures are synthetic metadata counterproof; fixed source binding
  # offers no runtime row injection or permission to map them into direct clocks.
  for n in(0,1):self.assertFalse([w for w in negatives[n]['states'][0]['writes']if w[0]==0xec])
  self.start(adopt=False);self.lib.zero_policy(1<<34);self.assertEqual(self.lib.adopt(),0)
  for st in negatives[2]['states']:self.assertEqual(self.req(*st['request']),0);self.assertEqual(self.events(),st['writes'])
  self.assertEqual(self.lib.get_reg(0x1088)&255,0)
  self.start(seed=negatives[3]['synthetic_options']['initial_reg_seed']);self.lib.inject(5,1);self.assertEqual(self.req(89,1),-110);self.assertEqual(self.lib.inspect(13,0),3)
  self.start();self.lib.inject(4,0);self.assertEqual(self.req(89,1),-110);self.assertEqual(self.lib.inspect(13,0),3)
  shortcut=negatives[5];self.assertEqual(shortcut['states'][0]['return'],0);self.assertFalse([w for w in shortcut['states'][0]['writes']if w[0]>=0x1000])
  self.start(seed=shortcut['synthetic_options']['initial_reg_seed']);self.lib.inject(4,0);self.assertEqual(self.req(89,1),-110);self.assertEqual(self.lib.inspect(5,0),1);self.assertFalse([w for w in self.events()if w[0]>=0x1000])
 def test_all_adoption_certificates_default_denied_before_any_io(self):
  self.start(permit=0,adopt=False);self.assertEqual(self.lib.adopt(),-13);self.assertEqual(self.lib.inspect(3,0),0)
  self.start(adopt=False);self.assertEqual(self.req(89,1),-95);self.assertEqual(self.lib.inspect(3,0),0)
  for flag in range(6):
   self.start(adopt=False);self.lib.certificate_flag(flag,0);self.assertEqual(self.lib.adopt(),-95);self.assertEqual(self.lib.inspect(3,0),0)
  self.start(adopt=False);self.lib.zero_policy(1<<33);self.assertEqual(self.lib.adopt(),-95);self.assertEqual(self.lib.inspect(3,0),0)
 def test_invalid_baseline_and_stale_or_unready_adoption_no_lease(self):
  for refs,foreign in(([0,0,0,0,1,0],0),([1,1,1,1,1,1],1),([1,1,1,1,2,1],3),([0]*6,4)):
   self.start(adopt=False)
   for i,v in enumerate(refs):self.lib.baseline(i,v)
   self.lib.inject(8,foreign);self.assertEqual(self.lib.adopt(),-22);self.assertEqual(self.lib.inspect(15,0),0);self.assertEqual(self.lib.inspect(3,0),0)
  self.start(adopt=False);self.lib.baseline(0,1);self.assertEqual(self.lib.adopt(),-112);self.assertEqual(self.lib.inspect(15,0),0);self.assertFalse(self.events())
  self.start(adopt=False);self.lib.inject(6,-116);self.assertEqual(self.lib.adopt(),-116);self.assertEqual(self.lib.inspect(15,0),0);self.assertEqual(self.lib.inspect(3,0),0)
 def test_default_off4_bit8_clear_and_other32bits_preserved(self):
  self.start(seed=0x24600100);self.assertEqual(self.req(89,1),0)
  gate=[w for w in self.events()if w[0]==0x1088][0];self.assertEqual(gate,[0x1088,0x2460000f]);self.assertEqual(self.lib.get_reg(0x1088),0x246000ff)
  self.assertEqual(self.req(89,0),0);gate=[w for w in self.events()if w[0]==0x1088][0];self.assertEqual(gate,[0x1088,0x246000f4]);self.assertEqual(self.lib.get_reg(0x1088),0x24600044)
 def test_every_early_late_regmap_failure_retains_pending_no_count_commit(self):
  self.start();self.assertEqual(self.req(89,1),0);total=self.lib.inspect(3,0)
  for kind in(0,1):
   for n in range(1,total+1):
    self.start();self.lib.inject(kind,n);self.assertEqual(self.req(89,1),-5,(kind,n));self.assertEqual(self.lib.inspect(5,0),1);self.assertEqual(self.refs(),[0]*6);self.assertEqual(self.lib.inspect(12,4),1);self.assertEqual(self.lib.inspect(8,0),1);self.assertEqual(self.lib.inspect(7,0),0)
    before=self.lib.inspect(3,0);self.assertEqual(self.req(87,1),-5);self.assertEqual(self.lib.inspect(3,0),before)
 def test_every_release_failure_retains_owned_refs_and_unknown_pending(self):
  self.start();self.assertEqual(self.req(89,1),0);before=self.lib.inspect(3,0);self.assertEqual(self.req(89,0),0);total=self.lib.inspect(3,0)-before
  for n in range(1,total+1):
   self.start();self.assertEqual(self.req(89,1),0);self.lib.inject(1,self.lib.inspect(3,0)+n);self.assertEqual(self.req(89,0),-5);self.assertEqual(self.refs(),[1,1,1,1,1,0]);self.assertEqual(self.lib.inspect(11,1),1);self.assertEqual(self.lib.inspect(8,0),1);self.assertEqual(self.lib.inspect(12,4),0)
 def test_epoch_changes_every_io_and_success_boundary_are_latched(self):
  self.start();self.assertEqual(self.req(89,1),0);total=self.lib.inspect(3,0);checks=self.lib.inspect(4,0)
  for n in range(1,total+1):
   self.start();self.lib.inject(2,n);self.assertEqual(self.req(89,1),-116);self.assertEqual(self.lib.inspect(5,0),1);self.assertEqual(self.refs(),[0]*6)
  for n in range(2,checks+1): # check1 belongs to already completed adoption
   self.start();self.lib.inject(3,n);self.assertEqual(self.req(89,1),-116);self.assertEqual(self.lib.inspect(7,0),0)
  self.start();self.assertEqual(self.req(89,1),0);before=self.lib.inspect(3,0);self.lib.inject(6,-116);self.assertEqual(self.req(87,1),-116);self.assertEqual(self.lib.inspect(5,0),1);self.lib.inject(6,0);self.assertEqual(self.req(87,1),-116);self.assertEqual(self.lib.inspect(3,0),before)
 def test_overflow_invalid_input_resource_lock_and_reinit(self):
  self.start(adopt=False)
  for i in range(5):self.lib.baseline(i,0xffffffff)
  for r in(0x48,0x4c,0x50,0x54,0x9c):self.lib.set_reg(r,0xa4600000)
  self.lib.set_reg(0x1080,0x246000ff);self.assertEqual(self.lib.adopt(),0);before=self.lib.inspect(3,0);self.assertEqual(self.req(89,1),-75);self.assertEqual(self.lib.inspect(3,0),before);self.assertEqual(self.lib.inspect(5,0),0)
  self.start();self.assertEqual(self.req(89,1,32),-22);self.assertEqual(self.lib.request(8,89,0,1),-116);self.assertEqual(self.lib.reinit(),-114)
  for i in range(4):self.assertEqual(self.lib.bad_init(i),-22)
  self.assertEqual(self.lib.unlocked_request(),-1)
  for limit in(0,1001):self.lib.inject(7,limit);self.assertEqual(self.req(89,1),-22)
 def test_active_reset_or_gate_ack_bad_baseline_refuses_adoption(self):
  for gate in(0xf,0x800000ff):
   self.start(adopt=False)
   for i in range(5):self.lib.baseline(i,1)
   for r in(0x48,0x4c,0x50,0x54,0x9c):self.lib.set_reg(r,0xa4600000)
   self.lib.set_reg(0x1080,gate);self.assertEqual(self.lib.adopt(),-112);self.assertFalse(self.events());self.assertEqual(self.lib.inspect(15,0),0)
 def test_lost_validator_after_adoption_latches_and_old_source_resumes(self):
  # Before acquiring an adopted owner, a missing provider is retryable.
  self.start(adopt=False);self.lib.inject(10,1)
  self.assertEqual(self.lib.adopt(),-19);self.assertEqual(self.lib.inspect(5,0),0)
  self.lib.inject(10,0);self.assertEqual(self.lib.adopt(),0)
  for held in (False,True):
   self.start()
   if held:self.assertEqual(self.req(89,1),0)
   prior=self.refs();before=self.lib.inspect(3,0);self.lib.inject(10,1)
   self.assertEqual(self.req(87,1),-19);self.assertEqual(self.lib.inspect(5,0),1)
   self.lib.inject(10,0);self.assertEqual(self.req(87,1),-19)
   self.assertEqual(self.refs(),prior);self.assertEqual(self.lib.inspect(3,0),before)
  # Run the exact earlier admit behavior, rather than infer a historical failure.
  old=(HERE/'cascadia-native-clock.c').read_text().replace(
   'if (!c->validate_epoch)\n\t\treturn latch(c, -ENODEV);',
   'if (!c->validate_epoch)\n\t\treturn -ENODEV;',1)
  with tempfile.TemporaryDirectory() as td:
   stage=Path(td);(stage/'cascadia-native-clock.c').write_text(old)
   (stage/'source-wrapper.c').write_text((HERE/'source-wrapper.c').read_text())
   so=stage/'historical.so'
   subprocess.run(self.cc+['-I'+str(HERE),'-shared','-fPIC',str(stage/'source-wrapper.c'),str(HERE/'native-fixture.c'),'-o',str(so)],check=True)
   lib=ctypes.CDLL(str(so));lib.request.argtypes=[ctypes.c_uint64,ctypes.c_uint,ctypes.c_uint,ctypes.c_int]
   self.assertEqual(lib.start(1,0x24600000),0);self.assertEqual(lib.adopt(),0)
   self.assertEqual(lib.request(7,89,0,1),0);lib.inject(10,1)
   self.assertEqual(lib.request(7,87,0,1),-19);self.assertEqual(lib.inspect(5,0),0)
   lib.inject(10,0);self.assertEqual(lib.request(7,87,0,1),0)
 def test_actual_candidate_sanitizer_executable(self):
  binary=Path(self.tmp.name)/'native-asan'
  subprocess.run(self.cc+['-DNATIVE_SANITIZER_SELFTEST','-fsanitize=address,undefined','-fno-omit-frame-pointer','-g']+self.src+['-o',str(binary)],check=True)
  subprocess.run([str(binary)],check=True)
if __name__=='__main__':unittest.main(verbosity=2)
