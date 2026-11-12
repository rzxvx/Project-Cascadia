#!/usr/bin/env python3
"""Package only selected-device outputs; never fall back to legacy boot/inputs."""
import argparse
import gzip
import json
import subprocess
import tempfile
from pathlib import Path
from device_common import A12, profile, sha, run, wrap, write_manifest, verify_manifest


def cpio_files(archive):
    data = gzip.decompress(Path(archive).read_bytes())
    i = 0
    while i+110 <= len(data):
        h = data[i:i+110]
        if h[:6] != b'070701': raise ValueError('invalid initramfs newc header')
        v = [int(h[6+j*8:14+j*8], 16) for j in range(13)]
        name = data[i+110:i+110+v[11]-1].decode().removeprefix('./')
        b = (i+110+v[11]+3)&~3
        if b+v[6] > len(data): raise ValueError('truncated cpio entry')
        if name == 'TRAILER!!!': return
        yield name, data[b:b+v[6]]
        i = (b+v[6]+3)&~3
    raise ValueError('initramfs has no trailer')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device')
    args=parser.parse_args(); p=profile.paths(args.device)
    verify_manifest(p['boot']/'bootset.json',p)
    kernel=A12/'build/payload/Image.gz'
    m1n1=p['payload']/'m1n1-idevice.macho'
    dtb=p['payload']/('t8020-'+p['board']+'.dtb')
    init=p['payload']/'initramfs.cpio.gz'
    expected='apple,'+p['board']
    compatibles=subprocess.check_output(['fdtget','-t','s',str(dtb),'/','compatible'],text=True).split()
    if expected not in compatibles: raise ValueError('wrong Linux DTB for selected device')
    files=dict(cpio_files(init))
    if files.get('etc/cascadia-device',b'').strip()!=p['name'].encode():
        raise ValueError('initramfs profile mismatch or absent profile stamp; rebuild selected initramfs')
    other=[q['board'] for q in profile.PROFILES.values() if q['board'] != p['board']]
    if any(n.startswith('lib/firmware/apple/'+board) for n in files for board in other):
        raise ValueError('initramfs contains the other device touch firmware')
    nfs=files.get('etc/nfsroot',b'').decode().strip()
    configured_nfs=p['nfsroot'].read_text().strip() if p['nfsroot'].is_file() else ''
    if nfs!=configured_nfs or (not nfs and p['name']!='xsmax'):
        raise ValueError('initramfs NFS path is stale; rebuild selected initramfs')
    keys=profile.validate_syscfg(p['syscfg'],p['name']) if p['name'] != 'xsmax' else None
    out=p['boot']
    with tempfile.TemporaryDirectory(dir=out,prefix='.calibration-') as tmp:
        calibrated=Path(tmp)/'calibrated.dtb'
        if p['name']=='xsmax':
            calibrated.write_bytes(dtb.read_bytes())
        elif p['name']=='ipad8':
            run('python3',A12/'boot/calibrate-ipad8-radio.py','--syscfg',p['syscfg'],
                '--dtb',dtb,'--output',calibrated)
        else:
            calibrated.write_bytes(dtb.read_bytes())
            def alias(name):
                return subprocess.check_output(['fdtget','-t','s',str(calibrated),'/aliases',name],text=True).strip()
            def blob(node,prop,data):
                run('fdtput','-t','bx',calibrated,node,prop,*(f'{b:02x}' for b in data))
            wifi,bt=alias('wifi0'),alias('bluetooth0')
            if not wifi.startswith('/') or not bt.startswith('/'):raise ValueError('invalid DT aliases')
            blob(wifi,'local-mac-address',keys['WMac'][:6]);blob(wifi,'brcm,cal-blob',keys['WCAL'])
            blob(bt,'local-bd-address',keys['BMac'][:6][::-1])
            sku=keys['WSKU'][8:16].rstrip(b'\0').decode('ascii')
            if not sku or not sku.isalnum():raise ValueError('invalid XR antenna SKU')
            run('fdtput','-t','s',calibrated,wifi,'apple,antenna-sku',sku)
        extra = 'cma=64M' if p['name']=='xsmax' else 'cpufreq.off=1 pcie_aspm=off brcmfmac.debug=0x108000'
        args=('chosen.bootargs=earlycon console=tty0 keep_bootcon ignore_loglevel '
              f'loglevel=8 rdinit=/init panic=-1 {extra}\n').encode()
        raw=out/'linux.macho'
        raw.write_bytes(m1n1.read_bytes()+args+calibrated.read_bytes()+kernel.read_bytes()+init.read_bytes()+bytes(4))
        raw.chmod(0o600)
        wrap(raw,out/'linux.img4','rkrn',out/'ticket.im4m')
        (out/'linux.img4').chmod(0o600)
        write_manifest(out/'linux.json',p,[out/'linux.img4'],nfsroot=nfs,
            components={'kernel':sha(kernel),'m1n1':sha(m1n1),'initramfs':sha(init),
                        'calibrated_dtb':sha(calibrated)},hardware_status='not boot tested')
    print('Packaged '+p['label']+' in '+str(out))


if __name__=='__main__':main()
