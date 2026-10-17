#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Seven optional, bounded actual-code groups; never accesses hardware."""
import hashlib, importlib.util, json, os, subprocess, sys, unittest
from pathlib import Path
STAGE=Path(__file__).resolve().parent
SOURCE=Path(os.environ.get('CASCADIA_SOURCE_ROOT','/source'))
KC12=Path(os.environ.get('CASCADIA_TEST_KERNELCACHE',str(SOURCE/'build/firmware/kernelcache.12H321.macho')))
KC13=Path(os.environ.get('CASCADIA_TEST_KERNELCACHE_13G36','/firmware13/kernelcache.13G36.macho'))
AVAILABLE=importlib.util.find_spec('unicorn') is not None
MANIFEST=json.loads((STAGE/'source-provenance.json').read_text())
def check_inputs(build,path):
 expected=MANIFEST['inputs'][build]['sha256']
 if hashlib.sha256(path.read_bytes()).hexdigest()!=expected:raise AssertionError('pinned image mismatch')
 for relative,sha in MANIFEST['fixtures'].items():
  if hashlib.sha256((STAGE/relative).read_bytes()).hexdigest()!=sha:raise AssertionError('fixture drift: '+relative)
 for relative,sha in MANIFEST['dependencies'].items():
  if hashlib.sha256((SOURCE/relative).read_bytes()).hexdigest()!=sha:raise AssertionError('dependency drift: '+relative)
def run(name,*args):
 p=subprocess.run([sys.executable,str(STAGE/name),*map(str,args)],check=True,capture_output=True,text=True)
 return json.loads(p.stdout)
class IRQ12(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  if not AVAILABLE or not KC12.is_file():raise unittest.SkipTest('optional Unicorn/pinned12H321 input unavailable')
  check_inputs('12H321',KC12)
  cls.data=run('emulate-drain-private.py',SOURCE,KC12)['cases']
 def test_disabled_queued_event(self):
  d=self.data['disabled_queued_event'];self.assertEqual(d['before']['enabled'],0);self.assertEqual(len(d['actions']),1);self.assertEqual(d['after']['consumer'],1)
 def test_failed_read_stale_buffer(self):
  d=self.data['failed_read_stale_buffer'];self.assertEqual(d['ADC_state'],2);self.assertIn('error',d['transactions'][0])
 def test_cached_resume_completion(self):
  d=self.data['cached_resume_completion'];self.assertEqual(d['ADC_state'],2);self.assertEqual(d['transactions'][0]['bytes'],'000000000000000000')
 def test_auto_reenable(self):self.assertEqual(self.data['auto_reenable_without_explicit_disable']['provider_calls'][0]['mocked'],'provider enableInterrupt')
 def test_failed_disable(self):
  d=self.data['provider_disable_error_ignored'];self.assertEqual(d['before']['enabled'],0);self.assertEqual(d['provider_calls'][0]['synthetic_return'],0xe00002c0)
class Provider12(unittest.TestCase):
 def test_native_provider_and_register_prefix(self):
  if not AVAILABLE or not KC12.is_file():self.skipTest('optional Unicorn/pinned12H321 input unavailable')
  check_inputs('12H321',KC12);d=run('emulate-provider-private.py',KC12)
  self.assertEqual(d['IOService_type'],1);self.assertEqual(d['actual_register_prefix']['vector_number'],19);self.assertFalse(d['complete_registration_executed']);self.assertFalse(d['physical_quiescence_granted'])
class Dispatch13(unittest.TestCase):
 def test_actual_constructor_getter_stubs_and_callback(self):
  if not AVAILABLE or not KC13.is_file():self.skipTest('optional Unicorn/pinned13G36 input unavailable')
  check_inputs('13G36',KC13);d=run('emulate-dispatch13-private.py',KC13)
  self.assertEqual(d['actual_constructor_vptr'],'0x8114d214');self.assertEqual(d['bounded_getter_arrival'][0]['entry'],'0x811472ac')
  self.assertEqual([s['return'] for s in d['generic_slots_actual_stubs']],[0,0,0xe00002c7,1])
  self.assertEqual([c['after_state'] for c in d['completion_callbacks']],[0,2,2,3]);self.assertFalse(d['physical_quiescence_granted'])
if __name__=='__main__':unittest.main()
