#!/usr/bin/env python3
"""Prepare isolated, RAM-only K93 bring-up inputs; never touches the device."""
import hashlib
import io
import pathlib
import re
import shutil
import tarfile
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/k93'
OUT.mkdir(parents=True, exist_ok=True)
url = 'https://dl-cdn.alpinelinux.org/alpine/v3.23/main/armv7/busybox-static-1.37.0-r30.apk'
apk = OUT / 'busybox-static-1.37.0-r30.apk'
if not apk.exists():
    with urllib.request.urlopen(url, timeout=60) as response:
        data = response.read(2 * 1024 * 1024)
    if len(data) != 628018:
        raise RuntimeError('Unexpected APK size')
    apk.write_bytes(data)
data = apk.read_bytes()
expected = 'fb023d6a1e2dce9069b11b302e674c273b9e17d04d17b03ee16526475b0971e6'
if hashlib.sha256(data).hexdigest() != expected:
    raise RuntimeError('BusyBox package SHA256 mismatch')
print('Verified official HTTPS package SHA256:', expected)
# APK v2 contains concatenated gzip members. gzip decompresses all members;
# tarfile ignore_zeros traverses the concatenated tar archives.
import gzip
with tarfile.open(fileobj=io.BytesIO(gzip.decompress(data)), mode='r:', ignore_zeros=True) as archive:
    matches = [m for m in archive.getmembers() if m.name == 'bin/busybox.static']
    if len(matches) != 1 or not matches[0].isfile():
        raise RuntimeError('Expected exactly one regular busybox.static')
    binary = archive.extractfile(matches[0]).read()
if binary[:7] != b'\x7fELF\x01\x01\x01' or binary[18:20] != b'\x28\x00':
    raise RuntimeError('Expected ARM ELF32 little-endian executable')
rootfs = OUT / 'rootfs'
for directory in ['bin', 'sbin', 'dev', 'proc', 'sys', 'tmp', 'run', 'etc', 'root']:
    (rootfs / directory).mkdir(parents=True, exist_ok=True)
(rootfs / 'bin/busybox').write_bytes(binary)
(rootfs / 'bin/busybox').chmod(0o755)
for applet in ['sh', 'mount', 'mkdir', 'setsid', 'uname', 'sleep', 'ls', 'cat', 'dmesg', 'free', 'ps', 'echo', 'stty', 'sync', 'poweroff']:
    path = rootfs / 'bin' / applet
    if not path.is_symlink():
        path.symlink_to('busybox')
shutil.copyfile(ROOT / 'rootfs/k93/init', rootfs / 'init')
(rootfs / 'init').chmod(0o755)
# Preserve upstream trim/dependencies while disabling unverified peripherals.
disabled = set('MTD MTD_APPLE_H2FMI DRM_APPLE_SGX BATTERY_APPLE_HDQ BACKLIGHT_APPLE_PMU_WLED MFD_APPLE_PMU_I2C REGULATOR_APPLE_PMU_PWRSW TOUCHSCREEN_APPLE_Z2 USB_EHCI_HCD USB_EHCI_HCD_PLATFORM BRCMFMAC BRCMFMAC_USB BT BLK_DEV_NBD HFSPLUS_FS'.split())
lines = ['# Generated K93 RAM-only config from the pinned upstream P105 fragment.']
for line in (ROOT / 'config/p105ap.config').read_text().splitlines():
    match = re.match(r'CONFIG_(\w+)=', line)
    if match and match[1] in disabled:
        line = f'# CONFIG_{match[1]} is not set'
    lines.append(line)
(OUT / 'k93.config').write_text('\n'.join(lines) + '\n')
print('Prepared', rootfs, 'and', OUT / 'k93.config')
