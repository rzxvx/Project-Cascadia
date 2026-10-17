import importlib.util,os,struct,unittest
from pathlib import Path
from unicorn.arm_const import UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_SP,UC_ARM_REG_LR,UC_ARM_REG_PC
P=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('adc',P/'emulate-original-adc6.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
class Original(unittest.TestCase):
 @classmethod
 def setUpClass(c):c.data=IMAGE.read_bytes()
 def leaf(self,x,off,value=0):
  x.m.reg_write(UC_ARM_REG_R0,value&0xffffffff);x.m.reg_write(UC_ARM_REG_R1,value>>32);x.m.reg_write(UC_ARM_REG_SP,m.S+0xf000);x.m.reg_write(UC_ARM_REG_LR,m.E|1);x.m.emu_start(m.B+off|1,m.E,count=10000)
  self.assertEqual(x.m.reg_read(UC_ARM_REG_PC),m.E);return x.m.reg_read(UC_ARM_REG_R0)|(x.m.reg_read(UC_ARM_REG_R1)<<32)
 def test_actual_clock_div24_and_inverse(self):
  for value in (0,1,23,24,25,48000,960000,1200000,0xffffffff,0x100000000,0x123456789abcd,0xffffffffffffffff):
   x=m.ADC(self.data);self.assertEqual(self.leaf(x,0x1bfc,value),value//24)
  for us in (0,1,2000,40000,50000,0xffffffff,0x100000000,0xffffffffffffffff//24):
   x=m.ADC(self.data);self.assertEqual(self.leaf(x,0x1c0c,us),us*24)
 def test_actual_current_counter_and_frequency_literal(self):
  for us in (1,2000,40000,50000,0xffffffff,0x100000000):
   x=m.ADC(self.data);x.step_us=1;x.fake_us=us-1;self.assertEqual(self.leaf(x,0x1ca98),us)
  x=m.ADC(self.data);self.assertEqual(self.leaf(x,0x1c1c)&0xffffffff,24000000)
 def test_success_raw_and_cleanup(self):
  for raw in (0,1,0xabc,4095):
   r=m.ADC(self.data).run(raw=raw);self.assertEqual((r['return_code'],r['sample_output'],r['final_control']),(0,raw,0));self.assertEqual([(t['op'],t['reg']) for t in r['transactions']],[('write',67),('read',67),('read',68),('write',67)])
  r=m.ADC(self.data).run(highflag=0x80);self.assertEqual(r['sample_output'],0xabc)
 def test_busy_timeout_retains_start_no_cleanup(self):
  r=m.ADC(self.data).run(busy_reads=100);self.assertEqual((r['return_code'],r['sample_output'],r['final_control']),(0xffffffff,0xa5a5a5a5,0x26));self.assertEqual(len([t for t in r['transactions'] if t['op']=='write']),1);self.assertFalse(any(t['reg']==68 for t in r['transactions']))
 def test_original_ignored_start_poll_sample_cleanup_errors(self):
  for ordinal,sample,control in ((1,0xabc,0),(2,0xabc,0),(3,0,0),(4,0xabc,6)):
   r=m.ADC(self.data).run(errors=(ordinal,));self.assertEqual((r['return_code'],r['sample_output'],r['final_control']),(0,sample,control))
 def test_no_event_reads_and_no_extra_writes(self):
  for kwargs in ({},{'busy_reads':1},{'busy_reads':100},{'errors':(1,)},{'errors':(2,)},{'errors':(3,)},{'errors':(4,)}):
   r=m.ADC(self.data).run(**kwargs);self.assertTrue(all(t['reg']in(67,68) for t in r['transactions']));self.assertTrue(all(t['reg']==67 for t in r['transactions'] if t['op']=='write'))
if __name__=='__main__':
 import argparse
 parser=argparse.ArgumentParser(description='Original ADC6/timer instruction controls; fakeI/O only')
 parser.add_argument('--image',type=Path,required=True)
 IMAGE=parser.parse_args().image
 unittest.main(argv=[__file__],verbosity=2)
