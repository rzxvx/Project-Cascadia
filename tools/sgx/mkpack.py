#!/usr/bin/env python3
"""mkpack.py -- build sgx2d + SuperTux-on-the-GPU for the device, in one go.

    KC841=path/to/kc841.macho tools/sgx/mkpack.py [OUTDIR] [--install [HOST]]

1. the template capture: logs/ios/mod/gt_m_blend.bin and gtm.out (iOS,
   `gltrace mod`), split into its regions and payload if not done yet
2. the kernel's render-target data for a 768x1024 frame: the kext's own code
   run under unicorn (tools/iosgpu/rtemu.py; needs the user's decrypted 8.4.1
   kernelcache, KC841)
3. the template pack (tools/sgx/rpack.py)
4. libsgxsdl.so and the demos, built for armv7/musl in the cascadia-armdev
   Docker image (tools/sgx/lib/Dockerfile), and supertux.sh
5. --install: copied to HOST (default root@10.55.0.2) under
   /usr/local/lib/sgx2d, with /usr/local/bin/supertux-gpu

OUTDIR (default build/sgx2d) holds data derived from Apple's driver and
shaders: it stays out of git, like the captures it is made from.
"""
import argparse, os, re, struct, subprocess, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
CAP = os.path.join(ROOT, 'logs', 'ios', 'mod')
LIB = os.path.join(ROOT, 'tools', 'sgx', 'lib')
W, H = 768, 1024
RT_GPU_BASE = '0x87c00000'
DEVICE_FILES = ('libsgxsdl.so', 'sprites', 'demo2', 'supertux.sh')

def run(cmd, **kw):
    print('+', ' '.join(cmd))
    subprocess.check_call(cmd, **kw)

def split_capture():
    """gt_m_blend.bin -> blend/r_<cpu>.bin; gtm.out -> blend_payload.txt"""
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
        open(pay, 'w').write(' '.join(m.group(1).split()) + '\n')
    return out, pay

def main():
    ap = argparse.ArgumentParser(description='build sgx2d and SuperTux-on-the-GPU')
    ap.add_argument('outdir', nargs='?', default=os.path.join(ROOT, 'build', 'sgx2d'))
    ap.add_argument('--install', nargs='?', const='root@10.55.0.2', metavar='HOST',
                    help='copy to HOST (default root@10.55.0.2) as supertux-gpu')
    a = ap.parse_args()
    out, install = os.path.abspath(a.outdir), a.install
    kc = os.environ.get('KC841')
    if not kc or not os.path.exists(kc):
        sys.exit('mkpack: set KC841 to the decrypted 8.4.1 kernelcache')
    capdir, pay = split_capture()
    emu = os.path.join(out, 'rtemu')
    os.makedirs(emu, exist_ok=True)
    w = open(pay).read().split()
    w[44], w[45] = '%08x' % W, '%08x' % H
    open(os.path.join(emu, 'payload.txt'), 'w').write(' '.join(w) + '\n')
    env = dict(os.environ, RT_GPU_BASE=RT_GPU_BASE)
    run([sys.executable, os.path.join(ROOT, 'tools', 'iosgpu', 'rtemu.py'), str(W), str(H),
         '1', '2', 'payload.txt'], cwd=emu, env=env, stdout=subprocess.DEVNULL)
    pack = os.path.join(out, 'pack')
    run([sys.executable, os.path.join(ROOT, 'tools', 'sgx', 'rpack.py'), capdir, emu, pack],
        stdout=subprocess.DEVNULL)
    if subprocess.call(['docker', 'image', 'inspect', 'cascadia-armdev'],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL):
        run(['docker', 'build', '--platform', 'linux/arm/v7', '-t', 'cascadia-armdev', LIB])
    run(['docker', 'run', '--rm', '--platform', 'linux/arm/v7', '-v', LIB + ':/w', '-w', '/w',
         'cascadia-armdev', 'make', 'all'])
    for f in DEVICE_FILES:
        run(['cp', os.path.join(LIB, f), pack])
    print('pack:', pack)
    if install:
        tar = subprocess.Popen(['tar', 'cf', '-', '-C', pack, '.'], stdout=subprocess.PIPE)
        run(['ssh', install,
             'rm -rf /usr/local/lib/sgx2d && mkdir -p /usr/local/lib/sgx2d /usr/local/bin && '
             'tar xf - -C /usr/local/lib/sgx2d && '
             'ln -sf /usr/local/lib/sgx2d/supertux.sh /usr/local/bin/supertux-gpu && '
             'echo installed: supertux-gpu'], stdin=tar.stdout)
        tar.wait()

if __name__ == '__main__':
    main()
