#!/usr/bin/env python3
"""Host-only lifecycle regression guards; not hardware validation."""
from pathlib import Path
import unittest
A12=Path(__file__).resolve().parents[1]
S=(A12/'initramfs/make-initramfs.sh').read_text().split("cat > \"$ROOT/init\" <<'INIT'\n",1)[1].split('\nINIT',1)[0]
class Lifecycle(unittest.TestCase):
    def test_default_both_profiles_is_a13(self):
        builder=(A12/'initramfs/make-initramfs.sh').read_text()
        self.assertIn('USBWATCH_MODE=${A12_USBWATCH_MODE:-a13}',builder)
        self.assertNotIn('USBWATCH_MODE=${A12_USBWATCH_MODE:-debian}',builder)

    def test_initial_a13_dhcp_only(self):
        block=S.split('touch /tmp/udhcpd.leases',1)[1].split('/usr/sbin/dropbear',1)[0]
        a,b=block.split('else',1)
        self.assertIn('udhcpd /etc/udhcpd.conf',a)
        self.assertNotIn('setsid /sbin/usbwatch',a)
        self.assertIn('setsid /sbin/usbwatch',b)
    def test_cleanup_before_proc_move(self):
        for action in ('killall usbwatch','killall udhcpd'):
            self.assertLess(S.index(action),S.index('mount --move /proc'))
    def test_handoff_after_virtual_mounts_before_switch(self):
        i=S.index('setsid chroot /newroot /run/cascadia/busybox')
        self.assertLess(S.index('mount --move /sys'),i)
        self.assertLess(i,S.index('exec switch_root'))
    def test_fallback_watchdog_after_failed_handoff(self):
        i=S.index('Only the recovery/fallback path')
        self.assertGreater(i,S.index('exec switch_root'))
        self.assertIn('setsid /sbin/usbwatch',S[i:])
    def test_debian_replacement_skips_marker(self):
        s=(A12/'rootfs/debian-overlay/etc/systemd/system/cascadia-usbwatch.service').read_text()
        self.assertIn('ConditionPathExists=!/run/cascadia/a13-usbwatch',s)
        self.assertIn('touch /newroot/run/cascadia/a13-usbwatch',S)
if __name__=='__main__':unittest.main()
