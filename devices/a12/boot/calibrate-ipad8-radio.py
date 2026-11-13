#!/usr/bin/env python3
"""Inject private J171a SysCfg radio data into a separate DTB; never log values."""
import argparse
import importlib.util
from pathlib import Path
import subprocess
import tempfile


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--syscfg', type=Path, required=True)
    p.add_argument('--dtb', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    if args.output.exists() or args.output.resolve() == args.dtb.resolve():
        p.error('output must be a new file; refusing to overwrite input or existing DTB')
    parser = Path(__file__).resolve().parents[1] / 'vendor/a13-tools/syscfg.py'
    spec = importlib.util.spec_from_file_location('private_syscfg', parser)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    data = args.syscfg.read_bytes()
    if len(data) != 131072 or data[:4] != b'gfCS':
        p.error('invalid SysCfg length or magic')
    keys = module.parse(data)
    if keys.get('RMd#', b'').rstrip(b'\0') != b'A2270':
        p.error('SysCfg is not identified as A2270 / J171a')
    for key in ('WMac', 'BMac'):
        mac = keys.get(key, b'')[:6]
        if len(mac) != 6 or not any(mac) or mac[0] & 1:
            p.error(f'invalid {key}')
    if not 1 <= len(keys.get('WCAL', b'')) <= 4096:
        p.error('missing or oversized WCAL')
    def get(tree, node, prop):
        return subprocess.check_output(['fdtget', '-t', 's', str(tree), node, prop], text=True).strip()
    def blob(tree, node, prop, value):
        subprocess.run(['fdtput', '-t', 'bx', str(tree), node, prop,
                        *(f'{byte:02x}' for byte in value)], check=True)
    if 'apple,j171a' not in get(args.dtb, '/', 'compatible').split():
        p.error('input DTB is not J171a')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=args.output.parent, prefix='.j171-radio-') as tmp:
        staged = Path(tmp) / 'calibrated.dtb'
        staged.write_bytes(args.dtb.read_bytes())
        staged.chmod(0o600)
        wifi = get(staged, '/aliases', 'wifi0')
        bt = get(staged, '/aliases', 'bluetooth0')
        if not wifi.startswith('/') or not bt.startswith('/'):
            p.error('invalid radio aliases')
        blob(staged, wifi, 'local-mac-address', keys['WMac'][:6])
        blob(staged, wifi, 'brcm,cal-blob', keys['WCAL'])
        blob(staged, bt, 'local-bd-address', keys['BMac'][:6][::-1])
        # Unlike the XR capture, this iPad SysCfg has no WSKU. Never invent it.
        sku = keys.get('WSKU', b'')[8:16].rstrip(b'\0')
        if sku:
            text = sku.decode('ascii')
            if not text.isalnum():
                p.error('invalid antenna SKU')
            subprocess.run(['fdtput', '-t', 's', str(staged), wifi,
                            'apple,antenna-sku', text], check=True)
        # Exclusive creation also protects against concurrent overwrite.
        with args.output.open('xb') as output:
            args.output.chmod(0o600)
            output.write(staged.read_bytes())
    print('J171a radio calibration saved privately; MACs and calibration not printed.')
    print('Module vendor still requires OTP identification; SysCfg is not vendor proof.')


if __name__ == '__main__':
    main()
