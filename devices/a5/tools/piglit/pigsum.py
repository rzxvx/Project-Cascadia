#!/usr/bin/env python3
"""pigsum.py DIR [STATUS...] -- a piglit run's results by status: the tests
that did not pass (or those of the statuses named: pass, fail, crash, skip,
timeout, warn, incomplete), a line each, then the count of every status.
DIR is what tools/piglit/run.sh wrote (/root/piglit-results/NAME): its
tests/*.json while the run goes on, results.json.bz2 when it is done.

    python3 /root/pigsum.py /root/piglit-results/p4 > p4.fails
    diff <(sed '$d' p3.fails | cut -c12- | sort) <(sed '$d' p4.fails | cut -c12- | sort)
"""
import bz2, gzip, json, lzma, os, sys

if len(sys.argv) < 2:
    sys.exit(__doc__)
d = sys.argv[1]
want = set(sys.argv[2:]) or {'fail', 'crash', 'timeout', 'incomplete', 'dmesg-fail', 'warn'}
results = {}
tdir = os.path.join(d, 'tests')
if os.path.isdir(tdir):
    for f in os.listdir(tdir):
        if f.endswith('.json'):
            try:
                results.update(json.load(open(os.path.join(tdir, f))))
            except ValueError:
                pass            # (the one being written)
else:
    for name, op in (('results.json.bz2', bz2.open), ('results.json.gz', gzip.open),
                     ('results.json.xz', lzma.open), ('results.json', open)):
        p = os.path.join(d, name)
        if os.path.exists(p):
            results = json.load(op(p, 'rt'))['tests']
            break
counts = {}
for name, v in sorted(results.items()):
    counts[v['result']] = counts.get(v['result'], 0) + 1
    if v['result'] in want:
        print('%-10s %s' % (v['result'], name))
print(counts)
