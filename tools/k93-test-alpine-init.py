#!/usr/bin/env python3
"""Exercise actual stage2 setup with command mocks; no mount/device access."""
import os
from pathlib import Path
import subprocess
import tempfile

source=(Path(__file__).resolve().parents[1]/'rootfs/k93/alpine-init').read_text()
source=source[:source.index('while :;')].replace('>/dev/tty0','>/dev/null')
fixture=r'''
record() { printf '%s\n' "$*" >>"$K93_TEST_LOG"; }
mkdir() { record mkdir "$@"; }
mount() { record mount "$@"; }
busybox() { record busybox "$@"; }
uname() { :; }
'''
with tempfile.TemporaryDirectory(prefix='k93-stage2-test-') as directory:
    log=Path(directory)/'commands'
    subprocess.run(['/bin/sh','-s'],input=fixture+source,text=True,check=True,
                   env=dict(os.environ,K93_TEST_LOG=str(log)),timeout=5)
    commands=log.read_text().splitlines()
    assert 'mkdir -p /dev/pts /dev/shm /tmp' in commands
    assert 'mount -t devpts devpts /dev/pts' in commands
    assert 'mount -t tmpfs -o size=32m,nr_inodes=4096 tmpfs /tmp' in commands
    assert 'mount -t tmpfs -o size=32m,nr_inodes=2048,mode=1777 tmpfs /dev/shm' in commands
    assert 'busybox ip link set lo up' in commands
    assert 'busybox hostname -F /etc/hostname' in commands
    assert all(not c.startswith('mount ') or ' -t tmpfs ' in c or ' -t devpts ' in c for c in commands)
print('PASS: actual stage2 commands bound shared RAM and enable loopback; no real mounts, devices or network changes')
