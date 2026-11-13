#!/usr/bin/env python3
"""Offline audio integration checks; no firmware, USB, MMIO or boot jobs."""
from pathlib import Path
import importlib.util, tempfile,unittest,subprocess,json
A=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('audio_installer',A/'rootfs/install-ipad-audio.py');M=importlib.util.module_from_spec(spec);spec.loader.exec_module(M)
class Audio(unittest.TestCase):
 def test_other_profiles_noop(self):
  for board in ['xr','xsmax']:
   with tempfile.TemporaryDirectory() as d:
    root=Path(d);self.assertFalse(M.install(root,board)['installed']);self.assertEqual(list(root.iterdir()),[])
 def test_root_install_idempotent(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d);(root/'etc').mkdir();(root/'etc/passwd').write_text('root:x:0:0:root:/root:/bin/sh\n')
   M.install(root,'ipad8','none',2);M.install(root,'ipad8','none',4)
   self.assertEqual((root/'etc/cascadia/ipad8-audio.conf').read_text(),'attenuation=2\n')
   self.assertEqual((root/'etc/systemd/system/multi-user.target.wants/ipad8-audio-enable.service').readlink(),Path('../ipad8-audio-enable.service'))
   self.assertIn('apple,j171a',(root/'usr/local/sbin/ipad8-audio-enable').read_text())
 def test_omarchy_buttons_and_power_owner(self):
  with tempfile.TemporaryDirectory() as d:
   root=Path(d)
   for rel in ['etc','usr/libexec','usr/local/bin','home/tablet/.local/share/omarchy-ipad/shell','home/tablet/.config/labwc','home/tablet/.config/omarchy-ipad']:
    (root/rel).mkdir(parents=True,exist_ok=True)
   (root/'etc/passwd').write_text('root:x:0:0:root:/root:/bin/sh\n')
   (root/'home/tablet/.local/share/omarchy-ipad/shell/shell.qml').touch()
   (root/'usr/local/bin/omarchy-volume').write_text('#!/bin/sh\n')
   (root/'home/tablet/.config/omarchy-ipad/shell.json').write_text('{"disabledPlugins":["omarchy.osd","other"],"bar":{"layout":{"right":[]}}}')
   (root/'home/tablet/.config/labwc/rc.xml').write_text('<labwc_config><keyboard><keybind key="XF86AudioRaiseVolume"><action name="Execute" command="brightness" /></keybind></keyboard></labwc_config>')
   (root/'usr/libexec/omarchy-powerd').write_text("from pathlib import Path\nclass Buttons:\n pass\ndef scan(name,path,self):\n            if name != 'gpio-keys' or path in self.keys.values():\n                pass\n")
   M.install(root,'ipad8','omarchy',2);M.install(root,'ipad8','omarchy',2)
   data=json.loads((root/'home/tablet/.config/omarchy-ipad/shell.json').read_text())
   self.assertNotIn('omarchy.osd',data['disabledPlugins']);self.assertIn('other',data['disabledPlugins'])
   self.assertEqual(data['bar']['layout']['right'],[{'id':'omarchy.audio'}])
   rc=(root/'home/tablet/.config/labwc/rc.xml').read_text();self.assertIn('omarchy-volume lower',rc);self.assertNotIn('brightness',rc)
   power=(root/'usr/libexec/omarchy-powerd').read_text();self.assertEqual(power.count('def cascadia_power_key_device('),1);self.assertIn('apple,j171a',power)
 def test_no_symlink_escape(self):
  with tempfile.TemporaryDirectory() as d, tempfile.TemporaryDirectory() as outside:
   root=Path(d);(root/'etc').mkdir();(root/'etc/passwd').touch();(root/'usr').symlink_to(outside)
   with self.assertRaises(ValueError):M.install(root,'ipad8','none')
   self.assertEqual(list(Path(outside).iterdir()),[])
 def test_kernel_gates_and_gain(self):
  s=(A/'linux/extras/apple-j171-speakers.c').read_text()
  for x in ['of_machine_is_compatible("apple,j171a")','READ_ONCE(attenuation)','divisor != 1 && divisor != 2 && divisor != 4','0600','GPIOD_OUT_LOW']:
   self.assertIn(x,s)
  self.assertIn('dev_dbg(a->dev,"speaker prepared:',s)
  sio=(A/'linux/extras/apple-sio-core.c').read_text();self.assertIn('of_machine_is_compatible("apple,j171a")',sio)
  for name in ['sio-cpu','spi-shim','uart-shim','audio-shim']:self.assertIn('"'+name+'"',sio)
  self.assertLess(sio.index('err = sio_j171_hold_domains'),sio.index('sio->rtk = devm_apple_rtkit_init'))
 def test_board_nodes(self):
  for board in ['n841','d331p']:
   s=(A/f'dts/t8020-{board}.dts').read_text();self.assertNotIn('j171a-speakers',s);self.assertNotIn('j171-volume-keys',s)
  s=(A/'dts/t8020-j171a.dts').read_text()
  for x in ['cascadia,j171a-speakers','cascadia,j171a-nco','<&pinctrl_ap 137 1>','linux,code = <114>']:
   self.assertIn(x,s)
 def test_m1n1_layout_guard(self):
  s=(A/'m1n1/ipad8/0013-j171-sio-audio-layout.patch').read_text();self.assertIn('adt_is_compatible(adt, 0, "J171aAP")',s)
  for x in ['SSPI','SUAR','SAUD','SQSP','ADMA','AAUD','0xffffffff']:self.assertIn(x,s)
 def test_power_input_gate(self):
  from unittest.mock import Mock,patch
  ns={};exec(M.POWER_HELPER,ns)
  fn=ns['cascadia_power_key_device']
  for board,accepted in [(b'apple,j171a\0apple,t8020\0',True),(b'apple,n841\0',False),(b'apple,d331p\0',False)]:
   with patch.dict(ns,{'Path':lambda _:Mock(read_bytes=lambda:board)}):
    self.assertEqual(fn('Apple SMC power/lid events'),accepted)
    self.assertTrue(fn('gpio-keys'));self.assertFalse(fn('j171-volume-keys'))
 def test_shell_syntax(self):
  for p in [A/'cascadia', A/'linux/build-kernel.sh', A/'rootfs/build-debian-rootfs.sh', *list((A/'rootfs/audio-overlay/usr/local/sbin').glob('*'))]:
   subprocess.run(['bash','-n',str(p)],check=True)
if __name__=='__main__':unittest.main()
