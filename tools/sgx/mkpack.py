#!/usr/bin/env python3
"""mkpack.py -- build sgx2d + SuperTux-on-the-GPU for the device, and install it.

    ./cascadia gpu [--install [HOST] | --root [DIR]]      (the usual way)

    tools/sgx/mkpack.py                  build, inside the cascadia-build image
    tools/sgx/mkpack.py --install [HOST] then install, on the host
    tools/sgx/mkpack.py --root [DIR]

Building:
1. the template capture: logs/ios/mod/gt_m_blend.bin and gtm.out (from the
   iPad's iOS, ./cascadia gpucap): the regions and payload the pack is made
   of, found and brought to the reference layout (capture-layout.json)
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
import argparse, hashlib, io, json, os, platform, re, shutil, struct, subprocess, sys, tarfile, time

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

def capture():
    """logs/ios/mod -> build/sgx2d/capture: the five regions the pack is made
    of, as r_<cpu>.bin under the reference capture's names, and the render
    command's payload.  iOS places its GL buffers differently from run to run
    (and ASLR moves them in the process), so each region is found by its
    contents with the pointer words masked, and the pointers are set to the
    reference layout's (tools/sgx/capture-layout.json); the result is checked
    against that layout's hashes."""
    for f in ('gt_m_blend.bin', 'gtm.out'):
        if not os.path.exists(os.path.join(CAP, f)):
            fail('no logs/ios/mod/%s -- capture it from the iPad\'s iOS first: '
                 './cascadia gpucap (docs/GPU.md)' % f)
    L = json.load(open(os.path.join(ROOT, 'tools', 'sgx', 'capture-layout.json')))
    unknown = ('\n  This capture is not laid out like any the pack knows.  Please report it,\n'
               '  with logs/ios/mod/gtm.out (a text log, no Apple data in it).')
    sha = lambda b: hashlib.sha256(b).hexdigest()
    def patched(b, patch, zero=False):
        b = bytearray(b)
        for o, w in patch.items():
            struct.pack_into('<I', b, int(o, 16), 0 if zero else int(w, 16))
        return bytes(b)
    d = open(os.path.join(CAP, 'gt_m_blend.bin'), 'rb').read()
    regs, o = [], 0
    while o < len(d):
        lo, hi = struct.unpack_from('<II', d, o)
        regs.append(d[o + 8:o + 8 + hi - lo])
        o += 8 + hi - lo
    out = os.path.join(OUT, 'capture')
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    for name, r in L['regions'].items():
        hit = [b for b in regs if len(b) == r['size'] and sha(patched(b, r['patch'], True)) == r['masked']]
        if not hit:
            fail('no %s region in logs/ios/mod/gt_m_blend.bin' % name + unknown)
        b = patched(hit[0], r['patch'])
        if sha(b) != r['sha256']:
            fail('the %s region does not match after relocation' % name + unknown)
        open(os.path.join(out, 'r_%s.bin' % r['cpu']), 'wb').write(b)
    t = open(os.path.join(CAP, 'gtm.out')).read()
    m = re.search(r'== mod_blend: render command at \S+, header[^\n]*\n   payload:\n'
                  r'((?:   [0-9a-f ]+\n)+)', t)
    if not m:
        fail('logs/ios/mod/gtm.out has no mod_blend render command -- recapture')
    w = m.group(1).split()
    P = L['payload']
    z = ' '.join('0' * 8 if str(i) in P['patch'] else x for i, x in enumerate(w))
    if len(w) != P['words'] or sha(z.encode()) != P['masked']:
        fail('the render command differs' + unknown)
    for i, x in P['patch'].items():
        w[int(i)] = x
    if sha(' '.join(w).encode()) != P['sha256']:
        fail('the render command does not match after relocation' + unknown)
    pay = os.path.join(out, 'payload.txt')
    open(pay, 'w').write(' '.join(w) + '\n')
    return out, pay

def build_pack(pack):
    kc = os.environ.get('KC841') or KC
    if not os.path.exists(kc):
        fail('no decrypted kernelcache at %s -- run ./cascadia firmware '
             '(or set KC841=)' % os.path.relpath(kc, ROOT))
    capdir, pay = capture()
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
