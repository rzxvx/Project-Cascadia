#!/usr/bin/env python3
"""Host-only DT/calibration and loader guard tests; no USB, SSH or kernel build."""
from pathlib import Path
import subprocess
import tempfile
import unittest

A12 = Path(__file__).resolve().parents[2]


class RadioTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='j171-radio-test-')
        cls.base = Path(cls.tmp.name)
        for board in ('j171a', 'n841'):
            pp = subprocess.check_output([
                'cpp', '-nostdinc', '-I' + str(A12 / 'dts/include'),
                '-I' + str(A12 / 'dts'), '-undef', '-D__DTS__',
                '-x', 'assembler-with-cpp', str(A12 / f'dts/t8020-{board}.dts')])
            subprocess.run(['dtc', '-q', '-I', 'dts', '-O', 'dtb', '-o',
                            str(cls.base / f'{board}.dtb')], input=pp, check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def get(self, board, node, prop, kind='s'):
        return subprocess.check_output(['fdtget', '-t', kind,
            str(self.base / f'{board}.dtb'), node, prop], text=True).strip()

    def alias(self, board, name):
        return self.get(board, '/aliases', name)

    def test_ipad_radios(self):
        wifi = self.alias('j171a', 'wifi0')
        bt = self.alias('j171a', 'bluetooth0')
        self.assertEqual(self.get('j171a', wifi, 'compatible'), 'pci14e4,43dc')
        self.assertEqual(self.get('j171a', wifi, 'brcm,board-type'), 'apple,magnoliaa')
        self.assertEqual(self.get('j171a', bt, 'apple,power-function', 'x').split()[1:], ['67503131', '0'])
        self.assertEqual(self.get('j171a', bt, 'device-wakeup-gpios', 'u').split()[1:], ['39', '0'])
        props = subprocess.check_output(['fdtget', '-p', str(self.base / 'j171a.dtb'), bt], text=True).splitlines()
        self.assertIn('brcm,broken-read-transmit-power', props)
        self.assertIn('brcm,use-bdaddr-property', props)

    def test_xr_values_preserved(self):
        wifi = self.alias('n841', 'wifi0')
        bt = self.alias('n841', 'bluetooth0')
        self.assertEqual(self.get('n841', wifi, 'brcm,board-type'), 'apple,yodak')
        self.assertEqual(self.get('n841', bt, 'apple,power-function', 'x').split()[1:], ['67503039', '0'])
        self.assertEqual(self.get('n841', bt, 'device-wakeup-gpios', 'u').split()[1:], ['6', '0'])
        self.assertEqual(self.get('n841', str(Path(wifi).parent), 'pwren-gpios', 'u').split()[1:], ['14', '0'])
        props = subprocess.check_output(['fdtget', '-p', str(self.base / 'n841.dtb'), bt], text=True).splitlines()
        self.assertNotIn('brcm,broken-read-transmit-power', props)
        self.assertNotIn('brcm,use-bdaddr-property', props)
        self.assertIn('brcm,broken-ext-scan', props)

    def test_actual_bluetooth_quirk_helper(self):
        patch = (A12 / 'linux/patches/linux-0026-bcm-dt-firmware-quirks.patch').read_text()
        added = '\n'.join(line[1:] for line in patch.splitlines()
                          if line.startswith('+') and not line.startswith('+++'))
        helper = added[:added.index('\n}\n') + 3]
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include <stddef.h>
enum { HCI_QUIRK_BROKEN_READ_TRANSMIT_POWER, HCI_QUIRK_USE_BDADDR_PROPERTY };
struct device { unsigned props; };
struct bcm_device { struct device *dev; };
struct bcm_data { struct bcm_device *dev; };
struct hci_dev { unsigned quirks; };
static bool device_property_read_bool(struct device *d, const char *s) {
 if (!strcmp(s, "brcm,broken-read-transmit-power")) return d->props & 1;
 if (!strcmp(s, "brcm,use-bdaddr-property")) return d->props & 2;
 assert(0); return false;
}
static void hci_set_quirk(struct hci_dev *h, unsigned bit) { h->quirks |= 1U << bit; }
'''
        harness += helper + r'''
int main(void) {
 struct hci_dev h = {4}; struct bcm_data bcm = {NULL};
 bcm_apply_dt_quirks(&bcm, &h); assert(h.quirks == 4);
 for (unsigned props = 0; props < 4; props++) {
  struct device dev = {props}; struct bcm_device bdev = {&dev};
  bcm.dev = &bdev; h.quirks = 4;
  bcm_apply_dt_quirks(&bcm, &h); assert(h.quirks == (4 | props));
 }
 return 0;
}
'''
        source = self.base / 'quirks.c'
        binary = self.base / 'quirks'
        source.write_text(harness)
        subprocess.run(['cc', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=undefined,bounds', str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)

    def test_loader_requires_explicit_vendor_before_ssh(self):
        for mode in ('wifi', 'bluetooth'):
            result = subprocess.run(['bash', str(A12 / 'boot/load-ipad8-radio.sh'), mode], capture_output=True)
            self.assertEqual(result.returncode, 2)
            self.assertIn(b'verified from OTP', result.stderr)

    def test_calibration_rejects_existing_output(self):
        output = self.base / 'existing.dtb'
        output.write_bytes(b'preserve')
        result = subprocess.run(['python3', str(A12 / 'boot/calibrate-ipad8-radio.py'),
            '--syscfg', '/nonexistent', '--dtb', str(self.base / 'j171a.dtb'),
            '--output', str(output)], capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(output.read_bytes(), b'preserve')


if __name__ == '__main__':
    unittest.main()
