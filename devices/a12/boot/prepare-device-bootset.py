#!/usr/bin/env python3
"""Prepare a strictly board-specific first stage with Linux-only boot args."""
import argparse
import os
import shutil
import sys
from pathlib import Path
from device_common import A12, profile, run, sha, wrap, write_manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device')
    parser.add_argument('--ticket', type=Path)
    serial = parser.add_mutually_exclusive_group()
    serial.add_argument('--serial', dest='serial', action='store_true')
    serial.add_argument('--no-serial', dest='serial', action='store_false')
    parser.set_defaults(serial=True)
    args = parser.parse_args()
    p = profile.paths(args.device)
    ticket = args.ticket or Path(os.environ.get('IM4M', str(p['inputs']/'apticket.im4m')))
    if not ticket.is_file():
        parser.error('provide IM4M or --ticket for the selected device')
    for name, digest in [('iBSS.raw', p['ibss_sha256']), ('DeviceTree.raw', p['adt_sha256'])]:
        if sha(p['extracted']/name) != digest:
            raise ValueError('wrong '+name+' for selected device')
    out = p['boot']; out.mkdir(parents=True, exist_ok=True)
    staged = out/'.iBSS.pristine.raw'
    shutil.copyfile(p['extracted']/'iBSS.raw', staged)
    bootargs = '-v wdt=-1'+(' serial=3' if args.serial else '')
    options = ['--any-im4m'] if p['name'] == 'xsmax' else []
    run(sys.executable, A12/'boot/patch_iboot.py', staged, bootargs,
        '--serial' if args.serial else '--no-serial', *options)
    generated = Path(str(staged)+'.patched')
    if generated.read_bytes().count((bootargs+'\0').encode()) != 1:
        raise ValueError('Linux iBSS bootargs were not installed exactly once')
    generated.replace(out/'iBSS.linux.raw'); staged.unlink()
    shutil.copyfile(ticket, out/'ticket.im4m')
    shutil.copyfile(p['extracted']/'DeviceTree.raw', out/'DeviceTree.raw')
    wrap(out/'DeviceTree.raw', out/'DeviceTree.img4', 'rdtr', ticket)
    (out/'RestoreTrustCache.raw').write_bytes(bytes(4))
    wrap(out/'RestoreTrustCache.raw', out/'RestoreTrustCache.img4', 'rtsc', ticket)
    files=[out/x for x in ['iBSS.linux.raw', 'ticket.im4m', 'DeviceTree.raw', 'DeviceTree.img4', 'RestoreTrustCache.img4']]
    if p['smartio_member']:
        # Compare decoded payload with the pinned, Apple-distributed IM4P.
        firmware = p['extracted']/'SmartIO.raw'
        if not firmware.is_file() or sha(firmware) != p['smartio_raw_sha256']:
            raise ValueError('missing or incorrect J171 SmartIO firmware')
        wrap(firmware, out/'SmartIO.img4', 'siof', ticket)
        files.append(out/'SmartIO.img4')
    if p.get('gfx_ipsw'):
        gfx = p['extracted']/'armfw_g11p.im4p'
        if sha(gfx) != p['gfx_im4p_sha256']:
            raise ValueError('incorrect XS Max GPU firmware')
        run('img4', '-i', gfx, '-o', out/'gfx.img4', '-M', ticket)
        files.append(out/'gfx.img4')
    write_manifest(out/'bootset.json', p, files, serial=args.serial, ibss_bootargs=bootargs)
    print('Prepared '+p['label']+' (Apple ADT pristine; Linux iBSS arguments only)')


if __name__ == '__main__':
    main()
