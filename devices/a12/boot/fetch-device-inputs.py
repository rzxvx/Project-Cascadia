#!/usr/bin/env python3
"""Range-extract only the selected board's Apple boot components."""
import argparse
import plistlib
import re
from pathlib import Path
from device_common import profile, run, sha


def decode_component(source, staged, digest, p, name):
    run('img4', '-i', source, '-o', staged)
    if sha(staged) != digest and name == 'iBSS.raw':
        # Some builds (J171/21B91) encrypt iBSS. Lookup is constrained by the
        # already verified manifest's product/build; the plaintext pin remains
        # mandatory. Never reinterpret an encrypted blob as patchable iBoot.
        staged.unlink()
        run('ipsw', 'img4', 'im4p', 'extract', '--lookup',
            '--lookup-device', p['product'], '--lookup-build', p['build'],
            '--output', staged, source)
    if sha(staged) != digest:
        staged.unlink()
        raise ValueError(name+' plaintext hash mismatch; refusing another build or key')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device')
    args = parser.parse_args()
    p = profile.paths(args.device)
    out = p['extracted']
    out.mkdir(parents=True, exist_ok=True)
    download = out/'download'
    download.mkdir(exist_ok=True)
    members = ['BuildManifest.plist', p['ibss_member'], p['adt_member']]
    if p['smartio_member']:
        members.append(p['smartio_member'])
    pattern = '^('+'|'.join(re.escape(n) for n in members)+')$'
    run('ipsw', 'extract', '--remote', '--pattern', pattern, '-o', download, p['ipsw'])
    def one(member):
        matches = [x for x in download.rglob(Path(member).name)
                   if x.as_posix().endswith('/'+member)]
        if len(matches) != 1:
            raise ValueError('expected exactly one '+member)
        return matches[0]
    manifest = plistlib.loads(one('BuildManifest.plist').read_bytes())
    if manifest['ProductBuildVersion'] != p['build'] or p['product'] not in manifest['SupportedProductTypes']:
        raise ValueError('IPSW build/product mismatch')
    identities = [i for i in manifest['BuildIdentities']
                  if i.get('Info', {}).get('DeviceClass') == p['apple_board']]
    if not identities:
        raise ValueError('IPSW has no selected board identity')
    for identity in identities:
        for key, member in [('iBSS', p['ibss_member']), ('DeviceTree', p['adt_member'])]:
            if identity['Manifest'][key]['Info']['Path'] != member:
                raise ValueError('BuildManifest component path mismatch: '+key)
    for member, name, digest in [(p['ibss_member'], 'iBSS.raw', p['ibss_sha256']),
                                  (p['adt_member'], 'DeviceTree.raw', p['adt_sha256'])]:
        staged = out/(name+'.part')
        decode_component(one(member), staged, digest, p, name)
        staged.replace(out/name)
    if p['smartio_member']:
        src = one(p['smartio_member'])
        if sha(src) != p['smartio_im4p_sha256']:
            raise ValueError('SmartIO hash mismatch')
        run('img4', '-i', src, '-o', out/'SmartIO.raw')
        if sha(out/'SmartIO.raw') != p['smartio_raw_sha256']:
            raise ValueError('decoded SmartIO hash mismatch')
    print(p['label']+' boot inputs verified in '+str(out))


if __name__ == '__main__':
    main()
