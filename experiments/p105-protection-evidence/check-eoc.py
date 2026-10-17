import importlib.util,os,unittest
from pathlib import Path
P=Path(__file__).resolve().parent
s=importlib.util.spec_from_file_location('eoc',P/'emulate-eoc.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m)
class Recovery(unittest.TestCase):
 @classmethod
 def setUpClass(c):c.inputs={'12':IMAGE12.read_bytes(),'13':IMAGE13.read_bytes()}
 def test_actual_pulse_preservation_both_versions(self):
  for v,d in self.inputs.items():
   for initial in (0,1,8,0xaa,0xff):
    r=m.EOC(d,v).run(initial=initial);self.assertEqual(r['return_code'],0);self.assertEqual(r['final']['41'],initial);self.assertEqual([x['bytes'][0]for x in r['transactions']],[initial,initial&8,initial]);self.assertFalse(r['notifications'])
 def test_read_failure_writes_zero_original_counterexample(self):
  for v,d in self.inputs.items():
   r=m.EOC(d,v).run(errors=(1,));self.assertEqual(r['return_code'],0);self.assertEqual(r['final']['41'],0)
 def test_first_error_hidden_last_error_preserved(self):
  for v,d in self.inputs.items():
   r=m.EOC(d,v).run(errors=(2,));self.assertEqual((r['return_code'],r['final']['41']),(0,0xaa))
   r=m.EOC(d,v).run(errors=(3,));self.assertEqual((r['return_code'],r['final']['41']),(0xe00002c0,8))
 def test_mode_preserves_valid_input_and_propagates_write_only(self):
  for v,d in self.inputs.items():
   for mode,expected in [('mode0',(0xa1,0x13)),('mode1',(0xa5,0x93))]:
    r=m.EOC(d,v).run(kind=mode);self.assertEqual((r['final']['1b'],r['final']['1c']),expected);self.assertEqual(r['return_code'],0)
    r=m.EOC(d,v).run(kind=mode,errors=(2,));self.assertEqual(r['return_code'],0xe00002c0)
   r=m.EOC(d,v).run(kind='mode1',errors=(1,));self.assertEqual((r['final']['1b'],r['final']['1c']),(4,128));self.assertEqual(r['return_code'],0)
 def test_force_outer_discards_errors_and_notifies(self):
  for v,d in self.inputs.items():
   for errors in ((),(1,),(2,)):
    r=m.EOC(d,v).run(kind='force',errors=errors);self.assertEqual(r['return_code'],0);self.assertEqual(len(r['notifications']),1);self.assertTrue(all(x['reg']==0x1b for x in r['transactions']))
 def test_debug_key_routes_to_mode_not_pulse(self):
  for errors in ((),(1,),(2,)):
   r=m.EOC(self.inputs['13'],'13').run(kind='dictionary',errors=errors);self.assertEqual(r['keys'],['AppleForceResetEndOfCharge']);self.assertEqual(r['return_code'],0);self.assertEqual(len(r['notifications']),1);self.assertEqual([x['reg']for x in r['transactions']],[0x1b,0x1b]);self.assertEqual(r['final']['41'],0xaa)
 def test_hash_and_single_use(self):
  with self.assertRaises(ValueError):m.EOC(self.inputs['12'],'13')
  x=m.EOC(self.inputs['13'],'13');x.run()
  with self.assertRaises(RuntimeError):x.run()
if __name__=='__main__':
 import argparse
 parser=argparse.ArgumentParser(description='Original END/TO methods with fakePMU; no hardware')
 parser.add_argument('--kernelcache12',type=Path,required=True)
 parser.add_argument('--kernelcache13',type=Path,required=True)
 args=parser.parse_args();IMAGE12=args.kernelcache12;IMAGE13=args.kernelcache13
 unittest.main(argv=[__file__],verbosity=2)
