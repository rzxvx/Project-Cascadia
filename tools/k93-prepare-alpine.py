#!/usr/bin/env python3
"""Prepare an isolated K93 armv7 Alpine userspace; never boot or touch storage.

The official minirootfs is SHA256-pinned. apk itself verifies repository/package
signatures using the keys in that pinned rootfs. No --allow-untrusted, package
scripts, host chroot, new image pull, privileged container or P105 overlay.
"""
import argparse
import hashlib
import json
import posixpath
from pathlib import Path
import shutil
import subprocess
import tarfile
import urllib.request

ROOT=Path(__file__).resolve().parents[1]
VERSION='3.24.2'
SHA256='5552f1ef2398cee46ef2cf733f8f1258f39a77ad065dfd47282fc0df9fe1976c'
URL=f'https://dl-cdn.alpinelinux.org/alpine/v3.24/releases/armv7/alpine-minirootfs-{VERSION}-armv7.tar.gz'
PACKAGES=['xorg-server','xf86-video-fbdev','xf86-input-evdev','xinit','xauth',
          'xterm','twm','font-misc-misc']
BROWSER_PACKAGES=['firefox-esr','xdotool','font-dejavu']

def rootfs_filter(member,destination):
    # Alpine's BusyBox applets use absolute symlinks. Rebase those inside
    # the tree BEFORE applying Python's traversal/escape protections.
    if member.issym() and member.linkname.startswith('/'):
        member=member.replace(linkname=posixpath.relpath(
            member.linkname.lstrip('/'),posixpath.dirname(member.name) or '.'))
    return tarfile.data_filter(member,destination)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',default='build/k93/alpine-gui-v1')
    parser.add_argument('--base-only',action='store_true')
    parser.add_argument('--browser',action='store_true',help='Include Firefox ESR, diagnostic X input and scalable fonts in a new tree')
    parser.add_argument('--finish-existing',action='store_true',help='Validate and finish an already prepared tree; never reinstall packages')
    args=parser.parse_args()
    assert not (args.base_only and args.browser),'Browser requires the graphical packages'
    requested=PACKAGES+(BROWSER_PACKAGES if args.browser else [])
    output=(ROOT/args.output).resolve()
    assert output.is_relative_to(ROOT/'build/k93') and output.parent.exists()
    assert output.exists()==args.finish_existing,'Preserve existing tree; use a new output directory or explicit --finish-existing'
    manifest_path=output.parent/(output.name+'-manifest.json')
    assert not manifest_path.exists(),'Preserve completed manifest'
    qemu=Path('/usr/bin/qemu-arm-static')
    assert qemu.is_file(),'Use the already installed ARM emulator'
    subprocess.run(['docker','image','inspect','cascadia-k93-build'],
                   stdout=subprocess.DEVNULL,check=True)
    cache=ROOT/'build/k93/alpine-downloads'
    cache.mkdir(exist_ok=True)
    archive=cache/URL.rsplit('/',1)[1]
    if not archive.exists():
        data=urllib.request.urlopen(URL,timeout=60).read(16*1024*1024)
        assert hashlib.sha256(data).hexdigest()==SHA256,'Wrong minirootfs digest'
        with archive.open('xb') as stream:stream.write(data)
    assert hashlib.sha256(archive.read_bytes()).hexdigest()==SHA256
    with tarfile.open(archive) as source:
        if not args.finish_existing:
            output.mkdir()
            source.extractall(output,filter=rootfs_filter)
        # Verify trust anchors against pinned official archive even on finish.
        for member in source.getmembers():
            if member.isfile() and member.name.lstrip('./').startswith('etc/apk/keys/'):
                assert (output/member.name).read_bytes()==source.extractfile(member).read()
    assert (output/'etc/alpine-release').read_text().strip()==VERSION
    assert (output/'etc/apk/arch').read_text().strip()=='armv7'
    # Run only inside the existing disposable build container, unprivileged.
    # qemu's -L supplies the target interpreter/libraries; apk --root confines
    # package changes to this new tree. No host binfmt registration or mounts.
    command=['docker','run','--rm','--pull','never','--cap-drop','ALL',
             '--security-opt','no-new-privileges','--user','1000:1000',
             '-v',f'{output}:/target',
             '-v',f'{qemu}:/usr/local/bin/qemu-arm-static:ro',
             'cascadia-k93-build','/usr/local/bin/qemu-arm-static','-L','/target',
             '/target/sbin/apk']
    if not args.base_only and not args.finish_existing:
        # This host's Docker DNS override is unreachable. Reuse the host's
        # declared resolver in its network namespace for this client only.
        # No daemon, listener, route or host resolver configuration is changed.
        resolvers=[l.split()[1] for l in Path('/etc/resolv.conf').read_text().splitlines()
                   if l.startswith('nameserver ')]
        assert resolvers,'No declared host DNS resolver'
        online=command[:3]+['--network','host','--dns',resolvers[0]]+command[3:]
        subprocess.run(online+['--root','/target','--arch','armv7','--usermode',
                                '--no-scripts','--no-cache','add',*requested],check=True)
    database=(output/'lib/apk/db/installed').read_bytes()
    installed={}
    for record in database.decode().split('\n\n'):
        fields=dict(line.split(':',1) for line in record.splitlines() if ':' in line)
        if 'P' in fields:
            assert fields.get('A') in ('armv7','noarch'),fields.get('A')
            installed[fields['P']]=fields.get('V')
    if not args.base_only:
        assert set(requested)<=installed.keys(),'Requested GUI/browser packages missing'
        for executable,flag in [('/usr/libexec/Xorg','-version'),('/usr/bin/xterm','-version')]:
            check=command.copy()
            check[3:3]=['--network','none']
            check[-1]='/target'+executable
            subprocess.run(check+[flag],check=True)
    # Keep the proven USB console/network RAM init. No storage mounts, sshd,
    # desktop autostart, Wi-Fi guesses or borrowed P105 initialization.
    shutil.copyfile(ROOT/'rootfs/k93/init',output/'init')
    (output/'init').chmod(0o755)
    files=[p for p in output.rglob('*') if p.is_file() and not p.is_symlink()]
    size=sum(p.stat().st_size for p in files)
    manifest={'board':'K93AP','architecture':'armv7','alpine':VERSION,
              'source':URL,'minirootfs_sha256':SHA256,
              'packages_requested':[] if args.base_only else requested,'installed_packages':installed,
              'browser_included':args.browser,'browser_hardware_tested':False,
              'regular_file_bytes':size,'regular_files':len(files),
              'install_scripts_run':False,'hardware_tested':False,
              'package_database_sha256':hashlib.sha256(database).hexdigest()}
    with manifest_path.open('x') as stream:
        json.dump(manifest,stream,indent=2);stream.write('\n')
    print(json.dumps(manifest,indent=2),flush=True)

if __name__=='__main__':main()
