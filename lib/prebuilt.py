#!/usr/bin/env python3
"""prebuilt.py -- prebuilt images by manifest: fetch them, or write the manifest.

    prebuilt.py get MANIFEST DEST       the files MANIFEST names, into DEST/<version>/
                                        (resumed, checked by sha256), and
                                        DEST/current pointing at that version
    prebuilt.py manifest DIR VERSION URLBASE COMMIT OUT
                                        a manifest for the files in DIR, each at
                                        URLBASE/<name> (a GitHub release's)
    prebuilt.py current DEST            the version DEST/current points at

A manifest is prebuilt/<channel>/<soc>.json in the repository: the version,
the commit it was built from, and per file its URL, size and sha256.  The
files themselves live in GitHub Releases, not in git: they are tens to
hundreds of MB, and every version would stay in the history for good.
Standard library only.
"""
import hashlib
import json
import os
import sys
import time
import urllib.request

CHUNK = 1 << 20


def fail(msg):
    sys.exit('prebuilt: ' + msg)


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(CHUNK), b''):
            h.update(b)
    return h.hexdigest()


def fetch(url, path, size):
    """into path.part, resumed from where an earlier try stopped"""
    part = path + '.part'
    have = os.path.getsize(part) if os.path.exists(part) else 0
    if have > size:
        os.remove(part); have = 0
    tries = 0
    while have < size:
        req = urllib.request.Request(url, headers={'Range': 'bytes=%d-' % have} if have else {})
        try:
            with urllib.request.urlopen(req, timeout=60) as r, open(part, 'ab' if have else 'wb') as f:
                if have and r.status != 206:           # no ranges: start again
                    f.truncate(0); have = 0
                t0, last = time.time(), 0
                while True:
                    b = r.read(CHUNK)
                    if not b:
                        break
                    f.write(b); have += len(b)
                    if time.time() - last > 0.5 or have == size:
                        last = time.time()
                        rate = have / max(time.time() - t0, 0.1) / 1048576
                        sys.stderr.write('\r    %s  %5.1f%%  %6.1f MB  %5.1f MB/s ' % (
                            os.path.basename(path), 100.0 * have / size, have / 1048576, rate))
        except (OSError, urllib.error.URLError) as e:
            tries += 1
            if tries > 5:
                sys.stderr.write('\n')
                fail('%s: %s -- run it again to resume' % (url, e))
            time.sleep(2 * tries)
    sys.stderr.write('\n')
    os.replace(part, path)


def get(manifest, dest):
    m = json.load(open(manifest))
    vdir = os.path.join(dest, m['version'])
    os.makedirs(vdir, exist_ok=True)
    for f in m['files']:
        p = os.path.join(vdir, f['name'])
        if os.path.exists(p) and os.path.getsize(p) == f['size'] and sha256(p) == f['sha256']:
            print('    ok: %s (have it)' % f['name'])
            continue
        fetch(f['url'], p, f['size'])
        if sha256(p) != f['sha256']:
            os.remove(p)
            fail('%s does not match its sha256 -- removed; run it again' % f['name'])
        print('    ok: %s' % f['name'])
    json.dump(m, open(os.path.join(vdir, 'manifest.json'), 'w'), indent=1)
    cur = os.path.join(dest, 'current')
    if os.path.islink(cur) or os.path.exists(cur):
        os.remove(cur)
    os.symlink(m['version'], cur)
    print('    prebuilt %s (commit %s)' % (m['version'], m.get('commit', '?')))


def manifest(d, version, urlbase, commit, out):
    files = []
    for n in sorted(os.listdir(d)):
        p = os.path.join(d, n)
        if not os.path.isfile(p) or n == 'manifest.json' or n.endswith('.part'):
            continue
        files.append({'name': n, 'url': urlbase.rstrip('/') + '/' + n,
                      'size': os.path.getsize(p), 'sha256': sha256(p)})
    m = {'version': version, 'commit': commit, 'files': files}
    with open(out, 'w') as f:
        json.dump(m, f, indent=1)
        f.write('\n')
    print('manifest: %s, %d files' % (out, len(files)))


def main(a):
    if a[:1] == ['get'] and len(a) == 3:
        get(a[1], a[2])
    elif a[:1] == ['manifest'] and len(a) == 6:
        manifest(*a[1:])
    elif a[:1] == ['current'] and len(a) == 2:
        cur = os.path.join(a[1], 'current')
        if not os.path.islink(cur):
            return 1
        print(os.readlink(cur))
    else:
        sys.stderr.write(__doc__)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
