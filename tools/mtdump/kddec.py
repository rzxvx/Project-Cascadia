#!/usr/bin/env python3
# Decode raw kd_buf dumps from kdtrace (armv7 iOS 8.4.1, K32: 32-byte entries).
# Usage:
#   kddec.py hist FILE                         class + (class,sub,code) histograms
#   kddec.py diff BASE GAME                    (class,sub,code) counts: BASE vs GAME
#   kddec.py dump FILE CLASS[,SUB[,CODE]] [N]  decode events matching filter (hex class/dec sub/dec code)
import sys, struct

REC = struct.Struct('<Q6I')   # u64 ts; u32 arg1..arg5; u32 debugid  (32 bytes)

def load(path):
    data = open(path, 'rb').read()
    n = len(data) // 32
    out = []
    for i in range(n):
        ts, a1, a2, a3, a4, a5, dbg = REC.unpack_from(data, i*32)
        out.append((ts, a1, a2, a3, a4, a5, dbg))
    return out

def dcode(dbg):
    return ((dbg >> 24) & 0xff, (dbg >> 16) & 0xff, (dbg >> 2) & 0x3fff, dbg & 3)

def hist(path):
    ev = load(path)
    print(f"{len(ev)} events")
    cls = {}
    scc = {}
    for e in ev:
        c, s, co, f = dcode(e[6])
        cls[c] = cls.get(c, 0) + 1
        scc[(c, s, co)] = scc.get((c, s, co), 0) + 1
    print("== class histogram ==")
    for c in sorted(cls): print(f"  class 0x{c:02x} : {cls[c]}")
    print("== (class,sub,code) histogram ==")
    for k in sorted(scc, key=lambda k: -scc[k]):
        print(f"  cl=0x{k[0]:02x} sub={k[1]:<3d} code={k[2]:<5d} : {scc[k]}")

def key_counts(path):
    ev = load(path); d = {}
    for e in ev:
        c, s, co, f = dcode(e[6])
        d[(c, s, co)] = d.get((c, s, co), 0) + 1
    return d, len(ev)

def diff(base, game):
    b, nb = key_counts(base)
    g, ng = key_counts(game)
    print(f"BASE {nb} events, GAME {ng} events")
    print("== (class,sub,code): keys NEW or grown in GAME (game>base), sorted by game count ==")
    keys = set(b) | set(g)
    rows = []
    for k in keys:
        bc, gc = b.get(k, 0), g.get(k, 0)
        rows.append((k, bc, gc))
    for k, bc, gc in sorted(rows, key=lambda r: -r[2]):
        tag = " <== NEW" if bc == 0 and gc else (" <== grew" if gc > bc*3 and gc > 20 else "")
        print(f"  cl=0x{k[0]:02x} sub={k[1]:<3d} code={k[2]:<5d} base={bc:<6d} game={gc:<6d}{tag}")

def dump(path, filt, limit):
    parts = filt.split(',')
    fc = int(parts[0], 16)
    fs = int(parts[1]) if len(parts) > 1 and parts[1] != '' else None
    fco = int(parts[2]) if len(parts) > 2 and parts[2] != '' else None
    ev = load(path)
    t0 = ev[0][0] if ev else 0
    shown = 0
    for e in ev:
        ts, a1, a2, a3, a4, a5, dbg = e
        c, s, co, f = dcode(dbg)
        if c != fc: continue
        if fs is not None and s != fs: continue
        if fco is not None and co != fco: continue
        fn = {1: 'START', 2: 'END', 0: '-', 3: '?'}[f]
        print(f"  +{(ts-t0):>12d} cl=0x{c:02x} sub={s:<3d} code={co:<4d} {fn:5s} "
              f"a1=0x{a1:08x} a2=0x{a2:08x} a3=0x{a3:08x} a4=0x{a4:08x} thr=0x{a5:x}")
        shown += 1
        if shown >= limit: break
    print(f"[{shown} events shown]")

def onset(path, k, target=0x31):
    """Sort by timestamp, find the FIRST event of `target` class (GPU graphics),
    and print the events around it (excluding noisy classes 0x01/0x04) -- the
    clock/gate that enables the GPU must fire just before graphics starts."""
    ev = sorted(load(path), key=lambda e: e[0])          # by raw mach timestamp
    idx = next((i for i, e in enumerate(ev) if dcode(e[6])[0] == target), None)
    if idx is None:
        print(f"no class 0x{target:02x} event found"); return
    print(f"first class 0x{target:02x} at sorted index {idx}, ts={ev[idx][0]}")
    lo = max(0, idx - k*30)   # scan a wide index band, then filter noise, cap output
    shown = 0
    for i in range(lo, min(len(ev), idx + k)):
        ts, a1, a2, a3, a4, a5, dbg = ev[i]
        c, s, co, f = dcode(dbg)
        if c in (0x01, 0x04): continue
        if shown == 0: t0 = ts
        mark = "  <== FIRST 0x31" if i == idx else ""
        fn = {1:'START',2:'END',0:'-',3:'?'}[f]
        print(f"  d{ts-ev[idx][0]:+d} cl=0x{c:02x} sub={s:<3d} code={co:<4d} {fn:5s} "
              f"a1=0x{a1:08x} a2=0x{a2:08x} a3=0x{a3:08x} a4=0x{a4:08x}{mark}")
        shown += 1
        if shown >= k*2: break

if __name__ == '__main__':
    cmd = sys.argv[1] if len(sys.argv) > 1 else 'hist'
    if cmd == 'hist': hist(sys.argv[2])
    elif cmd == 'diff': diff(sys.argv[2], sys.argv[3])
    elif cmd == 'dump': dump(sys.argv[2], sys.argv[3], int(sys.argv[4]) if len(sys.argv) > 4 else 60)
    elif cmd == 'onset': onset(sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 40)
    else: print("cmd: hist|diff|dump|onset")
