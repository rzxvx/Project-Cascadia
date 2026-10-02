#!/usr/bin/env python3
"""mkpack.py -- build sgx2d + SuperTux-on-the-GPU for the device, and install it.

    ./cascadia gpu [--install [HOST] | --root [DIR]]      (the usual way)

    tools/sgx/mkpack.py                  build, inside the cascadia-build image
    tools/sgx/mkpack.py --install [HOST] then install, on the host
    tools/sgx/mkpack.py --root [DIR]

Building:
1. the template capture: logs/ios/mod/gt_m_blend.bin and gtm.out (from the
   iPad's iOS, ./cascadia gpucap), split into its regions and payload
2. the kernel's render-target data for a 768x1024 frame: the kext's own code
   run under unicorn (tools/iosgpu/rtemu.py) out of the decrypted 8.4.1
   kernelcache (./cascadia firmware leaves it in build/firmware; KC841=
   overrides)
3. the template pack (tools/sgx/rpack.py)
4. libsgxsdl.so and the demos, cross-built against Alpine's armhf musl
   (tools/sgx/lib/cross.sh), and supertux.sh
The image has unicorn, capstone and the ARM cross compiler, so the host needs
nothing beyond docker and python3, and no ARM emulation.

Installing, as /usr/local/lib/sgx2d and /usr/local/bin/supertux-gpu:
   --install  over ssh into the root the device is running now
              (HOST, default root@10.55.0.2)
   --root     into a root filesystem tree on this machine, such as the one
              ./cascadia nfs exports (DIR, default that one)

build/sgx2d holds data derived from Apple's driver and shaders: it stays out
of git, like the captures it is made from.
"""
import argparse, io, os, platform, re, struct, subprocess, sys, tarfile, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
CAP = os.path.join(ROOT, 'logs', 'ios', 'mod')
LIB = os.path.join(ROOT, 'tools', 'sgx', 'lib')
OUT = os.path.join(ROOT, 'build', 'sgx2d')
KC = os.path.join(ROOT, 'build', 'firmware', 'kernelcache.12H321.macho')
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

def split_capture():
    """gt_m_blend.bin -> blend/r_<cpu>.bin; gtm.out -> blend_payload.txt"""
    for f in ('gt_m_blend.bin', 'gtm.out'):
        if not os.path.exists(os.path.join(CAP, f)):
            fail('no logs/ios/mod/%s -- capture it from the iPad\'s iOS first: '
                 './cascadia gpucap (docs/GPU.md)' % f)
    out = os.path.join(CAP, 'blend')
    if not os.path.isdir(out):
        os.makedirs(out)
        d = open(os.path.join(CAP, 'gt_m_blend.bin'), 'rb').read()
        o = 0
        while o < len(d):
            lo, hi = struct.unpack_from('<II', d, o)
            o += 8
            open(os.path.join(out, 'r_%08x.bin' % lo), 'wb').write(d[o:o + hi - lo])
            o += hi - lo
    pay = os.path.join(CAP, 'blend_payload.txt')
    if not os.path.exists(pay):
        t = open(os.path.join(CAP, 'gtm.out')).read()
        m = re.search(r'== mod_blend: render command at \S+, header[^\n]*\n   payload:\n'
                      r'((?:   [0-9a-f ]+\n)+)', t)
        if not m:
            fail('logs/ios/mod/gtm.out has no mod_blend render command -- recapture')
        open(pay, 'w').write(' '.join(m.group(1).split()) + '\n')
    return out, pay

def build_pack(pack):
    kc = os.environ.get('KC841') or KC
    if not os.path.exists(kc):
        fail('no decrypted kernelcache at %s -- run ./cascadia firmware '
             '(or set KC841=)' % os.path.relpath(kc, ROOT))
    capdir, pay = split_capture()
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

def install_ssh(pack, host):
    # The device's live root: an NFS root keeps it, a RAM root loses it at reboot
    sh = ('tar xf - -C / && echo "installed: /%s" && '
          '{ command -v supertux2 >/dev/null || '
          'echo "note: SuperTux itself is not installed there: apk add supertux"; }' % LAUNCHER)
    p = subprocess.Popen(['ssh', host, sh], stdin=subprocess.PIPE)
    p.communicate(tarball(pack))
    if p.returncode:
        fail('install over ssh to %s failed' % host)

def default_root():
    if os.environ.get('DST'):
        return os.environ['DST']
    if platform.system() == 'Darwin':
        return os.path.expanduser('~/cascadia-root')
    return '/srv/cascadia-root'

def install_root(pack, root):
    root = os.path.abspath(root)
    if not os.path.exists(os.path.join(root, 'sbin', 'p105-stage2')):
        fail('%s is not a cascadia root filesystem (no sbin/p105-stage2); '
             'give its path to --root' % root)
    # The Linux export is root's (it is written through no_root_squash)
    sudo = [] if os.access(os.path.join(root, 'usr'), os.W_OK) else ['sudo']
    p = subprocess.Popen(sudo + ['tar', 'xf', '-', '-C', root], stdin=subprocess.PIPE)
    p.communicate(tarball(pack))
    if p.returncode:
        fail('could not unpack into %s' % root)
    print('installed: %s/%s' % (root, LAUNCHER))
    if not os.path.exists(os.path.join(root, 'usr', 'bin', 'supertux2')):
        print('note: SuperTux itself is not in that root: apk add supertux on the device')

def main():
    ap = argparse.ArgumentParser(description='build sgx2d and SuperTux-on-the-GPU')
    to = ap.add_mutually_exclusive_group()
    to.add_argument('--install', nargs='?', const=DEVICE, metavar='HOST',
                    help='install the build over ssh (default %s)' % DEVICE)
    to.add_argument('--root', nargs='?', const='', metavar='DIR',
                    help='install the build into a root filesystem tree (default: '
                         'this host\'s NFS root, %s)' % default_root())
    a = ap.parse_args()
    pack = os.path.join(OUT, 'pack')
    if a.install is None and a.root is None:
        build_pack(pack)
        build_lib(pack)
        print('built: %s' % os.path.relpath(pack, ROOT))
        return
    if not os.path.exists(os.path.join(pack, 'supertux.sh')):
        fail('nothing built in %s -- ./cascadia gpu builds it' % os.path.relpath(pack, ROOT))
    if a.install:
        install_ssh(pack, a.install)
    else:
        install_root(pack, a.root or default_root())

if __name__ == '__main__':
    main()
