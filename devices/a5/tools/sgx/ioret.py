#!/usr/bin/env python3
"""ioret.py PERF.DATA: the ioctls of a perf record of raw_syscalls
sys_enter and sys_exit (filter id == 54), counted by command and result
(docs/research/p105-mesa.md, M38), on the iPad:

    perf record -o /tmp/x.data -e raw_syscalls:sys_enter -e raw_syscalls:sys_exit \
        --filter 'id == 54' -p $(pidof Xorg) -- sleep 4
    python3 ioret.py /tmp/x.data

c01864b0 is a page flip, c01864b1 a DIRTYFB, c018643b CRTC_GET_SEQUENCE,
40406444 the driver's SUBMIT."""
import collections, re, subprocess, sys
out = subprocess.run(["perf", "script", "-i", sys.argv[1], "-F", "tid,event,trace"],
                     capture_output=True, text=True).stdout
last, n = {}, collections.Counter()
for line in out.splitlines():
    m = re.match(r"\s*(\d+)\s+raw_syscalls:sys_(enter|exit):\s+NR \d+ (.*)", line)
    if not m:
        continue
    tid, kind, rest = m.groups()
    if kind == "enter":
        a = re.match(r"\(([0-9a-f]+), ([0-9a-f]+)", rest)
        last[tid] = a.group(2) if a else "?"
    else:
        r = re.search(r"= (-?\d+)", rest)
        n[(last.pop(tid, "?"), r.group(1) if r else "?")] += 1
for (cmd, ret), k in sorted(n.items(), key=lambda x: -x[1]):
    print("%6d %s -> %s" % (k, cmd, ret))
