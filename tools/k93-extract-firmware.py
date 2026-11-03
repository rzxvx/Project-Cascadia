#!/usr/bin/env python3
"""Extract public stock K93 firmware for offline analysis; never contacts USB."""
from pathlib import Path
import hashlib
import json
import sys
import zipfile
from Crypto.Cipher import AES

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
sys.path.insert(0, str(ROOT / 'src'))
from img3decrypt import parse_img3
from dtparse import parse_node

# Public firmware decryption material, not device/user credentials.
# https://theapplewiki.com/wiki/Keys:Donner_12H321_(iPad2,1)
PARTS = {
    'DeviceTree.k93ap.img3': ('5bffad9ad82108c5ae3d38150dc56aea', '12433a4c85ec86ac6e14884f01661e2c5de43adee0323d1e88d4edd8d1ae9fda'),
    'iBSS.k93.RELEASE.dfu': ('781b9672a86ba1b41f8b7fa0af714c94', 'bbd7bf676dbcc6ba93c76d496b7af39ae7772eaaad2ec9fb71dc1fd004827784'),
    'iBEC.k93.RELEASE.dfu': ('83220a369c9525f3febb3edd331c65fb', '179027ef30b5008882d6dbd723ceb50c39cba40bb33ba620b71bcbc53ff2d424'),
}

def main():
    out = ROOT / 'build/k93/firmware-12H321'
    out.mkdir(parents=True, exist_ok=True)
    records = {}
    with zipfile.ZipFile(ROOT / 'iPad2,1_8.4.1_12H321_Restore.ipsw') as archive:
        for name, (iv, key) in PARTS.items():
            entries = [n for n in archive.namelist() if Path(n).name == name]
            if len(entries) != 1:
                raise RuntimeError(f'Expected exactly one {name}')
            raw = archive.read(entries[0])
            cipher, size = parse_img3(raw)
            plain = AES.new(bytes.fromhex(key), AES.MODE_CBC, bytes.fromhex(iv)).decrypt(cipher)[:size]
            if name.startswith('DeviceTree'):
                node, end = parse_node(plain, 0)
                if end != len(plain):
                    raise RuntimeError('DeviceTree has unexpected trailing bytes')
                props = node['props']
                model = props.get('model', b'').rstrip(b'\0').decode()
                if model != 'iPad2,1':
                    raise RuntimeError(f'Unexpected board {model!r}')
                print(f'Validated Apple DeviceTree: {model}, {end} bytes')
            (out / name).write_bytes(raw)
            (out / (name + '.dec')).write_bytes(plain)
            records[name] = {'archive_member': entries[0], 'size': len(plain), 'sha256': hashlib.sha256(plain).hexdigest()}
    (out / 'manifest.json').write_text(json.dumps(records, indent=2) + '\n')
    print(f'Extracted {len(records)} components to {out}')

if __name__ == '__main__':
    main()
