#!/usr/bin/env python3
"""One device selection for A12 tools; no filesystem or USB side effects."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import shlex
import sys

A12 = Path(__file__).resolve().parent
PROFILES = json.loads((A12 / 'profiles.json').read_text())


def canonical(value):
    for name, p in PROFILES.items():
        if value in p['aliases']:
            return name
    raise ValueError('device must be xr, ipad8 or xsmax (n841/j171a/d331p)')


def selected(device=None):
    choices = [v for v in (device, os.environ.get('A12_DEVICE'),
               os.environ.get('A12_PROFILE'), os.environ.get('A12_BOARD')) if v]
    names = {canonical(v) for v in choices}
    if len(names) > 1:
        raise ValueError('A12 device/profile/board selectors disagree; refusing mixed profiles')
    return names.pop() if names else 'xr'



def paths(device=None):
    name = selected(device)
    p = dict(PROFILES[name])
    p.update(name=name, inputs=A12/'boot/inputs'/name,
             extracted=A12/'boot/extracted'/name,
             payload=A12/'build/devices'/name/'payload',
             boot=A12/'build/devices'/name/'boot',
             touch=A12/'build/devices'/name/'touch',
             nfsroot=A12/'build/devices'/name/'nfsroot')
    p['syscfg'] = Path(os.environ.get('SYSCFG', str(p['inputs']/'syscfg.bin')))
    return p


def validate_syscfg(path, device=None):
    p = paths(device)
    data = Path(path).read_bytes()
    if len(data) != 131072 or data[:4] != b'gfCS':
        raise ValueError('SysCfg size/magic mismatch')
    spec = importlib.util.spec_from_file_location('a12_syscfg', A12/'vendor/a13-tools/syscfg.py')
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    keys = mod.parse(data)
    model = keys.get('RMd#', b'').rstrip(b'\0').decode('ascii', errors='strict')
    if model not in p['models']:
        raise ValueError('SysCfg model does not match selected device; values withheld')
    for key in ('WMac', 'BMac'):
        mac = keys.get(key, b'')[:6]
        if len(mac) != 6 or not any(mac) or mac[0] & 1:
            raise ValueError('invalid ' + key)
    if not 1 <= len(keys.get('WCAL', b'')) <= 4096:
        raise ValueError('missing or oversized WCAL')
    if p['name'] == 'xr' and len(keys.get('WSKU', b'')) < 16:
        raise ValueError('XR SysCfg requires WSKU')
    return keys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device')
    parser.add_argument('--shell', action='store_true')
    parser.add_argument('--check-syscfg', type=Path)
    args = parser.parse_args()
    p = paths(args.device)
    if args.check_syscfg:
        validate_syscfg(args.check_syscfg, p['name'])
        print('SysCfg matches selected device; private values withheld')
    elif args.shell:
        values = {f'A12_{k.upper()}': str(p[k]) for k in
                  ('board', 'label', 'product', 'cpid', 'bdid', 'inputs', 'extracted', 'payload', 'boot', 'touch', 'nfsroot', 'syscfg', 'build', 'ipsw', 'touch_tag', 'touch_personality')}
        values['A12_DEVICE'] = p['name']
        values['A12_PROFILE'] = p['name']
        for k, v in values.items():
            print('export ' + k + '=' + shlex.quote(v))
    else:
        print(json.dumps(p, default=str, indent=2))


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError) as e:
        sys.exit(str(e))
