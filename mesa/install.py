#!/usr/bin/env python3
"""install.py -- put the Mesa mesa/build.sh built onto the iPad.

    ./cascadia mesa [--install [HOST] | --root [DIR]]     (the usual way)

    mesa/install.py --auto       the running device's NFS root over ssh, else
                                 this host's exported root (what ./cascadia
                                 mesa does)
    mesa/install.py --install [HOST]   over ssh (default root@10.55.0.2)
    mesa/install.py --root [DIR]       into a root filesystem tree

Installs /usr/local/lib/sgx-mesa (libGL, libEGL, libGLESv1_CM, libGLESv2,
libgbm, libgallium, the test programs) and makes it the system's OpenGL:
/etc/ld-musl-armhf.path puts its lib ahead of /usr/lib, so every program
loads this Mesa, not Alpine's (which stays installed; the file taken away,
it is back) -- and X's configuration for the GPU, /etc/X11/sgx.conf.d
(modesetting and glamor, which `desktop` takes when it is there).
/usr/local/bin/sgx-gl runs a program with another build
(SGX_MESA=dir).  MESA_BUILD=release installs the release build.  Where
things go is decided as tools/sgx/mkpack.py decides it for SuperTux, with
its helpers.
"""
import argparse, io, os, subprocess, sys, tarfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, 'tools', 'sgx'))
import mkpack  # noqa: E402  (probe, ssh, default_root, sudo_for, is_root, DEVICE)

PREFIX = 'usr/local/lib/sgx-mesa'
LAUNCHER = 'usr/local/bin/sgx-gl'
BUILT = os.path.join(ROOT, 'build', 'mesa',
                     'install-release' if os.environ.get('MESA_BUILD') == 'release' else 'install',
                     PREFIX)
# musl's library path (the default /lib:/usr/local/lib:/usr/lib), this
# Mesa's ahead of the system's
LD_PATH = 'etc/ld-musl-armhf.path'
LD_PATH_TEXT = '/lib\n/%s/lib\n/usr/local/lib\n/usr/lib\n' % PREFIX
# X on the GPU (M36): modesetting with glamor, a configuration directory of
# its own (`desktop` uses it; tools/desktop/xfce-setup.sh) -- and the driver
# where the X server's GLX looks for it (AIGLX: /usr/lib/dri)
X_CONF = 'etc/X11/sgx.conf.d/20-sgx.conf'
X_CONF_TEXT = '''Section "Device"
    Identifier "SGX543MP2"
    Driver     "modesetting"
    Option     "kmsdev" "/dev/dri/card0"
    Option     "AccelMethod" "glamor"
EndSection
'''
AIGLX = 'usr/lib/dri/apple_sgx_dri.so'


def fail(msg):
    sys.exit('mesa: ' + msg)


def tarball():
    """the prefix and the sgx-gl link, every entry root's"""
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode='w', format=tarfile.USTAR_FORMAT) as t:
        def own(ti):
            ti.uid = ti.gid = 0
            ti.uname = ti.gname = 'root'
            return ti
        t.add(BUILT, PREFIX, filter=own)
        ln = own(tarfile.TarInfo(LAUNCHER))
        ln.type, ln.linkname, ln.mode = tarfile.SYMTYPE, '/' + PREFIX + '/bin/sgx-gl', 0o777
        ln.mtime = int(time.time())
        t.addfile(ln)
        for name, text in ((LD_PATH, LD_PATH_TEXT), (X_CONF, X_CONF_TEXT)):
            f = own(tarfile.TarInfo(name))
            f.size, f.mode, f.mtime = len(text), 0o644, int(time.time())
            t.addfile(f, io.BytesIO(text.encode()))
        ln = own(tarfile.TarInfo(AIGLX))
        ln.type, ln.linkname, ln.mode = tarfile.SYMTYPE, '/' + PREFIX + '/lib/dri/apple_sgx_dri.so', 0o777
        ln.mtime = int(time.time())
        t.addfile(ln)
    return buf.getvalue()


def install_ssh(host):
    p = subprocess.Popen(['ssh', host, 'tar xf - -C / && echo "installed: /%s"' % PREFIX],
                         stdin=subprocess.PIPE)
    p.communicate(tarball())
    if p.returncode:
        fail('install over ssh to %s failed' % host)


def install_root(root):
    root = os.path.abspath(root)
    sudo = mkpack.sudo_for(root)
    if not mkpack.is_root(root):
        fail('%s is not a cascadia root filesystem (no sbin/p105-stage2)' % root)
    p = subprocess.Popen(sudo + ['tar', 'xf', '-', '-C', root], stdin=subprocess.PIPE)
    p.communicate(tarball())
    if p.returncode:
        fail('could not unpack into %s' % root)
    print('installed: %s/%s' % (root, PREFIX))


def auto():
    dev = mkpack.probe(mkpack.DEVICE)
    root = mkpack.default_root()
    if dev and dev[0].startswith('nfs'):
        install_ssh(mkpack.DEVICE)
    elif mkpack.is_root(root):
        install_root(root)
    elif dev:
        fail('the device runs from its RAM root, which has no room for Mesa.\n'
             '  Boot it from NFS (./cascadia nfs on, then flash) and run ./cascadia mesa again.')
    else:
        fail('nowhere to install: no device at %s and no exported root at %s\n'
             '  (./cascadia nfs on).  Or name one: --install HOST / --root DIR.'
             % (mkpack.DEVICE, root))
    print('\nOn the iPad every program now takes this Mesa: eglinfo -B, glclear 100,')
    print('             gltri --fb (to see it)')


def main():
    ap = argparse.ArgumentParser(description='install the sgx Mesa on the iPad')
    to = ap.add_mutually_exclusive_group(required=True)
    to.add_argument('--auto', action='store_true')
    to.add_argument('--install', nargs='?', const=mkpack.DEVICE, metavar='HOST')
    to.add_argument('--root', nargs='?', const='', metavar='DIR')
    a = ap.parse_args()
    if not os.path.exists(os.path.join(BUILT, 'bin', 'glclear')):
        fail('nothing built in %s -- ./cascadia mesa builds it' % os.path.relpath(BUILT, ROOT))
    if a.auto:
        auto()
    elif a.install:
        install_ssh(a.install)
    else:
        install_root(a.root or mkpack.default_root())


if __name__ == '__main__':
    main()
