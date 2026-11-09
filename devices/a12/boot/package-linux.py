#!/usr/bin/env python3
import hashlib, json, os, pathlib, shutil, subprocess, sys, tempfile
root = pathlib.Path(__file__).resolve().parents[2]
a12 = root / 'a12'
payload = a12 / 'build/payload'
# A12_BOARD: n841 (XR, the default) or d331p (XS Max)
board = os.environ.get('A12_BOARD', 'n841')
sub = '' if board == 'n841' else '/' + board
out = a12 / f'boot/inputs{sub}'
out.mkdir(parents=True, exist_ok=True)
parts = [
    payload / 'm1n1-idevice.macho',
    payload / f't8020-{board}.dtb',
    payload / 'Image.gz',
    payload / ('initramfs.cpio.gz' if board == 'n841' else f'initramfs-{board}.cpio.gz'),
]
for p in parts:
    if not p.is_file(): raise SystemExit(f'missing {p}')

dtb = parts[1].read_bytes()
syscfg_path = os.environ.get('XR_SYSCFG')
if syscfg_path:
    # Read private per-phone values from outside the repo. Never include the
    # raw syscfg, MAC address, or calibration in the printed manifest.
    sys.path.insert(0, str(root / 'a13/tools'))
    from syscfg import parse
    keys = parse(pathlib.Path(syscfg_path).read_bytes())
    for key in ('WMac', 'WSKU', 'WCAL', 'BMac'):
        if key not in keys: raise SystemExit(f'syscfg missing {key}')
    sku = keys['WSKU'][8:16].rstrip(b'\0').decode('ascii')
    if not (1 <= len(sku) <= 8 and sku.isalnum()):
        raise SystemExit('invalid Wi-Fi antenna SKU in syscfg')
    if len(keys['WMac']) < 6 or not keys['WCAL']:
        raise SystemExit('invalid Wi-Fi MAC or calibration in syscfg')
    if len(keys['BMac']) < 6 or not any(keys['BMac'][:6]):
        raise SystemExit('invalid Bluetooth address in syscfg')
    with tempfile.TemporaryDirectory(prefix='xr-dtb-') as tmp:
        staged = pathlib.Path(tmp) / 'n841.dtb'
        staged.write_bytes(dtb)
        wifi = subprocess.check_output(
            ['fdtget', str(staged), '/aliases', 'wifi0'], text=True).strip()
        if not wifi.startswith('/'):
            raise SystemExit('invalid wifi0 alias in DTB')
        for name, blob in (('local-mac-address', keys['WMac'][:6]),
                           ('brcm,cal-blob', keys['WCAL'])):
            subprocess.run(['fdtput', '-t', 'bx', str(staged), wifi, name,
                            *(f'{byte:02x}' for byte in blob)], check=True)
        subprocess.run(['fdtput', '-t', 's', str(staged), wifi,
                        'apple,antenna-sku', sku], check=True)
        bt = subprocess.check_output(
            ['fdtget', str(staged), '/aliases', 'bluetooth0'], text=True).strip()
        if not bt.startswith('/'):
            raise SystemExit('invalid bluetooth0 alias in DTB')
        # Linux local-bd-address uses least-significant byte first; Apple's
        # syscfg BMac is in display order, as in the A13 packer.
        subprocess.run(['fdtput', '-t', 'bx', str(staged), bt,
                        'local-bd-address',
                        *(f'{byte:02x}' for byte in keys['BMac'][:6][::-1])],
                       check=True)
        dtb = staged.read_bytes()
if board == 'n841':
    extra = 'cpufreq.off=1 pcie_aspm=off brcmfmac.debug=0x108000'
else:
    extra = 'cma=64M'
bootargs = ('chosen.bootargs=earlycon console=tty0 keep_bootcon ignore_loglevel '
            f'loglevel=8 rdinit=/init panic=-1 {extra}\n').encode()
raw = out / 'm1n1-linux.macho'
with raw.open('wb') as f:
    f.write(parts[0].read_bytes())
    f.write(bootargs)
    for p in parts[1:]: f.write(dtb if p == parts[1] else p.read_bytes())
    f.write(b'\0\0\0\0')
wrapped = out / 'm1n1-linux.img4'
ticket = pathlib.Path(os.environ.get('XR_IM4M', str(a12 / f'boot/extracted{sub}/apticket.im4m')))
if not ticket.is_file():
    raise SystemExit(f'missing XR IM4M: {ticket}; set XR_IM4M=/path/to/apticket.im4m')
subprocess.run(['img4','-i',str(raw),'-o',str(wrapped),'-A','-T','rkrn','-M',str(ticket)],check=True)
verify = out / '.verify-linux.raw'
subprocess.run(['img4','-i',str(wrapped),'-o',str(verify)],check=True,stdout=subprocess.DEVNULL)
if verify.read_bytes() != raw.read_bytes(): raise SystemExit('IMG4 payload verification failed')
manifest = {
    'bootargs': bootargs.decode().strip(),
    'raw_size': raw.stat().st_size,
    'raw_sha256': hashlib.sha256(raw.read_bytes()).hexdigest(),
    'img4_sha256': hashlib.sha256(wrapped.read_bytes()).hexdigest(),
    'parts': {p.name: hashlib.sha256(dtb if p == parts[1] else p.read_bytes()).hexdigest()
              for p in parts},
    'syscfg_wifi_injected': bool(syscfg_path),
    'syscfg_bt_address_injected': bool(syscfg_path),
}
(out/'linux-bootset.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(json.dumps(manifest,indent=2))
