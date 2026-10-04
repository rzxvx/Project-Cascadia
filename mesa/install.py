#!/usr/bin/env python3
"""install.py -- put the Mesa mesa/build.sh built onto the iPad.

    ./cascadia mesa [--install [HOST] | --root [DIR]]     (the usual way)

    mesa/install.py --auto       the running device's NFS root over ssh, else
                                 this host's exported root (what ./cascadia
                                 mesa does)
    mesa/install.py --install [HOST]   over ssh (default root@10.55.0.2)
    mesa/install.py --root [DIR]       into a root filesystem tree

Installs /usr/local/lib/sgx-mesa (libEGL, libGLESv2, libgbm, libgallium,
glclear, gltri) and /usr/local/bin/sgx-gl, which runs a program with this Mesa
instead of the system's.  Where things go is decided as tools/sgx/mkpack.py
decides it for SuperTux, with its helpers.
"""
import argparse, io, os, subprocess, sys, tarfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, 'tools', 'sgx'))
import mkpack  # noqa: E402  (probe, ssh, default_root, sudo_for, is_root, DEVICE)

PREFIX = 'usr/local/lib/sgx-mesa'
LAUNCHER = 'usr/local/bin/sgx-gl'
BUILT = os.path.join(ROOT, 'build', 'mesa', 'install', PREFIX)


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
    print('\nOn the iPad: sgx-gl glclear 100      (sgx-gl glclear 1 --fb to see it)')
    print('             sgx-gl gltri          (sgx-gl gltri --fb to see it)')


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
