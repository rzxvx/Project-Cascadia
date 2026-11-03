#!/usr/bin/env python3
"""Build isolated K93 RAM bundle after the kernel build has completed."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--diagnostic', action='store_true', help='Separate K93 build with handoff checkpoints')
parser.add_argument('--early-trace', action='store_true', help='Use explicitly built decompressor/kernel trace image (implies diagnostic)')
parser.add_argument('--watchdog-seconds', type=int, choices=range(15, 121), metavar='15..120', help='Arm hardware reset for this RAM boot test')
parser.add_argument('--stage-watchdog', action='store_true', help='Use kernel with checkpoint deadlines at30/45/60/75/90/105s')
parser.add_argument('--mmu-watchdog', action='store_true', help='Use checkpoints after CPU validation through MMU-on entry')
parser.add_argument('--init-watchdog', action='store_true', help='Trace C kernel startup through console initialization')
parser.add_argument('--late-watchdog', action='store_true', help='Trace post-console startup and scheduling init')
parser.add_argument('--counter-clock', action='store_true', help='V8 late trace with K93 watchdog counter clock enabled')
args = parser.parse_args()
if args.counter_clock:
    args.late_watchdog = True
if args.late_watchdog:
    args.init_watchdog = True
if args.init_watchdog:
    args.mmu_watchdog = True
if args.mmu_watchdog:
    args.stage_watchdog = True
if args.stage_watchdog:
    if args.watchdog_seconds not in (None, 120):
        parser.error('Stage watchdog needs initial120s deadline')
    args.early_trace = True
    args.watchdog_seconds = 120
if args.early_trace:
    args.diagnostic = True
OUT = ROOT / ('build/k93/bundle-diag-v3' if args.early_trace else 'build/k93/bundle-diag-v2' if args.diagnostic else 'build/k93/bundle')
if args.stage_watchdog:
    OUT = ROOT / 'build/k93/bundle-diag-v4'
if args.mmu_watchdog:
    OUT = ROOT / 'build/k93/bundle-diag-v5'
if args.init_watchdog:
    OUT = ROOT / 'build/k93/bundle-diag-v6'
if args.late_watchdog:
    OUT = ROOT / 'build/k93/bundle-diag-v7'
if args.counter_clock:
    OUT = ROOT / 'build/k93/bundle-diag-v8'
if args.watchdog_seconds:
    OUT = OUT.with_name(OUT.name + f'-wdt{args.watchdog_seconds}')
OUT.mkdir(parents=True, exist_ok=True)
kernel_dir = ROOT / ('build/k93/kernel-trace' if args.early_trace else 'build/out')
if args.stage_watchdog:
    kernel_dir = ROOT / 'build/k93/kernel-trace-staged'
if args.mmu_watchdog:
    kernel_dir = ROOT / 'build/k93/kernel-trace-mmu'
if args.init_watchdog:
    kernel_dir = ROOT / 'build/k93/kernel-trace-init'
if args.late_watchdog:
    kernel_dir = ROOT / 'build/k93/kernel-trace-late'
kernel = (kernel_dir / 'zImage').read_bytes()
kernel_original_hash = hashlib.sha256(kernel).hexdigest()
if struct.unpack_from('<I', kernel, 0x24)[0] != 0x016f2818:
    raise SystemExit('Invalid ARM zImage magic')
if args.diagnostic:
    # The local compressed/head.S explicitly reserves this 32-byte patch
    # area. The first two EFI signature XORs cancel; remaining words are NOPs.
    expected = struct.pack('<8I', 0xe2255a4d, 0xe2255a4d, *([0xe1a00000] * 6))
    if kernel[:32] != expected:
        raise SystemExit('Unexpected ARM zImage entry; refusing trace patch')
    obj = OUT / 'zimage-entry-trace.o'
    raw = OUT / 'zimage-entry-trace.bin'
    subprocess.run(['arm-linux-gnueabihf-as', '-o', str(obj), str(ROOT / 'pongo/k93_zimage_entry_trace.S')], check=True)
    subprocess.run(['arm-linux-gnueabihf-objcopy', '-O', 'binary', '-j', '.text', str(obj), str(raw)], check=True)
    marker = raw.read_bytes()
    if len(marker) != 32:
        raise SystemExit('zImage trace must fit exactly in original 32-byte patch area')
    kernel = marker + kernel[32:]
config = (kernel_dir / 'kernel-config').read_text()
for forbidden in ['CONFIG_MTD=y', 'CONFIG_DRM_APPLE_SGX=y', 'CONFIG_BATTERY_APPLE_HDQ=y']:
    if forbidden in config.splitlines():
        raise SystemExit('Unsafe K93 config: ' + forbidden)
dts = ROOT / 'dts/k93ap.dts'
subprocess.run(['dtc', '-I', 'dts', '-O', 'dtb', '-o', str(OUT / 'k93ap.dtb'), str(dts)], check=True)
dtb = (OUT / 'k93ap.dtb').read_bytes()
if struct.unpack_from('>II', dtb)[0] != 0xd00dfeed or struct.unpack_from('>I', dtb, 4)[0] != len(dtb):
    raise SystemExit('Invalid DTB')
kernel += bytes((-len(kernel)) % 4)
dtb_phys = 0x80008000 + len(kernel)
(OUT / 'staging-generated.h').write_text(f'#define DTB_PHYS {dtb_phys:#x}u\n#define DTB_SIZE {len(dtb):#x}u\n')
# Copy C into the isolated build directory so its quoted generated-header
# include cannot resolve to stale P105 output in pongo/.
source = (ROOT / 'pongo/linux_boot.c').read_text().replace('P105 linux-boot', 'K93 Linux RAM boot')
if args.diagnostic:
    source = (ROOT / 'pongo/k93_linux_boot_diag.c').read_text()
(OUT / 'linux_boot.c').write_text(source)
subprocess.run([
    'arm-linux-gnueabihf-gcc', '-static', '-nostdlib', '-nostartfiles', '-O2',
    '-fno-pie', '-no-pie', '-fno-builtin', '-ffreestanding',
    '-DBOOT_WDT_BASE=0x3f103020', '-DBOOT_STACK_TOP=0x80007000',
    *([f'-DBOOT_WDT_SECONDS={args.watchdog_seconds}'] if args.watchdog_seconds else []),
    '-DFB_TEXT_W=1024', '-DFB_TEXT_H=768', '-DFB_TEXT_STRIDE=4096',
    *(['-DK93_BOOT_TRACE'] if args.diagnostic else []),
    '-I', str(OUT), '-I', str(ROOT / 'pongo'), '-Wl,--build-id=none',
    '-Wl,-T,' + str(ROOT / 'pongo/linux-boot.ld'),
    '-o', str(OUT / 'linux-boot.elf'), str(ROOT / 'pongo/linux_boot_start.S'),
    str(OUT / 'linux_boot.c'),
], check=True)
subprocess.run(['arm-linux-gnueabihf-objcopy', '-O', 'binary', str(OUT / 'linux-boot.elf'), str(OUT / 'linux-boot.bin')], check=True)
loader = (OUT / 'linux-boot.bin').read_bytes()
# Reserve at least 8 KiB below stack top. Never overlap loaded zImage.
if len(loader) >= 0x5000:
    raise SystemExit('Loader overlaps stack reservation')
bundle = loader + bytes(0x8000-len(loader)) + kernel + dtb
if 0x80000000 + len(bundle) >= 0x90000000:
    raise SystemExit('Bundle exceeds reserved lower RAM window')
(OUT / 'staging-bundle.bin').write_bytes(bundle)
manifest = {'board': 'K93AP', 'dtb_phys': hex(dtb_phys), 'stack_top': '0x80007000', 'watchdog_physical': '0x3f103020', 'files': {}}
if args.watchdog_seconds:
    manifest['watchdog_timeout_seconds'] = args.watchdog_seconds
if args.diagnostic:
    manifest['diagnostic'] = {'scanout': '1024x768 stride4096 (readable hardware diagnostic)', 'cpu_mpidr': '0x900', 'markers': ['before L1 flush', 'after L1 flush', 'after L2 flush', 'MMU/caches off, before kernel branch']}
    manifest['diagnostic']['zimage_entry_trace'] = 'white band at y320..335; only first32 bytes replaced, original +0x20 branch preserved'
    manifest['diagnostic']['original_zimage_sha256'] = kernel_original_hash
    if args.early_trace:
        manifest['diagnostic']['kernel_bands'] = {'368': 'decompressor after hyp setup', '400': 'before decompression', '432': 'after decompression', '464': 'cache cleanup returned', '496': 'uncompressed kernel entry', '528': 'CPU lookup returned (not necessarily valid)'}
    if args.stage_watchdog:
        manifest['diagnostic']['reset_deadlines_seconds'] = {'30': 'decompressor after hyp setup', '45': 'before decompression', '60': 'after decompression', '75': 'cache cleanup returned', '90': 'uncompressed kernel entry', '105': 'CPU lookup returned (not necessarily valid)', '120': 'loader deadline, no kernel checkpoint updated it'}
    if args.mmu_watchdog:
        manifest['diagnostic']['reset_deadlines_seconds'] = {'30': 'CPU accepted', '45': 'boot-data validation returned', '60': 'SMP and phys/virt fixups returned', '75': 'page tables ready', '90': 'processor setup returned', '105': 'MMU-on entry', '120': 'loader deadline, no new checkpoint'}
    if args.init_watchdog:
        manifest['diagnostic']['reset_deadlines_seconds'] = {'30': 'C start_kernel entered', '45': 'setup_arch returned', '60': 'scheduler initialized', '75': 'interrupt controller initialized', '90': 'time initialization returned', '105': 'console initialized', '120': 'C entry not reached'}
    if args.late_watchdog:
        manifest['diagnostic']['reset_deadlines_seconds'] = {'30': 'post-console panic check passed', '45': 'entering delay calibration', '60': 'delay calibration returned', '75': 'entering rest_init', '90': 'init threads created before first schedule', '105': 'init task reached kernel_init_freeable', '120': 'no late checkpoint reached'}
for name in ['linux-boot.bin', 'staging-bundle.bin', 'k93ap.dtb']:
    data = (OUT / name).read_bytes()
    manifest['files'][name] = {'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
(OUT / 'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
print(json.dumps(manifest, indent=2))
print('Offline bundle assembled; boot on hardware remains untested.')
