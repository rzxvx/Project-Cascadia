#!/usr/bin/env python3
"""rt-test.py -- the render target code (mesa/files/.../sgx/sgx_rt.c)
against the kext's own (tools/iosgpu/rtemu.py), size by size.

    KC841=kernelcache python3 mesa/host/rt-test.py [SIZE...]
    python3 mesa/host/rt-test.py --dir DIR [SIZE...]

The first runs rtemu.py for each size (WxH; a sweep by default) in a
temporary directory -- it needs the decrypted 8.4.1 kernelcache, and the
GL driver's payload words that ./cascadia gpu leaves in
build/sgx2d/rtemu/payload.txt; the second takes rtemu.py's output from
DIR/WxH/.  Builds rt-test.c with sgx_rt.c, runs it on rtemu.py's
addresses, and compares every buffer, the payload words, the 3D block and
the TA command (tools/sgx/rgen.py's ta_cmd()).  The 3D block and the TA
command are another size's with this size's words written over them: the
words that do not change with the size are the GL driver's, not ours.
"""
import glob, json, os, struct, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
SGX = os.path.join(ROOT, 'mesa', 'files', 'src', 'gallium', 'drivers', 'sgx')
sys.path.insert(0, os.path.join(ROOT, 'tools', 'sgx'))
import rgen

SWEEP = ['%dx%d' % (w, h) for w in (32, 64, 128, 256, 512, 1024, 2048)
         for h in (32, 64, 128, 256, 512, 1024, 2048)]
SWEEP += ['768x1024', '1024x768', '300x200', '1x1', '33x33', '100x50', '767x1023',
          '1000x1', '480x320', '2049x64', '4096x4096', '4096x16']


def rtemu(sizes, out):
    payload = open(os.path.join(ROOT, 'build', 'sgx2d', 'rtemu', 'payload.txt')).read().split()
    for sz in sizes:
        w, h = map(int, sz.split('x'))
        d = os.path.join(out, sz)
        os.makedirs(d, exist_ok=True)
        p = list(payload)
        p[44], p[45] = '%08x' % w, '%08x' % h       # the GL driver's own copy of the size
        open(os.path.join(d, 'payload.txt'), 'w').write(' '.join(p))
        env = dict(os.environ, RT_GPU_BASE='0x87c00000', KC841=os.path.abspath(os.environ['KC841']))
        r = subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'iosgpu', 'rtemu.py'),
                            str(w), str(h), '1', '2', 'payload.txt'], cwd=d, env=env,
                           capture_output=True, text=True)
        open(os.path.join(d, 'out.txt'), 'w').write(r.stdout + r.stderr)
        if r.returncode:
            print('%s: rtemu.py failed (%s/out.txt)' % (sz, d))


def words(b):
    return struct.unpack('<%dI' % (len(b) // 4), b)


def compare(name, ours, theirs):
    if ours == theirs:
        return 0
    if len(ours) != len(theirs):
        print('    %s: %#x bytes, rtemu %#x' % (name, len(ours), len(theirs)))
    a, b = words(ours[:len(theirs)]), words(theirs[:len(ours)])
    bad = [i for i in range(len(a)) if a[i] != b[i]]
    for i in bad[:6]:
        print('    %s +%#05x: %08x, rtemu %08x' % (name, 4 * i, a[i], b[i]))
    if len(bad) > 6:
        print('    %s: %d words differ' % (name, len(bad)))
    return 1


def main():
    args = sys.argv[1:]
    tmp = tempfile.mkdtemp(prefix='rt-test.')
    if args[:1] == ['--dir']:
        top, args = args[1], args[2:]
        sizes = args or sorted(os.path.basename(os.path.dirname(p))
                               for p in glob.glob(os.path.join(top, '*x*', 'rt_*.json')))
    else:
        if 'KC841' not in os.environ:
            sys.exit('rt-test.py: KC841=kernelcache, or --dir DIR')
        top, sizes = os.path.join(tmp, 'rtemu'), args or SWEEP
        rtemu(sizes, top)
    exe = os.path.join(tmp, 'rt-test')
    subprocess.run(['cc', '-O1', '-Wall', '-Werror', '-I', SGX, '-o', exe,
                    os.path.join(HERE, 'rt-test.c'), os.path.join(SGX, 'sgx_rt.c')], check=True)

    dirs = {sz: os.path.join(top, sz) for sz in sizes}
    failed = 0
    for k, sz in enumerate(sizes):
        d = dirs[sz]
        other = dirs[sizes[(k + 1) % len(sizes)]]
        w, h = map(int, sz.split('x'))
        if not os.path.exists(os.path.join(d, 'rt_%s_blk3d.bin' % sz)):
            print('%-10s no rtemu.py output' % sz)
            failed += 1
            continue
        j = json.load(open(os.path.join(d, 'rt_%s.json' % sz)))
        blk_va = words(open(os.path.join(d, 'rt_%s_payload.bin' % sz), 'rb').read())[51 - 2]
        out = os.path.join(tmp, 'out', sz)
        os.makedirs(out, exist_ok=True)
        pin = os.path.join(out, 'payload_in.bin')
        open(pin, 'wb').write(b''.join(struct.pack('<I', int(x, 16)) for x in
                                       open(os.path.join(d, 'payload.txt')).read().split()[2:]))
        cin = os.path.join(out, 'cmd_in.bin')
        open(cin, 'wb').write(rgen.ta_cmd(other))
        r = subprocess.run([exe, str(w), str(h), str(j['ncores']), '%#x' % blk_va] +
                           ['%#x' % b['gpu'] for b in j['bufs']] +
                           [out, pin, glob.glob(os.path.join(other, 'rt_*_blk3d.bin'))[0], cin])
        bad = r.returncode
        if not bad:
            for i, b in enumerate(j['bufs']):
                bad |= compare('b%d' % i, open(os.path.join(out, 'b%d.bin' % i), 'rb').read(),
                               open(os.path.join(d, b['file']), 'rb').read())
            for name, theirs in (('payload', 'rt_%s_payload.bin' % sz),
                                 ('blk3d', 'rt_%s_blk3d.bin' % sz)):
                bad |= compare(name, open(os.path.join(out, name + '.bin'), 'rb').read(),
                               open(os.path.join(d, theirs), 'rb').read())
            bad |= compare('cmd', open(os.path.join(out, 'cmd.bin'), 'rb').read(), rgen.ta_cmd(d))
        print('%-10s %s' % (sz, 'WRONG' if bad else 'same'))
        failed += bool(bad)
    print('%d of %d sizes the same as the kext' % (len(sizes) - failed, len(sizes)))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
