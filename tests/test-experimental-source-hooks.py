"""Apply both isolated DWC2 hook patches strictly to a disposable source copy."""
from pathlib import Path
import os,shutil,subprocess,tempfile,unittest
REPO=Path(__file__).resolve().parents[1]
LINUX=Path(os.environ.get('CASCADIA_TEST_LINUX',str(REPO/'build/linux')))
SOURCE=REPO/'experiments/p105-source'
class Hooks(unittest.TestCase):
 def test_exact_hooks_apply_with_strict_whitespace(self):
  if not shutil.which('git') or not (LINUX/'drivers/usb/dwc2/core.h').exists():
   self.skipTest('git and reviewed patched Linux6.12 sources required')
  with tempfile.TemporaryDirectory(prefix='p105-source-hooks-')as name:
   dest=Path(name);(dest/'drivers/usb/dwc2').mkdir(parents=True)
   for name in ('core.h','gadget.c','core_intr.c','platform.c','drd.c','Makefile','Kconfig'):
    shutil.copyfile(LINUX/'drivers/usb/dwc2'/name,dest/'drivers/usb/dwc2'/name)
   for name in ('p105-udc-observer.c','p105-udc-observer.h'):
    shutil.copyfile(SOURCE/'observer-base'/name,dest/'drivers/usb/dwc2'/name)
   for name in ('dwc2-observer-hooks.patch','dwc2-source-reader.patch'):
    subprocess.run(['git','apply','--check','--whitespace=error',str(SOURCE/name)],cwd=dest,check=True)
    subprocess.run(['git','apply','--whitespace=error',str(SOURCE/name)],cwd=dest,check=True)
   for name in ('p105-udc-observer.c','p105-udc-observer.h'):
    self.assertEqual((dest/'drivers/usb/dwc2'/name).read_bytes(),(SOURCE/name).read_bytes())
   text=(dest/'drivers/usb/dwc2/Kconfig').read_text()
   for symbol in ('P105_DWC2_POWER_OBSERVER','P105_DWC2_SOURCE_READER','P105_COMBINED_CURRENT_PROVIDER'):
    self.assertIn('config '+symbol,text)
   self.assertIn('bool p105_dwc2_matches',(dest/'drivers/usb/dwc2/platform.c').read_text())
   self.assertIn('p105_power_cookie',(dest/'drivers/usb/dwc2/core.h').read_text())
if __name__=='__main__':unittest.main()
