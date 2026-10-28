import json, subprocess, tempfile, unittest
from pathlib import Path
HERE=Path(__file__).resolve().parents[1]/'experiments/p105-current'
class Core(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.tmp=tempfile.TemporaryDirectory()
  cls.binary=Path(cls.tmp.name)/'core-fixture'
  subprocess.run(['gcc','-DCASCADIA_OFFLINE','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=undefined','-fno-sanitize-recover=all','-g','-O1',str(HERE/'d1946-current-core.c'),str(HERE/'core-fixture.c'),'-o',str(cls.binary)],check=True)
 @classmethod
 def tearDownClass(cls): cls.tmp.cleanup()
 def test_actual_code_terminal_registers(self):
  oracle=json.loads((HERE/'oracle-registers.json').read_text())
  self.assertEqual(oracle['sha256'],'db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c')
  self.assertEqual(len(oracle['cases']),16)
  for c in oracle['cases']:
   with self.subTest(kind=c['kind'],value=c['value'],control=c['control']):
    result=subprocess.check_output([str(self.binary),c['kind'],str(c['value']),str(c['control'])],text=True).strip()
    self.assertEqual(result,c['registers'])
 def test_checked_io_boundaries(self):
  self.assertIn('fixtures PASS',subprocess.check_output([str(self.binary),'negative'],text=True))
if __name__=='__main__': unittest.main()
