#!/usr/bin/env python3
"""Exercise actual bootstrap command/order/error guards with no mounts/devices.

Replace only its BusyBox execution boundary, terminal message destination,
PID1 exec boundary and infinite rescue loop. Real mounts/switch_root and the
USB rescue transport remain hardware-unverified; this checks control flow.
"""
import os
from pathlib import Path
import subprocess
import tempfile

source=(Path(__file__).resolve().parents[1]/'rootfs/k93/squash-init').read_text()
source=source.replace('bb() { /bin/busybox "$@"; }',r'''bb() {
    printf '%s\n' "$*" >>"$K93_MOCK_LOG"
    if [ "$*" = "$K93_FAIL" ]; then return 1; fi
    if [ "$*" = 'test -f /root-transfer-network' ]; then
        [ "$K93_MODE" = network ]; return $?
    fi
    if [ "$*" = 'cat /root-run-limit' ]; then printf '%s\n' "$K93_RUN_LIMIT"; fi
    return 0
}''')
start=source.index('rescue() {');end=source.index('\ntrap rescue EXIT',start)
source=source[:start]+'rescue() { trap - EXIT INT TERM; exit 77; }'+source[end:]
source=source.replace('>/newroot/dev/tty0','>/dev/null')
source=source.replace('>/run/root.squashfs','>/dev/null')
source=source.replace('exec /bin/busybox switch_root /newroot /k93-init',
                      'bb switch_root /newroot /k93-init\ntrap - EXIT INT TERM')
with tempfile.TemporaryDirectory(prefix='k93-squash-init-test-') as directory:
    log=Path(directory)/'commands'
    def run(failure,mode='embedded',limit='96'):
        if log.exists():log.unlink()
        env=dict(os.environ,K93_MOCK_LOG=str(log),K93_FAIL=failure,K93_MODE=mode,K93_RUN_LIMIT=limit)
        result=subprocess.run(['sh','-s'],input=source,text=True,env=env)
        return result.returncode,log.read_text().splitlines()
    status,commands=run('')
    assert status==0
    assert commands[-1]=='switch_root /newroot /k93-init'
    checksum=commands.index('sha256sum -c /root.squashfs.sha256')
    loop=commands.index('losetup -r /dev/loop0 /root.squashfs')
    squash=commands.index('mount -t squashfs -o ro /dev/loop0 /run/k93-lower')
    overlay=next(i for i,c in enumerate(commands) if c.startswith('mount -t overlay '))
    assert checksum<loop<squash<overlay
    assert 'mount -t tmpfs -o size=64m,nr_inodes=8192 tmpfs /run/k93-write' in commands
    assert commands.index('mount --move /run /newroot/run')<len(commands)-1
    failures=[commands[i] for i in [3,checksum,loop,squash,overlay]]
    failures+=['test ! -e /sys/block/loop0/loop/backing_file','mount --move /dev /newroot/dev']
    for failure in failures:
        status,observed=run(failure)
        assert status==77,(failure,status)
        assert observed==commands[:commands.index(failure)+1],failure
    status,network=run('',mode='network')
    assert status==0
    receiver='timeout 80 /bin/busybox nc -l -n -p 48932 -s 10.93.0.2 -w 75 10.93.0.1'
    checksum='sha256sum -c /root.squashfs.sha256'
    loop='losetup -r /dev/loop0 /run/root.squashfs'
    assert network.index(receiver)<network.index(checksum)<network.index(loop)
    assert 'mount -t tmpfs -o size=96m,nr_inodes=2048 tmpfs /run' in network
    assert 'ip address add 10.93.0.2/30 dev usb0' in network
    status,larger=run('',mode='network',limit='192')
    assert status==0 and 'mount -t tmpfs -o size=192m,nr_inodes=2048 tmpfs /run' in larger
    for bad in ['','0','193','96; echo bad']:
        status,observed=run('',mode='network',limit=bad)
        assert status==77 and observed[-1]=='cat /root-run-limit'
    for failure in [receiver,checksum,loop]:
        status,observed=run(failure,mode='network')
        assert status==77 and observed==network[:network.index(failure)+1]
print('PASS: actual bootstrap command order, hash/board/loop guards and fail-closed rescue; no hardware')
