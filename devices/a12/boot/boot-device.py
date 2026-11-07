#!/usr/bin/env python3
"""Identity-checked RAM boot for XR or iPad 8; serial capture is opt-in by manifest."""
import argparse
import os
import signal
import subprocess
import sys
import time
from pathlib import Path
from device_common import A12, profile, run, verify_manifest


def check_identity(text,p,mode):
    fields=dict(line.split(':',1) for line in text.splitlines() if ':' in line)
    fields={k.strip():v.strip() for k,v in fields.items()}
    for key,value in [('PRODUCT',p['product']),('CPID',p['cpid']),('BDID',p['bdid'])]:
        actual=fields.get(key,'')
        if key in ('CPID','BDID'):
            try: matches=int(actual,16)==int(value,16)
            except ValueError: matches=False
        else: matches=actual==value
        if not matches:raise ValueError('connected device does not match selected '+key)
    if fields.get('MODE')!=mode or (mode=='DFU' and not fields.get('PWND')):
        raise ValueError('expected '+mode+(' (pwned)' if mode=='DFU' else ''))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device')
    parser.add_argument('--radios',action='store_true')
    parser.add_argument('--verify-only',action='store_true')
    a=parser.parse_args();p=profile.paths(a.device)
    first=verify_manifest(p['boot']/'bootset.json',p)
    linux=verify_manifest(p['boot']/'linux.json',p)
    if p['name']=='ipad8' and 'SmartIO.img4' not in first['artifacts']:
        raise ValueError('J171 boot manifest lacks required SmartIO')
    if not p['nfsroot'].is_file() or p['nfsroot'].read_text().strip()!=linux['nfsroot']:
        raise ValueError('selected NFS setting changed since packing; rebuild initramfs and repack')
    if a.radios:
        if p['name']=='xr':
            folder=p['inputs']/'brcm'
            required=[folder/name for name in ['brcmfmac4357b1-pcie.apple,yodak.bin',
                'brcmfmac4357b1-pcie.apple,yodak-DRDO-u.txt',
                'brcmfmac4357b1-pcie.apple,yodak.clm_blob',p['bluetooth']]]
        else:
            vendor=os.environ.get('IPAD8_RADIO_VENDOR')
            if vendor not in ('m','u'):
                raise ValueError('set IPAD8_RADIO_VENDOR=m|u after OTP identification before boot')
            folder=p['inputs']/'radios'
            label='MUR' if vendor=='m' else 'USI'
            required=[folder/'bluetooth-candidates'/('BCM4355C1.MagnoliaA.'+label+'.hcd')]
            required += [folder/'brcm'/('brcmfmac4355c1-pcie.apple,magnoliaa'+ext)
                for ext in ['.bin','-YSBC-'+vendor+'.txt','.clm_blob','.txcap_blob']]
        if any(not f.is_file() or not f.stat().st_size for f in required):
            raise ValueError('selected radio firmware incomplete; prepare it before boot')
    # No USB interaction for artifact-only checks.

    if a.verify_only:
        print('Verified '+p['label']+' manifests; NFS '+linux['nfsroot']);return
    import usb.core
    attached=list(usb.core.find(find_all=True,idVendor=0x05ac,idProduct=0x1227))
    if len(attached)!=1:
        raise ValueError('expected exactly one Apple DFU device; refusing ambiguous USB target')
    text=subprocess.check_output(['irecovery','-q'],text=True,timeout=5)
    check_identity(text,p,'DFU')
    out=p['boot'];logdir=out/'logs'/time.strftime('%Y%m%d-%H%M%S',time.gmtime())
    logdir.mkdir(parents=True,exist_ok=False)
    # Capture only this profile's serial build; leave other sessions alone.
    if first['serial']:
        pidfile=out/'capture.pid'
        if pidfile.exists():
            pid=int(pidfile.read_text());cmd=Path('/proc')/str(pid)/'cmdline'
            if cmd.exists() and b'capture-dcsd.py' in cmd.read_bytes():os.kill(pid,signal.SIGTERM)
        with (logdir/'capture.stderr').open('ab') as err:
            child=subprocess.Popen([sys.executable,str(A12/'boot/capture-dcsd.py'),str(logdir/'dcsd.log')],
                stdin=subprocess.DEVNULL,stdout=err,stderr=err,start_new_session=True)
        pidfile.write_text(str(child.pid)+'\n')
    with (logdir/'transfers.log').open('w') as log:
        ctl=Path(os.environ.get('USBLITER8CTL',str(A12/'vendor/a13-tools/usbliter8ctl')))
        run(sys.executable,ctl,'boot',out/'iBSS.linux.raw',stdout=log,stderr=log)
        if p['name']=='xr':
            # Match the original boot-xr-linux.sh stabilization and timing.
            for attempt in range(50):
                try:
                    text=subprocess.check_output(['irecovery','-q'],text=True,stderr=subprocess.DEVNULL,timeout=2)
                    check_identity(text,p,'Recovery')
                    time.sleep(0.5)
                    text=subprocess.check_output(['irecovery','-q'],text=True,stderr=subprocess.DEVNULL,timeout=2)
                    check_identity(text,p,'Recovery')
                    break
                except (subprocess.SubprocessError,ValueError):time.sleep(0.2)
            text=subprocess.check_output(['irecovery','-q'],text=True,timeout=2)
            check_identity(text,p,'Recovery')
        else:
            for attempt in range(30):
                try:
                    text=subprocess.check_output(['irecovery','-q'],text=True,stderr=subprocess.DEVNULL,timeout=2)
                    check_identity(text,p,'Recovery');break
                except (subprocess.SubprocessError,ValueError):time.sleep(1)
            else:raise ValueError('selected device did not enter iBSS Recovery')
        files=[('DeviceTree.img4','devicetree')]
        if p['smartio_member']:files.append(('SmartIO.img4','firmware'))
        files += [('RestoreTrustCache.img4','firmware'),('linux.img4','bootx')]
        for name,command in files:
            run('irecovery','-f',out/name,stdout=log,stderr=log,timeout=60)
            run('irecovery','-c',command,stdout=log,stderr=log,timeout=10)
            if p['name']!='xr':time.sleep(1)
    print('Boot payload sent; capture: '+str(logdir))
    if not a.radios:return
    host=os.environ.get('A12_SSH_HOST','root@172.16.42.1')
    opts=['-F','/dev/null','-o','BatchMode=yes','-o','Tunnel=no','-o','ConnectTimeout=2',
          '-o','StrictHostKeyChecking=no','-o','UserKnownHostsFile=/dev/null']
    # Host address/DHCP and NFS export must be ready; do not reconfigure an
    # arbitrary host interface that might belong to another connected device.
    for attempt in range(60):
        command='test -f /etc/debian_version && tr "\\000" "\\n" < /proc/device-tree/compatible | grep -qx apple,'+p['board']
        try:
            if subprocess.run(['ssh',*opts,host,command],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=6).returncode==0:break
        except subprocess.TimeoutExpired:pass
        time.sleep(1 if p['name']=='xr' else 2)
    else:raise ValueError('selected Debian root not reachable; check host USB address and NFS')
    env=dict(os.environ,A12_DEVICE=p['name'],A12_PROFILE=p['name'],XR_SSH_HOST=host,IPAD8_SSH_HOST=host,
             A12_RADIO_DIR=str(p['inputs']/'brcm'),IPAD8_RADIO_DIR=str(p['inputs']/'radios'))
    if p['name']=='xr':
        for script in ['load-wifi-firmware.sh','load-bluetooth-firmware.sh']:
            run('bash',A12/'boot'/script,env=env)
    else:
        vendor=os.environ.get('IPAD8_RADIO_VENDOR')
        if vendor not in ('m','u'):raise ValueError('set IPAD8_RADIO_VENDOR=m|u only after verifying the module vendor from OTP')
        for mode in ['wifi','bluetooth']:run('bash',A12/'boot/load-ipad8-radio.sh',mode,vendor,env=env)


if __name__=='__main__':main()
