#!/usr/bin/env python3
"""Convert an already selectively extracted 21B91 Wi-Fi tree and BlueTool.

No OS image decompression or firmware installation occurs here. Mount/extract
read-only with bounded memory first; do not run standalone lzfse on the OS DMG.
"""
import argparse
import subprocess
import urllib.request
from device_common import A12, profile, sha, run


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--wifi-dir',required=True)
    parser.add_argument('--bluetool',required=True)
    a=parser.parse_args();p=profile.paths('ipad8')
    out=p['inputs']/'radios';out.mkdir(parents=True,exist_ok=True)
    run('python3',A12/'vendor/a13-tools/brcmfw.py',a.wifi_dir,out,'magnoliaa')
    cache=A12/'build/tools/hcdpack';cache.mkdir(parents=True,exist_ok=True)
    src=cache/'hcdpack.c'
    expected='ad2f5f370501ac59cd822468c064b56a7b332dcdbbf68a67e01c1ed02e75d4ef'
    if not src.is_file() or sha(src)!=expected:
        url='https://raw.githubusercontent.com/corellium/projectsandcastle/03db9c6ae04141eb940f3b9f56d446f50d57fadf/hcdpack/hcdpack.c'
        with urllib.request.urlopen(url,timeout=30) as f:src.write_bytes(f.read(1024*1024))
        if sha(src)!=expected:raise ValueError('hcdpack source hash mismatch')
    run('cc','-O2',src,'-o',cache/'hcdpack')
    dst=out/'bluetooth-candidates';dst.mkdir(exist_ok=True)
    hashes={'m':'ab1b27474c02f57b314059456a69269864eccf6cb8ab62346fc277e4bcf7b247',
            'u':'e6671cf600d52ec50e9e261a72d27a2e398b4bdbc8fd16f9b55d32ee2011261f'}
    for vendor,label in [('m','MUR'),('u','USI')]:
        hcd=dst/('BCM4355C1.MagnoliaA.'+label+'.hcd')
        run(cache/'hcdpack',a.bluetool,'C-4355__s-C1','MagnoliaA','V-'+vendor,hcd,stdout=subprocess.DEVNULL)
        if sha(hcd)!=hashes[vendor]:raise ValueError('wrong 21B91 Bluetooth firmware candidate')
    print('Prepared J171 radio candidates; select vendor only after OTP identification')


if __name__=='__main__':main()
