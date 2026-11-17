#!/usr/bin/env python3
"""Offline checks for J171-only display/backlight/rootfs integration."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest

A12 = Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('ipad_display',A12/'rootfs/install-ipad-display.py')
installer=importlib.util.module_from_spec(spec);spec.loader.exec_module(installer)

class DisplayIntegration(unittest.TestCase):
    def test_backlight_board_and_address_guards(self):
        s=(A12/'linux/extras/apple-j171-backlight.c').read_text()
        self.assertIn('of_machine_is_compatible("apple,j171a")',s)
        self.assertIn('pmic->usid == 0x0f',s)
        self.assertIn('"cascadia,j171a-pmic-backlight"',s)
        self.assertIn('#define J171_DAC 0x1e00',s)
        self.assertIn('SMC_KEY(rLDl)',s)
        for board in ['n841','d331p']:
            self.assertNotIn('cascadia,j171a-pmic-backlight',(A12/f'dts/t8020-{board}.dts').read_text())

    def test_default_writes_but_no_probe_write(self):
        s=(A12/'linux/extras/apple-j171-backlight.c').read_text()
        self.assertIn('static bool allow_writes = true;',s)
        probe=s[s.index('static int j171_probe'):s.index('static const struct of_device_id')]
        self.assertNotIn('regmap_bulk_write(',probe)
        self.assertNotIn('apple_smc_write_u32(',probe)
        self.assertNotIn('backlight_update_status(',probe)
        self.assertIn('props.max_brightness = bl->boot_level',probe)
        self.assertIn('if (level > bl->boot_level)',s)
        self.assertIn('ret = ret == level ? 0 : -EIO;',s)

    def test_staged_by_normal_builder(self):
        s=(A12/'linux/build-kernel.sh').read_text()
        self.assertIn('linux/extras/apple-j171-backlight.c',s)
        self.assertIn('obj-$(CONFIG_BACKLIGHT_CLASS_DEVICE) += apple-j171-backlight.o',s)
        s=(A12/'boot/package-device-linux.py').read_text()
        self.assertIn("if p['name']=='ipad8': extra += ' cma=64M'",s)
        self.assertNotIn('allow_writes=0',s)

    def test_all_board_dtbs_compile(self):
        with tempfile.TemporaryDirectory() as tmp:
            for board in ['j171a','n841','d331p']:
                source=subprocess.check_output(['cpp','-nostdinc','-I'+str(A12/'dts/include'),
                    '-I'+str(A12/'dts'),'-undef','-D__DTS__','-x','assembler-with-cpp',str(A12/f'dts/t8020-{board}.dts')])
                dtb=Path(tmp)/(board+'.dtb')
                subprocess.run(['dtc','-q','-I','dts','-O','dtb','-o',str(dtb)],input=source,check=True)
                if board=='j171a':
                    node='/soc/spmi@23d180700/pmic@f/backlight@1e00'
                    self.assertEqual(subprocess.check_output(['fdtget','-t','s',str(dtb),node,'compatible'],text=True).strip(),'cascadia,j171a-pmic-backlight')
                    self.assertEqual(subprocess.check_output(['fdtget','-t','s',str(dtb),'/soc/dsi@230400000','status'],text=True).strip(),'disabled')
                if board=='n841':
                    self.assertEqual(subprocess.check_output(['fdtget','-t','s',str(dtb),'/soc/display-pipe@230000000','status'],text=True).strip(),'disabled')

    def root(self,tmp):
        r=Path(tmp);(r/'etc').mkdir();(r/'etc/os-release').write_text('ID=fixture\n')
        return r

    def test_other_profiles_rejected_without_changes(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=self.root(tmp)
            for profile in ['xr','xsmax']:
                with self.assertRaises(ValueError):installer.install(root,profile)
            self.assertFalse((root/'usr').exists())
        with self.assertRaises(ValueError):installer.install(Path('/'),'ipad8')

    def test_debian_without_omarchy_is_not_replaced(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=self.root(tmp);installer.install(root,'ipad8')
            self.assertTrue((root/'etc/udev/rules.d/70-cascadia-j171-backlight.rules').is_file())
            self.assertFalse((root/'usr/local/libexec/arch-ipad-login-session').exists())

    def test_omarchy_wiring_idempotent(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=self.root(tmp);s=root/'usr/local/libexec/arch-ipad-login-session';s.parent.mkdir(parents=True)
            original='#!/bin/bash\nexport WLR_RENDERER=pixman\nexec /usr/bin/labwc -m\n';s.write_text(original)
            installer.install(root,'ipad8');first=s.read_bytes();installer.install(root,'ipad8')
            self.assertEqual(s.read_bytes(),first)
            self.assertEqual(s.with_name(s.name+'.before-cascadia-display').read_text(),original)
            self.assertIn('cascadia-ipad-labwc -m',s.read_text())
            self.assertNotIn('/run/cascadia/enable-agx-desktop',s.read_text())

    def test_unrecognized_launcher_is_not_modified(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=self.root(tmp);s=root/'usr/local/libexec/arch-ipad-login-session';s.parent.mkdir(parents=True)
            s.write_text('#!/bin/sh\nexec custom-session\n')
            with self.assertRaises(ValueError):installer.install(root,'ipad8')
            self.assertFalse((root/'etc/udev').exists())

    def test_native_gpu_wrapper_gates_and_fallback(self):
        s=(A12/'rootfs/display-overlay/usr/local/bin/cascadia-ipad-labwc').read_text()
        self.assertIn('grep -qx apple,j171a',s)
        self.assertIn('= apple-h11p',s)
        self.assertIn('WLR_RENDERER=gles2',s)
        self.assertIn('WLR_RENDERER=pixman',s)
        self.assertNotIn('/run/cascadia/enable-agx-desktop',s)
        rule=(A12/'rootfs/display-overlay/etc/udev/rules.d/70-cascadia-j171-backlight.rules').read_text()
        self.assertIn('KERNEL=="j171-backlight"',rule)
        self.assertIn('0664',rule)
        self.assertNotIn('0666',rule)

if __name__=='__main__':unittest.main()
