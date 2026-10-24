#!/usr/bin/env python3
"""Build/fully read back K93 SquashFS offline; no mounts, root or device access.

Host squashfs-tools is extracted locally, not installed. Its package digest
comes from a Packages index authenticated by Ubuntu's signed InRelease using
the existing build image's Ubuntu archive keyring. The Alpine tree is read-only.
"""
import argparse
import hashlib
import json
import lzma
from pathlib import Path
import re
import stat
import struct
import subprocess
import tempfile
import urllib.request

ROOT=Path(__file__).resolve().parents[1]
ARCHIVE='https://archive.ubuntu.com/ubuntu/'

def fetch(url,path):
    if not path.exists():
        with urllib.request.urlopen(url,timeout=60) as response:
            data=response.read(8*1024*1024)
        with path.open('xb') as output:output.write(data)
    return path.read_bytes()

def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream,'sha256').hexdigest()

def tree_inventory(root):
    result={}
    for p in sorted(root.rglob('*')):
        rel=p.relative_to(root).as_posix();mode=p.lstat().st_mode
        if stat.S_ISLNK(mode):result[rel]=['link',p.readlink().as_posix()]
        elif stat.S_ISREG(mode):result[rel]=['file',stat.S_IMODE(mode),p.stat().st_size,digest(p)]
        elif stat.S_ISDIR(mode):result[rel]=['dir',stat.S_IMODE(mode)]
        else:raise ValueError('Unexpected special file in userspace: '+rel)
    return result

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('tree',nargs='?',default='build/k93/alpine-gui-v2')
    parser.add_argument('--image-name',help='New output basename under build/k93; preserve existing image')
    parser.add_argument('--block-size',type=int,choices=[131072,1048576],default=131072)
    parser.add_argument('--arm-bcj',action='store_true',help='Use ARM/Thumb XZ filters supported by the K93 kernel')
    args=parser.parse_args()
    tree=(ROOT/args.tree).resolve()
    assert tree.is_relative_to(ROOT/'build/k93') and tree.is_dir()
    proof=tree.parent/(tree.name+'-manifest.json')
    source_manifest=json.loads(proof.read_text())
    assert source_manifest['board']=='K93AP' and source_manifest['architecture']=='armv7'
    assert digest(tree/'lib/apk/db/installed')==source_manifest['package_database_sha256']
    name=args.image_name or tree.name
    assert re.fullmatch(r'[a-zA-Z0-9_-]+',name)
    image=tree.parent/(name+'.squashfs')
    receipt=tree.parent/(name+'-squashfs.json')
    assert not image.exists() and not receipt.exists(),'Preserve existing image/receipt'
    cache=ROOT/'build/k93/squashfs-tools-host';cache.mkdir(exist_ok=True)
    release=cache/'InRelease';index=cache/'Packages.xz'
    release_data=fetch(ARCHIVE+'dists/jammy/InRelease',release)
    docker=['docker','run','--rm','--pull','never','--network','none',
            '--cap-drop','ALL','--security-opt','no-new-privileges','--user','1000:1000',
            '-v',f'{ROOT}:/work','-w','/work','cascadia-k93-build']
    subprocess.run(docker+['gpgv','--keyring','/usr/share/keyrings/ubuntu-archive-keyring.gpg',
                          '/work/'+release.relative_to(ROOT).as_posix()],check=True)
    sha_section=release_data.decode().split('\nSHA256:\n',1)[1].split('\nSHA512:',1)[0]
    matches=re.findall(r'^ ([a-f0-9]{64}) +(\d+) +main/binary-amd64/Packages.xz$',sha_section,re.M)
    assert len(matches)==1
    index_data=fetch(ARCHIVE+'dists/jammy/main/binary-amd64/Packages.xz',index)
    assert len(index_data)==int(matches[0][1]) and hashlib.sha256(index_data).hexdigest()==matches[0][0]
    packages=[]
    for paragraph in lzma.decompress(index_data).decode().split('\n\n'):
        record=dict(l.split(': ',1) for l in paragraph.splitlines() if ': ' in l and not l.startswith(' '))
        if record.get('Package') in ('squashfs-tools','liblzo2-2') and record.get('Architecture')=='amd64':packages.append(record)
    assert len(packages)==2 and {p['Package'] for p in packages}=={'squashfs-tools','liblzo2-2'}
    for package in packages:
        filename=package['Filename']
        assert re.fullmatch(r'pool/main/[a-z0-9/-]+/[a-zA-Z0-9_.+~-]+\.deb',filename)
        deb=cache/filename.rsplit('/',1)[1]
        data=fetch(ARCHIVE+filename,deb)
        assert len(data)==int(package['Size']) and hashlib.sha256(data).hexdigest()==package['SHA256']
        extracted=cache/('extracted' if package['Package']=='squashfs-tools' else 'extracted-liblzo2-2')
        if not extracted.exists():
            extracted.mkdir()
            subprocess.run(docker+['dpkg-deb','--extract','/work/'+deb.relative_to(ROOT).as_posix(),
                                  '/work/'+extracted.relative_to(ROOT).as_posix()],check=True)
        # Never execute cached tools/libraries unless they still match the
        # authenticated deb. No package scripts or system install commands.
        with tempfile.TemporaryDirectory(prefix='k93-squash-tools-',dir='/var/tmp') as temp:
            unpack=Path(temp)
            subprocess.run(['bsdtar','-xf',str(deb),'-C',str(unpack),'data.tar.zst'],check=True)
            with tempfile.TemporaryDirectory(prefix='k93-squash-deb-',dir='/var/tmp') as temp2:
                subprocess.run(['bsdtar','-xf',str(unpack/'data.tar.zst'),'-C',temp2],check=True)
                assert tree_inventory(extracted)==tree_inventory(Path(temp2))
    tools=cache/'extracted'
    candidates=list((cache/'extracted-liblzo2-2').rglob('liblzo2.so.2'))
    assert len(candidates)==1
    library='/work/'+candidates[0].parent.relative_to(ROOT).as_posix()
    docker[2:2]=['-e','LD_LIBRARY_PATH='+library]
    before=tree_inventory(tree)
    binary='/work/'+tools.relative_to(ROOT).as_posix()+'/usr/bin/'
    filters=['-Xbcj','arm,armthumb'] if args.arm_bcj else []
    subprocess.run(docker+[binary+'mksquashfs','/work/'+tree.relative_to(ROOT).as_posix(),
                          '/work/'+image.relative_to(ROOT).as_posix(),'-noappend','-all-root',
                          '-no-xattrs','-comp','xz','-b',str(args.block_size),*filters,'-processors','2','-no-progress'],check=True)
    with image.open('rb') as stream:header=stream.read(96)
    assert header[:4]==b'hsqs' and struct.unpack_from('<H',header,20)[0]==4
    assert struct.unpack_from('<I',header,12)[0]==args.block_size
    assert struct.unpack_from('<H',header,28)[0]==4
    assert struct.unpack_from('<Q',header,40)[0]<=image.stat().st_size
    with tempfile.TemporaryDirectory(prefix='k93-squash-readback-',dir='/var/tmp') as temp:
        # No mount privileges: unsquashfs reads every file and metadata into a
        # disposable, task-created directory, then compare hashes and links.
        verify_docker=docker.copy();verify_docker[2:2]=['-v',f'{temp}:/verify']
        subprocess.run(verify_docker+[binary+'unsquashfs','-no-progress','-processors','2',
                                     '-d','/verify/root','/work/'+image.relative_to(ROOT).as_posix()],check=True)
        assert tree_inventory(Path(temp)/'root')==before,'Full image readback differs'
    assert tree_inventory(tree)==before,'Source changed during compression'
    result={'board':'K93AP','source_manifest_sha256':digest(proof),
            'source_tree_inventory_sha256':hashlib.sha256(json.dumps(before,sort_keys=True).encode()).hexdigest(),
            'image_sha256':digest(image),'image_bytes':image.stat().st_size,
            'compression':'xz','block_size':args.block_size,'arm_bcj':args.arm_bcj,'all_owner_root':True,
            'host_packages':{p['Package']:{'sha256':p['SHA256'],'version':p['Version']} for p in packages},
            'full_readback_matches':True,'source_unchanged':True,'hardware_tested':False}
    with receipt.open('x') as output:json.dump(result,output,indent=2);output.write('\n')
    print(json.dumps(result,indent=2))

if __name__=='__main__':main()
