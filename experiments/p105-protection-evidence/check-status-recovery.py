import importlib.util,unittest
from pathlib import Path
P=Path(__file__).resolve().parent
s=importlib.util.spec_from_file_location('full',P/'emulate-status-recovery.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m)
class Full(unittest.TestCase):
 @classmethod
 def setUpClass(c):c.data=m.b.events.load_input(IMAGE12)
 def test_actual_end_consumer_reaches_mode_and_masks(self):
  r=m.Full(self.data).full(8);self.assertEqual([t['register']for t in r['transactions']],['0x0a','0x1b','0x1b']);self.assertEqual(r['final1b1c'],[0xa5,0x93]);self.assertTrue(any(x.get('actual_recovery_entry')=='0x80ed0ce0' for x in r['calls']))
  r=m.Full(self.data,1).full(8);self.assertEqual(r['final1b1c'],[4,128]);self.assertEqual(r['first_timeout'],100)
 def test_timeout_clears_reference_even_when_restore_failed(self):
  r=m.Full(self.data).full(32);self.assertEqual([t['register']for t in r['transactions']],['0x0a','0x41','0x41','0x41']);self.assertEqual(r['final41'],0xaa);self.assertEqual(r['first_timeout'],0)
  r=m.Full(self.data,3).full(32);self.assertEqual(r['final41'],8);self.assertEqual(r['first_timeout'],0);self.assertTrue('error' in r['transactions'][-1])
 def test_timeout_failed_read_clobbers_state_and_resets_reference(self):
  r=m.Full(self.data,1).full(32);self.assertEqual(r['final41'],0);self.assertEqual(r['first_timeout'],0)
if __name__=='__main__':
 import argparse
 parser=argparse.ArgumentParser(description='Actual complete status/recovery caller; fakePMU only')
 parser.add_argument('--kernelcache12',type=Path,required=True)
 IMAGE12=parser.parse_args().kernelcache12
 unittest.main(argv=[__file__],verbosity=2)
