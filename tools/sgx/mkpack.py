#!/usr/bin/env python3
"""mkpack.py -- build sgx2d + SuperTux-on-the-GPU for the device, and install it.

    ./cascadia gpu [--install [HOST] | --root [DIR]]      (the usual way)

    tools/sgx/mkpack.py                  build, inside the cascadia-build image
    tools/sgx/mkpack.py --install [HOST] then install, on the host
    tools/sgx/mkpack.py --root [DIR]

Building:
1. the template frame (frame.py): our USSE programs (programs.py), PDS
   programs (pds.py) and state, plus the one PDS template iOS's GL driver
   keeps, out of the IPSW (build/firmware/gl-event.pds)
2. the kernel's render-target data for a 768x1024 frame: the kext's own code
   run under unicorn (tools/iosgpu/rtemu.py) out of the decrypted 8.4.1
   kernelcache (build/firmware; KC841= overrides)
3. the template pack (tools/sgx/rpack.py)
4. libsgxsdl.so and the demos, cross-built against Alpine's armhf musl
   (tools/sgx/lib/cross.sh), and supertux.sh
The image has unicorn, capstone and the ARM cross compiler, so the host needs
nothing beyond docker and python3, and no ARM emulation.  Everything comes
from the IPSW and this repository: no capture from the device's iOS.

Installing, as /usr/local/lib/sgx2d and /usr/local/bin/supertux-gpu:
   --auto     the running device's NFS root (with apk add supertux there),
              else this host's exported root -- what ./cascadia gpu does
   --install  over ssh into the root the device is running now
              (HOST, default root@10.55.0.2)
   --root     into a root filesystem tree on this machine, such as the one
              ./cascadia nfs exports (DIR, default that one)

build/sgx2d holds data derived from Apple's kext and GL driver (from the
IPSW): it stays out of git, like the rest of build/.
"""
import argparse, io, os, platform, subprocess, sys, tarfile, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import frame

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
LIB = os.path.join(ROOT, 'tools', 'sgx', 'lib')
OUT = os.path.join(ROOT, 'build', 'sgx2d')
KC = os.path.join(ROOT, 'build', 'firmware', 'kernelcache.12H321.macho')
EVENT = os.path.join(ROOT, 'build', 'firmware', 'gl-event.pds')
W, H = 768, 1024
RT_GPU_BASE = '0x87c00000'
DEVICE_FILES = ('libsgxsdl.so', 'sprites', 'demo2', 'supertux.sh')
PREFIX = 'usr/local/lib/sgx2d'
LAUNCHER = 'usr/local/bin/supertux-gpu'
DEVICE = 'root@10.55.0.2'

def run(cmd, **kw):
    print('+', ' '.join(cmd))
    subprocess.check_call(cmd, **kw)

def fail(msg):
    sys.exit('mkpack: ' + msg)

def build_pack(pack):
    kc = os.environ.get('KC841') or KC
    for f in (kc, EVENT):
        if not os.path.exists(f):
            fail('no %s -- run ./cascadia firmware' % os.path.relpath(f, ROOT))
    capdir = os.path.join(OUT, 'frame')
    frame.write(capdir, EVENT)
    pay = os.path.join(capdir, 'payload.txt')
    emu = os.path.join(OUT, 'rtemu')
    os.makedirs(emu, exist_ok=True)
    w = open(pay).read().split()
    w[44], w[45] = '%08x' % W, '%08x' % H
    open(os.path.join(emu, 'payload.txt'), 'w').write(' '.join(w) + '\n')
    env = dict(os.environ, RT_GPU_BASE=RT_GPU_BASE, KC841=os.path.abspath(kc))
    run([sys.executable, os.path.join(ROOT, 'tools', 'iosgpu', 'rtemu.py'), str(W), str(H),
         '1', '2', 'payload.txt'], cwd=emu, env=env, stdout=subprocess.DEVNULL)
    run([sys.executable, os.path.join(ROOT, 'tools', 'sgx', 'rpack.py'), capdir, emu, pack],
        stdout=subprocess.DEVNULL)

def build_lib(pack):
    run(['bash', os.path.join(LIB, 'cross.sh')])
    for f in DEVICE_FILES:
        run(['cp', os.path.join(LIB, f), pack])

def tarball(pack):
    """/usr/local/lib/sgx2d and the supertux-gpu link, every entry root's"""
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode='w', format=tarfile.USTAR_FORMAT) as t:
        def own(ti):
            ti.uid = ti.gid = 0
            ti.uname = ti.gname = 'root'
            return ti
        t.add(pack, PREFIX, filter=own)
        ln = own(tarfile.TarInfo(LAUNCHER))
        ln.type, ln.linkname, ln.mode = tarfile.SYMTYPE, '/' + PREFIX + '/supertux.sh', 0o777
        ln.mtime = int(time.time())
        t.addfile(ln)
    return buf.getvalue()

def ssh(host, cmd, **kw):
    return subprocess.run(['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=4',
                           '-o', 'StrictHostKeyChecking=accept-new', host, cmd], **kw)

def probe(host):
    """(root filesystem type, has the GPU driver) of the running device, or None"""
    r = ssh(host, "mount -t debugfs none /sys/kernel/debug 2>/dev/null; "
                  "awk '$2==\"/\" {t=$3} END {printf \"%s \", t}' /proc/mounts; "
                  "test -e /sys/kernel/debug/apple-sgx/mem && echo sgx || echo -",
            capture_output=True, text=True)
    if r.returncode or len(r.stdout.split()) != 2:
        return None
    fs, sgx = r.stdout.split()
    return fs, sgx == 'sgx'

def supertux(host):
    """SuperTux itself, from Alpine, if the device does not have it yet"""
    if ssh(host, 'command -v supertux2 >/dev/null').returncode == 0:
        return
    print('==> SuperTux itself: apk add supertux, on the device')
    if ssh(host, 'apk update -q && apk add supertux').returncode:
        fail('apk could not fetch it.  The device reaches the internet through this\n'
             '  host: ./cascadia net on, then ./cascadia gpu again.')

def install_ssh(pack, host):
    # The device's live root: an NFS root keeps it, a RAM root loses it at reboot
    p = subprocess.Popen(['ssh', host, 'tar xf - -C / && echo "installed: /%s"' % LAUNCHER],
                         stdin=subprocess.PIPE)
    p.communicate(tarball(pack))
    if p.returncode:
        fail('install over ssh to %s failed' % host)
    supertux(host)

def auto(pack):
    """Where it can go: the running device's NFS root, over ssh (and SuperTux
    with it), or else the root this host exports."""
    dev = probe(DEVICE)
    root = default_root()
    have_root = is_root(root)
    if dev and not dev[1]:
        print('note: the running kernel has no GPU driver -- ./cascadia build, then flash')
    if dev and dev[0].startswith('nfs'):
        install_ssh(pack, DEVICE)
    elif have_root:
        install_root(pack, root)
        if dev:
            print('note: the device runs from RAM now; play once it boots from this root')
    elif dev:
        fail('the device runs from its RAM root, which has no room for SuperTux.\n'
             '  Boot it from NFS (./cascadia nfs on, then flash) and run ./cascadia gpu again.')
    else:
        fail('nowhere to install: no device at %s and no exported root at %s\n'
             '  (./cascadia nfs on).  Or name one: --install HOST / --root DIR.' % (DEVICE, root))
    print('\nOn the iPad: supertux-gpu')

def default_root():
    if os.environ.get('DST'):
        return os.environ['DST']
    if platform.system() == 'Darwin':
        return os.path.expanduser('~/cascadia-root')
    return '/srv/cascadia-root'

def sudo_for(root):
    """The exported root is root's, and on a Mac even its directories are
    0700: anything but a tree of one's own is read and written through sudo."""
    ok = all(os.access(os.path.join(root, d), os.R_OK | os.W_OK | os.X_OK)
             for d in ('', 'sbin', 'usr'))
    return [] if ok else ['sudo']

def in_root(root, path, sudo):
    return subprocess.call(sudo + ['test', '-e', os.path.join(root, path)]) == 0

def is_root(root):
    return os.path.isdir(root) and in_root(root, 'sbin/p105-stage2', sudo_for(root))

def install_root(pack, root):
    root = os.path.abspath(root)
    sudo = sudo_for(root)
    if not os.path.isdir(root) or not in_root(root, 'sbin/p105-stage2', sudo):
        fail('%s is not a cascadia root filesystem (no sbin/p105-stage2); '
             'give its path to --root' % root)
    p = subprocess.Popen(sudo + ['tar', 'xf', '-', '-C', root], stdin=subprocess.PIPE)
    p.communicate(tarball(pack))
    if p.returncode:
        fail('could not unpack into %s' % root)
    print('installed: %s/%s' % (root, LAUNCHER))
    if not in_root(root, 'usr/bin/supertux2', sudo):
        print('note: SuperTux itself is not in that root yet.  Run ./cascadia gpu again\n'
              '      with the device up (it adds it there), or: apk add supertux on it')

def main():
    ap = argparse.ArgumentParser(description='build sgx2d and SuperTux-on-the-GPU')
    to = ap.add_mutually_exclusive_group()
    to.add_argument('--install', nargs='?', const=DEVICE, metavar='HOST',
                    help='install the build over ssh (default %s)' % DEVICE)
    to.add_argument('--auto', action='store_true',
                    help='install where it fits: the running device, else this host\'s NFS root')
    to.add_argument('--root', nargs='?', const='', metavar='DIR',
                    help='install the build into a root filesystem tree (default: '
                         'this host\'s NFS root, %s)' % default_root())
    a = ap.parse_args()
    pack = os.path.join(OUT, 'pack')
    if a.install is None and a.root is None and not a.auto:
        build_pack(pack)
        build_lib(pack)
        print('built: %s' % os.path.relpath(pack, ROOT))
        return
    if not os.path.exists(os.path.join(pack, 'supertux.sh')):
        fail('nothing built in %s -- ./cascadia gpu builds it' % os.path.relpath(pack, ROOT))
    if a.auto:
        auto(pack)
    elif a.install:
        install_ssh(pack, a.install)
    else:
        install_root(pack, a.root or default_root())

if __name__ == '__main__':
    main()
