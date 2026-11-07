#!/usr/bin/env python3
"""Host-only guards: no firmware fetching, USB, build tree or NFS changes."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

A12=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(A12/'boot'))
from device_common import profile, write_manifest, verify_manifest
spec=importlib.util.spec_from_file_location('boot_device',A12/'boot/boot-device.py')
boot=importlib.util.module_from_spec(spec);spec.loader.exec_module(boot)


def syscfg(model):
    keys={'RMd#':model.encode(),'WMac':bytes.fromhex('020000000001'),
          'BMac':bytes.fromhex('020000000002'),'WCAL':b'cal', 'WSKU':b'12345678DRDO\0\0\0\0'}
    data=bytearray(131072)
    struct.pack_into('<4sIIIII',data,0,b'gfCS',len(data),len(data),2,0,len(keys))
    for i,(k,v) in enumerate(keys.items()):
        off=24+i*20;data[off:off+20]=k[::-1].encode()+v.ljust(16,b'\0')
    return data


class Separation(unittest.TestCase):
    def setUp(self):
        self.env=patch.dict(os.environ,{},clear=True);self.env.start()
    def tearDown(self):self.env.stop()
    def test_aliases(self):
        self.assertEqual(profile.selected('n841'),'xr')
        self.assertEqual(profile.selected('j171a'),'ipad8')
        with self.assertRaises(ValueError):profile.selected('ipad7')
    def test_conflicting_environment_rejected(self):
        os.environ.update(A12_DEVICE='xr',A12_PROFILE='ipad8')
        with self.assertRaises(ValueError):profile.paths()
    def test_private_and_output_paths_disjoint(self):
        a,b=profile.paths('xr'),profile.paths('ipad8')
        for name in ['inputs','extracted','payload','boot','touch','nfsroot','syscfg']:
            self.assertNotEqual(a[name],b[name])
        self.assertEqual(a['board'],'n841');self.assertEqual(b['board'],'j171a')
    def test_shell_selection(self):
        for device in ['xr','ipad8']:
            result=subprocess.check_output(['bash','-c',f'source "{A12}/profile.sh"; printf "%s" "$A12_DEVICE/$A12_BOARD"'],env={'PATH':os.environ.get('PATH','/usr/bin:/bin'),'A12_DEVICE':device},text=True)
            self.assertEqual(result,device+'/'+profile.paths(device)['board'])
    def test_frozen_dependencies(self):
        m=json.loads((A12/'vendor/DEPENDENCIES.json').read_text())
        self.assertEqual(len(m['files']),14)
        for f in m['files']:
            self.assertEqual(hashlib.sha256((A12/f['destination']).read_bytes()).hexdigest(),f['sha256'])
    def test_syscfg_wrong_device_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/'syscfg';p.write_bytes(syscfg('A2270'))
            profile.validate_syscfg(p,'ipad8')
            with self.assertRaises(ValueError):profile.validate_syscfg(p,'xr')
            p.write_bytes(syscfg('A2105'));profile.validate_syscfg(p,'xr')
            with self.assertRaises(ValueError):profile.validate_syscfg(p,'ipad8')
    def test_usb_board_guard(self):
        text='PRODUCT: iPhone11,8\nCPID: 0x8020\nBDID: 0x0c\nMODE: DFU\nPWND: usbliter8\n'
        boot.check_identity(text,profile.paths('xr'),'DFU')
        with self.assertRaises(ValueError):boot.check_identity(text,profile.paths('ipad8'),'DFU')
        with self.assertRaises(ValueError):boot.check_identity(text.replace('PWND: usbliter8',''),profile.paths('xr'),'DFU')
    def test_manifest_guards(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);f=root/'linux.img4';f.write_bytes(b'test')
            p=profile.paths('xr');manifest=root/'linux.json'
            write_manifest(manifest,p,[f]);verify_manifest(manifest,p)
            with self.assertRaises(ValueError):verify_manifest(manifest,profile.paths('ipad8'))
            f.write_bytes(b'changed')
            with self.assertRaises(ValueError):verify_manifest(manifest,p)
    def test_boot_upload_order_without_usb(self):
        # Execute the real orchestration with transfers/USB discovery mocked.
        # This is a host test of sequencing, not a device boot result.
        import usb.core
        for device in ('xr','ipad8'):
            with tempfile.TemporaryDirectory() as d:
                p=profile.paths(device);root=Path(d);p['boot']=root
                p['nfsroot']=root/'nfsroot';p['nfsroot'].write_text('172.16.42.2:/srv/test-root\n')
                fields='PRODUCT: '+p['product']+'\nCPID: '+p['cpid']+'\nBDID: '+p['bdid']+'\n'
                calls=[]
                first={'serial':False,'artifacts':{'SmartIO.img4':'test'} if device=='ipad8' else {}}
                linux={'nfsroot':'172.16.42.2:/srv/test-root'}
                with patch.object(boot.profile,'paths',return_value=p), \
                     patch.object(boot,'verify_manifest',side_effect=[first,linux]), \
                     patch.object(usb.core,'find',return_value=[object()]), \
                     patch.object(boot.subprocess,'check_output',side_effect=[fields+'MODE: DFU\nPWND: usbliter8\n',*([fields+'MODE: Recovery\n']*(3 if device=='xr' else 1))]), \
                     patch.object(boot,'run',side_effect=lambda *a,**k:calls.append(tuple(map(str,a)))), \
                     patch.object(boot.time,'sleep') as sleep, \
                     patch.object(sys,'argv',['boot-device.py']):
                    boot.main()
                uploads=[Path(c[2]).name for c in calls if c[:2]==('irecovery','-f')]
                expected=['DeviceTree.img4']+(['SmartIO.img4'] if device=='ipad8' else [])+['RestoreTrustCache.img4','linux.img4']
                self.assertEqual(uploads,expected)
                self.assertEqual([c.args[0] for c in sleep.call_args_list],
                                 [0.5] if device=='xr' else [1,1,1,1])

    def test_no_dynamic_a13_dependencies(self):
        for f in A12.rglob('*'):
            rel=f.relative_to(A12)
            if any(x in rel.parts for x in ['build','inputs','extracted','logs','vendor','tests','__pycache__']):continue
            if f.is_file() and f.suffix in ('.sh','.py'):
                text=f.read_text()
                for line in text.splitlines():
                    if line.lstrip().startswith('#'):continue
                    self.assertNotIn('/a13/tools/',line,str(rel))
                    self.assertNotIn('"a13/tools"',line,str(rel))
                    self.assertNotIn('/a13/m1n1/',line,str(rel))


if __name__=='__main__':unittest.main()
