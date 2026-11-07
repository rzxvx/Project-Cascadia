#!/usr/bin/env python3
"""Host-only transport isolation checks: no USB or hardware power writes."""
from pathlib import Path
import hashlib
import re
import subprocess
import tempfile
import unittest

A12=Path(__file__).resolve().parents[2]
PATCH=A12/'linux/patches/linux-0010-spi-apple-legacy.patch'


class SpiGating(unittest.TestCase):
    def test_remote_xr_patch_restored_exactly(self):
        self.assertEqual(hashlib.sha256(PATCH.read_bytes()).hexdigest(),
                         'a7fa14024da85c5e87eab48ba1bb490a9feead277c8f1b4e671ed2cd1239bc98')
        self.assertIn('No runtime PM',PATCH.read_text())
        self.assertIn('0xa4f',PATCH.read_text())

    def test_j171_matches_no_xr_or_generic_controller(self):
        s=(A12/'linux/extras/spi-apple-sio.c').read_text()
        table=s.split('static const struct of_device_id apple_spi_of_match[] = {',1)[1].split('};',1)[0]
        self.assertEqual(re.findall(r'\.compatible\s*=\s*"([^"]+)"',table),['apple,j171a-spi'])
        self.assertIn('.name = "apple-spi-j171"',s)
        self.assertIn('ctlr->auto_runtime_pm = true;',s) # preserved for J171 only
        self.assertIn('devm_pm_runtime_enable',s)

    def test_staging_does_not_overwrite_xr_driver(self):
        s=(A12/'linux/build-kernel.sh').read_text()
        self.assertNotIn('cp "$A12/linux/extras/spi-apple-sio.c" \\\n   "$K/drivers/spi/spi-apple.c"',s)
        self.assertIn('"$K/drivers/spi/spi-apple-j171.c"',s)
        self.assertIn('obj-$(CONFIG_SPI_APPLE) += spi-apple-j171.o',s)
        self.assertNotIn('git -C "$K" reset',s)
        self.assertNotIn('make clean',s)

    def test_real_device_tree_selection(self):
        with tempfile.TemporaryDirectory(prefix='a12-spi-gating-') as d:
            for board,controller,expected in [('n841','235104000','apple,t8020-spi'),
                                               ('j171a','23510c000','apple,j171a-spi')]:
                pp=subprocess.check_output(['cpp','-nostdinc','-I'+str(A12/'dts/include'),
                    '-I'+str(A12/'dts'),'-undef','-D__DTS__','-x','assembler-with-cpp',
                    str(A12/'dts'/('t8020-'+board+'.dts'))])
                dt=Path(d)/(board+'.dtb')
                subprocess.run(['dtc','-q','-I','dts','-O','dtb','-o',str(dt)],input=pp,check=True)
                value=subprocess.check_output(['fdtget','-t','s',str(dt),'/soc/spi@'+controller,'compatible'],text=True).strip()
                self.assertEqual(value,expected)

    def test_patched_xr_legacy_probe_keeps_no_runtime_pm(self):
        k=A12/'build/linux'
        if not (k/'.git').exists():self.skipTest('requires the pinned kernel source checkout')
        baseline=subprocess.check_output(['git','-C',str(k),'show','HEAD:drivers/spi/spi-apple.c'])
        with tempfile.TemporaryDirectory(prefix='a12-spi-legacy-') as d:
            p=Path(d)/'drivers/spi/spi-apple.c';p.parent.mkdir(parents=True);p.write_bytes(baseline)
            subprocess.run(['git','apply',str(PATCH)],cwd=d,check=True)
            s=p.read_text();block=s.split('if (spi->legacy) {',1)[1].split('\n\tirq = platform_get_irq',1)[0]
            self.assertIn('apple_spi_legacy_transfer_one',block)
            self.assertIn('return 0;',block)
            self.assertNotIn('auto_runtime_pm',block)
            self.assertNotIn('pm_runtime_enable',block)
            self.assertNotIn('pm_runtime_set_active',block)


if __name__=='__main__':unittest.main()
