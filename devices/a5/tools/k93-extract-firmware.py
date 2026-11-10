#!/usr/bin/env python3
"""Extract public stock K93 firmware for offline analysis; never contacts USB.

    python3 tools/k93-extract-firmware.py [IPSW]

IPSW defaults to iPad2,1_8.4.1_12H321_Restore.ipsw in devices/a5 or the
repository's top.  Into build/k93/firmware-12H321/:

    <name>             the stock img3 as it is in the IPSW
    <name>.dec         its decrypted payload
    kernelcache.12H321.k93.macho
                       the kernelcache, decrypted and decompressed: what the
                       K93 drivers are read out of (SDIO, multitouch, PMU,
                       audio), as the P105's were out of its own
    adt.txt            the DeviceTree as text (src/dtparse.py)
    manifest.json      sizes and hashes of the plaintexts
"""
from pathlib import Path
import hashlib
import io
import json
import struct
import sys
import zipfile
from contextlib import redirect_stdout
from Crypto.Cipher import AES

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
sys.path.insert(0, str(ROOT / 'src'))
from img3decrypt import parse_img3
from dtparse import parse_node, print_node

IPSW_NAME = 'iPad2,1_8.4.1_12H321_Restore.ipsw'

# Public firmware decryption material, not device/user credentials.
# https://theapplewiki.com/wiki/Keys:Donner_12H321_(iPad2,1), and the same
# values in tihmstar's fwkeydb (keys/firmware/iPad2,1/0x8940/12H321).
PARTS = {
    'DeviceTree.k93ap.img3': ('5bffad9ad82108c5ae3d38150dc56aea', '12433a4c85ec86ac6e14884f01661e2c5de43adee0323d1e88d4edd8d1ae9fda'),
    'iBSS.k93.RELEASE.dfu': ('781b9672a86ba1b41f8b7fa0af714c94', 'bbd7bf676dbcc6ba93c76d496b7af39ae7772eaaad2ec9fb71dc1fd004827784'),
    'iBEC.k93.RELEASE.dfu': ('83220a369c9525f3febb3edd331c65fb', '179027ef30b5008882d6dbd723ceb50c39cba40bb33ba620b71bcbc53ff2d424'),
    'kernelcache.release.k93': ('38d12734c845d1dcdb84846caf619da7', '6dea51edc4c6f6c205e25ca1d9af5b9ee7167ed5685a937ad892e7fcbfb3e615'),
}


def lzss(comp, outlen):
    """Apple's complzss: 4 KiB window, 18-byte matches, flag bit 1 = literal
    (the same decoder as scripts/extract-sgx-firmware.py)."""
    N, F, THR = 4096, 18, 2
    text = bytearray(N)
    r = N - F
    out = bytearray()
    si, n = 0, len(comp)
    flags = 0
    while len(out) < outlen and si < n:
        flags >>= 1
        if not flags & 0x100:
            flags = comp[si] | 0xFF00
            si += 1
        if flags & 1:
            c = comp[si]
            si += 1
            out.append(c)
            text[r] = c
            r = (r + 1) & (N - 1)
        else:
            if si + 1 >= n:
                break
            i = comp[si] | ((comp[si + 1] & 0xF0) << 4)
            j = (comp[si + 1] & 0x0F) + THR
            si += 2
            for k in range(j + 1):
                c = text[(i + k) & (N - 1)]
                out.append(c)
                text[r] = c
                r = (r + 1) & (N - 1)
    return bytes(out[:outlen])


def find_ipsw():
    if len(sys.argv) > 1:
        return Path(sys.argv[1])
    for c in (ROOT / IPSW_NAME, ROOT.parents[1] / IPSW_NAME):
        if c.is_file():
            return c
    raise SystemExit(f'no {IPSW_NAME} in {ROOT} or {ROOT.parents[1]} -- pass its path')


def main():
    out = ROOT / 'build/k93/firmware-12H321'
    out.mkdir(parents=True, exist_ok=True)
    records = {}
    with zipfile.ZipFile(find_ipsw()) as archive:
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
                text = io.StringIO()
                with redirect_stdout(text):
                    print_node(node, '/')
                (out / 'adt.txt').write_text(text.getvalue())
            if name.startswith('kernelcache'):
                if plain[:8] != b'complzss':
                    raise RuntimeError('kernelcache did not decrypt to complzss')
                # +0x0c is the uncompressed size (+0x10 the compressed one)
                outlen = struct.unpack_from('>I', plain, 0x0C)[0]
                macho = lzss(plain[0x180:], outlen)
                if struct.unpack_from('<I', macho, 0)[0] != 0xFEEDFACE:
                    raise RuntimeError('kernelcache is not a 32-bit Mach-O')
                (out / 'kernelcache.12H321.k93.macho').write_bytes(macho)
                print(f'Kernelcache: {len(macho)} bytes, Mach-O')
            (out / name).write_bytes(raw)
            (out / (name + '.dec')).write_bytes(plain)
            records[name] = {'archive_member': entries[0], 'size': len(plain), 'sha256': hashlib.sha256(plain).hexdigest()}
    (out / 'manifest.json').write_text(json.dumps(records, indent=2) + '\n')
    print(f'Extracted {len(records)} components to {out}')

if __name__ == '__main__':
    main()
