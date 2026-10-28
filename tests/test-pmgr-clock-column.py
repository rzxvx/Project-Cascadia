"""Actual mapper versus post-layout historical source, tied to native MMIO oracle."""
import contextlib,hashlib,importlib.util,io,json,os,struct,subprocess,sys,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
HERE=ROOT/'tests/fixtures/p105-pmgr-clock'
BLOB='1887bed05c8b485fbde22aeb4fce35c6a29ad122'
def node(name,props,children):
 props={'name':name.encode()+b'\0',**props};raw=struct.pack('<II',len(props),len(children))
 for key,value in props.items():raw+=key.encode().ljust(32,b'\0')+struct.pack('<I',len(value))+value+b'\0'*((-len(value))%4)
 return raw+b''.join(children)
class Columns(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  spec=importlib.util.spec_from_file_location('candidate',ROOT/'scripts/adt-pmgr-map.py');cls.c=importlib.util.module_from_spec(spec);spec.loader.exec_module(cls.c)
  raw=(HERE/'historical-44-layout-map.py').read_bytes()
  assert hashlib.sha1(b'blob '+str(len(raw)).encode()+b'\0'+raw).hexdigest()==BLOB
  cls.original={'__name__':'historical_layout_only_control'};exec(compile(raw,'pinned-layout-only-map.py','exec'),cls.original)
  cls.rows=json.loads((HERE/'oracle-pmgr-subset.json').read_text())['subset_rows'];cls.table=b''.join(struct.pack('<7I',*r['words'])+r['name'].encode().ljust(16,b'\0')for r in cls.rows)
  cls.phys=json.loads((HERE/'oracle-physical-v3.json').read_text());cls.bind=json.loads((HERE/'oracle-bindings.json').read_text())
 @classmethod
 def output(cls,entry,table,extra=(),peek=None):
  with tempfile.TemporaryDirectory()as tmp:
   d=Path(tmp)/'synthetic.raw';d.write_bytes(node('device-tree',{},[node('arm-io',{},[node('pmgr',{'device-clocks':table},[])])]))
   args=['adt-pmgr-map.py',str(d)]
   if peek:
    p=Path(tmp)/'provided-read-only-dump.txt';p.write_text(peek);args.append(str(p))
   args.extend(extra);old=sys.argv;out=io.StringIO()
   try:
    sys.argv=args
    with contextlib.redirect_stdout(out):entry()
   finally:sys.argv=old
   return out.getvalue()
 def test_actual_historical_clock_b4_is_not_native_usbreg_ec(self):
  old=self.output(self.original['main'],self.table);new=self.output(self.c.main,self.table)
  self.assertIn('3f1000b4',next(entry for entry in old.splitlines()if entry.startswith('USBREG ')))
  self.assertIn('3f1000ec',next(entry for entry in new.splitlines()if entry.startswith('USBREG ')))
  self.assertNotIn('3f1000b4',new)
 def test_original_physical_graph_oracle_and_retained_base_agree(self):
  self.assertEqual(self.phys['kernelcache_sha256'],self.bind['kernelcache_sha256'])
  self.assertEqual(self.bind['retained_map']['no_base_adjustment'],True)
  row=[entry for entry in self.rows if entry['id']==42][0];self.assertEqual(row['words'][4],45)
  actual=[e for e in self.phys['cases'][0]['states'][1]['events']if e[0]=='fake_pmgr_write'];self.assertEqual([e[1]for e in actual],['0xec','0x1098'])
  self.assertEqual(self.c.clock_registers(row['words'][4]),((0x3f1000ec,),None))
 def test_clock_index_zero_terminator_and_valid_upper_boundary(self):
  self.assertEqual(self.c.clock_registers(0),((),None))
  self.assertEqual(self.c.clock_registers(51),((0x3f100104,),None))
  self.assertEqual(self.c.clock_registers(0x00330001),((0x3f10003c,),None))
  self.assertEqual(self.c.clock_registers(0x00332d01),((0x3f10003c,0x3f1000ec,0x3f100104),None))
 def test_encoded_clock_code_not_falsely_mapped_to_low_six_bits(self):
  for code in(0x40|45,0x80|45,0xc0|45):self.assertEqual(self.c.clock_registers(code),((),'encoded:%02x'%code))
  self.assertEqual(self.c.clock_registers(0x00006d01),((0x3f10003c,),'encoded:6d'))
  controls=self.phys['negative_controls'];self.assertEqual(controls[1]['synthetic_options']['clock_word_override'],0x6d)
  self.assertFalse([e for e in controls[1]['states'][0]['events']if e[:2]==['fake_pmgr_write','0xec']])
 def test_startup_invalid_codes_rejected_even_after_zero_terminator(self):
  for code in(52,53,0xff,0x3400):self.assertEqual(self.c.clock_registers(code),((),'invalid'))
  controls={x['packed_clock_word']:x['accepted_by_original_startup']for x in self.bind['packed_clock_guard']}
  self.assertTrue(controls[51]);self.assertFalse(controls[52]);self.assertTrue(controls[0x6d])
  for word in(-1,0x100000000):
   with self.assertRaises(ValueError):self.c.clock_registers(word)
 def test_36byte_clock_address_remains_explicitly_unverified(self):
  self.assertEqual(self.c.clock_registers(45,36),((),'unverified'))
  legacy=struct.pack('<5I',0x2a000000,0,40,38,45)+b'USBREG'.ljust(16,b'\0')
  line=next(entry for entry in self.output(self.c.main,legacy,('--entry-size','36')).splitlines()if entry.startswith('USBREG '));self.assertIn('unverified',line);self.assertNotIn('3f1000',line)
 def test_native_invalid_clock_row_has_no_verified_gate_address(self):
  row=struct.pack('<7I',0x59000000,0,40,34,52,0,0)+b'USB-EHCI'.ljust(16,b'\0')
  line=next(entry for entry in self.output(self.c.main,row).splitlines()if entry.startswith('USB-EHCI '));self.assertIn('invalid-row',line);self.assertNotIn('3f101088',line)
 def test_offline_provided_dump_uses_correct_clock_address_without_device(self):
  # A synthetic root with no gate makes the existing clock-only live column execute.
  row=struct.pack('<7I',0x02000000,0,0,0,45,0,0)+b'PREDIV'.ljust(16,b'\0')
  output=self.output(self.c.main,row,peek='3f1000b4: 11111111\n3f1000ec: 22222222\n')
  self.assertIn('= 22222222',output);self.assertNotIn('11111111',output)
if __name__=='__main__':unittest.main(verbosity=2)
