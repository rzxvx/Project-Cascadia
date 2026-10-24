"""Actual candidate mapper versus pinned original repository control; no device."""
import contextlib,hashlib,importlib.util,io,json,os,struct,subprocess,sys,tempfile,unittest,warnings
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
HERE=ROOT/'tests/fixtures/p105-pmgr44'
BLOB='42bec65d851f0b2f5a09ee57294a4a3902d104bc'
def node(name,props,children):
 props={'name':name.encode()+b'\0',**props};out=struct.pack('<II',len(props),len(children))
 for key,value in props.items():out+=key.encode().ljust(32,b'\0')+struct.pack('<I',len(value))+value+b'\0'*((-len(value))%4)
 return out+b''.join(children)
def image(table):return node('device-tree',{},[node('arm-io',{},[node('pmgr',{'device-clocks':table},[])])])
class Table(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  spec=importlib.util.spec_from_file_location('candidate',ROOT/'scripts/adt-pmgr-map.py');cls.mapper=importlib.util.module_from_spec(spec);spec.loader.exec_module(cls.mapper)
  raw=(HERE/'historical-36-map.py').read_bytes()
  assert hashlib.sha1(b'blob '+str(len(raw)).encode()+b'\0'+raw).hexdigest()==BLOB
  cls.original={'__name__':'legacy_control'};exec(compile(raw,'pinned-original-adt-pmgr-map.py','exec'),cls.original)
  cls.fixture=json.loads((HERE/'oracle-pmgr-subset.json').read_text());cls.table=b''.join(struct.pack('<7I',*row['words'])+row['name'].encode().ljust(16,b'\0')for row in cls.fixture['subset_rows'])
 def output(self,entry,table,extra=()):
  with tempfile.TemporaryDirectory()as tmp:
   p=Path(tmp)/'synthetic.raw';p.write_bytes(image(table));out=io.StringIO();old=sys.argv
   try:
    sys.argv=['adt-pmgr-map.py',str(p),*extra]
    with contextlib.redirect_stdout(out), warnings.catch_warnings():
     if entry is self.original['main']:warnings.simplefilter('ignore',ResourceWarning)
     entry()
   finally:sys.argv=old
   return out.getvalue()
 def test_original_control_misses_usb_ehci_candidate_maps_exact_gate(self):
  original=self.output(self.original['main'],self.table);self.assertNotIn('USB-EHCI',original)
  candidate=self.output(self.mapper.main,self.table)
  for name,id,address in(('USB-OTG',88,'3f101084'),('USB-EHCI',89,'3f101088'),('USB-OHCI0',90,'3f101090'),('USB-OHCI1',91,'3f101094')):
   lines=[line.split()for line in candidate.splitlines()if line.startswith(name+' ')];self.assertEqual(len(lines),1);self.assertEqual(lines[0][1:3],[str(id),address])
 def test_exact_original_metadata_names_and_first_words(self):
  entries=self.mapper.device_clock_entries(self.table)
  self.assertEqual([(id,list(words),name)for id,words,name in entries],[(row['id'],row['words'][:5],row['name'])for row in self.fixture['subset_rows']])
  phy=[line for line in self.output(self.mapper.main,self.table).splitlines()if line.startswith('USB-PHY ')][0].split();self.assertEqual(phy[2:4],['-','-'])
 def test_explicit_36_byte_compatibility_never_silently_autodetected(self):
  legacy=struct.pack('<5I',0x59000000,0,40,34,0)+b'USB-EHCI'.ljust(16,b'\0')
  with self.assertRaises(ValueError):self.mapper.device_clock_entries(legacy)
  self.assertEqual(self.mapper.device_clock_entries(legacy,36)[0][2],'USB-EHCI')
  self.assertIn('3f101088',self.output(self.mapper.main,legacy,('--entry-size','36')))
 def test_truncated_empty_oversized_invalid_name_and_unsupported_abi(self):
  for raw in(b'',self.table[:-1],self.table+b'X',b'\0'*(44*4097)):
   with self.assertRaises(ValueError):self.mapper.device_clock_entries(raw)
  bad=struct.pack('<7I',*([0]*7))+b'\xff'*16
  with self.assertRaises(UnicodeDecodeError):self.mapper.device_clock_entries(bad)
  for stride in(0,35,43,48,-44):
   with self.assertRaises(ValueError):self.mapper.device_clock_entries(self.table,stride)
 def test_gate_and_clock_zero_indices_are_unmapped(self):
  rows=self.mapper.device_clock_entries(self.table);phy=[entry for entry in rows if entry[0]==87][0];self.assertEqual(phy[1][3:5],(0,0))
  reg=[entry for entry in rows if entry[0]==42][0];self.assertEqual(reg[1][3:5],(38,45));output=self.output(self.mapper.main,self.table);self.assertIn('3f101098',output);self.assertIn('3f1000ec',output)
 def test_original_kernel_count_oracle_establishes44_not36(self):
  oracle=json.loads((HERE/'oracle-count.json').read_text());self.assertEqual(oracle['source_sha256'],'db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c')
  case=[entry for entry in oracle['count_cases']if entry['OSData_length']==5104][0];self.assertEqual(case['actual_count'],116);self.assertEqual(self.fixture['actual_entry_size'],44)
if __name__=='__main__':unittest.main(verbosity=2)
