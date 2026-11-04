#!/usr/bin/env python3
"""release-check.py -- refuse a release that carries anything of one machine's.

    release-check.py --forbid DIR [DIR...] --cpio FILE --tar FILE [--tar FILE]

Every file under the --forbid directories (build/firmware: Apple's, from
the IPSW; build/keep and build/overlay: keys, the host key, addresses) is
hashed, and no member of the cpio (the kernel's built-in initramfs) or of
a tar (the desktop root, .tar or .tar.xz) may have the same contents, nor
a name of the kind they have, nor a private key in it.  Exit 1 with the
list when anything is found.
"""
import hashlib
import os
import re
import sys
import tarfile

NAMES = re.compile(r'(^|/)(P105\.mtprops|brcmfmac4334[^/]*|BCM4334B0[^/]*|sgx543\.fw|authorized_keys|'
                   r'dropbear_[a-z0-9]+_host_key|nfsroot|[^/]*\.ipsw|kernelcache[^/]*|gl-event\.pds)$'
                   r'|(^|/)usr/local/lib/sgx2d(/|$)')
# a real key: the header and then base64, not the bare header string that
# libgio and the MIME database carry to recognise the format
KEY = re.compile(rb'-----BEGIN [A-Z ]*PRIVATE KEY-----\r?\n(?:[A-Za-z0-9+/=]{40,}\r?\n){2}')


def forbidden(dirs):
    h = {}
    for d in dirs:
        for dp, _, fs in os.walk(d):
            for f in fs:
                p = os.path.join(dp, f)
                if os.path.isfile(p) and os.path.getsize(p) > 64:     # not tiny common files
                    h[hashlib.sha256(open(p, 'rb').read()).hexdigest()] = os.path.relpath(p)
    return h


def cpio_members(path):
    d = open(path, 'rb').read()
    p = 0
    while p + 110 <= len(d) and d[p:p + 6] in (b'070701', b'070702'):
        f = [int(d[p + 6 + 8 * i:p + 14 + 8 * i], 16) for i in range(13)]
        mode, fsize, nsize = f[1], f[6], f[11]
        name = d[p + 110:p + 110 + nsize - 1].decode('utf-8', 'replace')
        if name == 'TRAILER!!!':
            break
        p = (p + 110 + nsize + 3) & ~3
        yield name, d[p:p + fsize] if (mode & 0o170000) == 0o100000 else None
        p = (p + fsize + 3) & ~3


def tar_members(path):
    with tarfile.open(path, 'r:*') as t:
        for m in t:
            yield m.name, t.extractfile(m).read() if m.isfile() else None


def check(label, members, bad_hashes, problems):
    n = 0
    for name, data in members:
        n += 1
        name = name.lstrip('./')
        if NAMES.search(name):
            problems.append('%s: %s (a name of the kind only one machine has)' % (label, name))
        if data:
            h = hashlib.sha256(data).hexdigest()
            if h in bad_hashes:
                problems.append('%s: %s is %s' % (label, name, bad_hashes[h]))
            # not inside programs: libraries carry public test keys (GnuTLS's
            # self-tests); a key of one's own would be a file -- and ours are
            # caught above by their sha256 and their names
            if not data.startswith(b'\x7fELF') and KEY.search(data):
                problems.append('%s: %s holds a private key' % (label, name))
    print('    checked %s: %d entries' % (label, n))


def main(a):
    dirs, cpios, tars, cur = [], [], [], None
    for x in a:
        if x in ('--forbid', '--cpio', '--tar'):
            cur = {'--forbid': dirs, '--cpio': cpios, '--tar': tars}[x]
        elif cur is None:
            sys.stderr.write(__doc__); return 2
        else:
            cur.append(x)
    bad = forbidden(dirs)
    print('    %d files no release may carry (%s)' % (len(bad), ', '.join(dirs)))
    problems = []
    for c in cpios:
        check(os.path.basename(c), cpio_members(c), bad, problems)
    for t in tars:
        check(os.path.basename(t), tar_members(t), bad, problems)
    if problems:
        print('NOT for release:')
        for p in problems[:50]:
            print('    ' + p)
        return 1
    print('    ok: nothing of this machine\'s in it')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
