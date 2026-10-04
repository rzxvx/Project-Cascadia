#!/usr/bin/env python3
"""corpus.py -- read what the shader oracle brought back (M11,
docs/research/p105-mesa.md): for each case, the USSE programs iOS's GL
driver made for it, disassembled, and the other bytes the draw changed.

    corpus.py DIR [--brief] [CASE...]   DIR from tools/shadercap.sh
                                        (logs/ios/corpus/<date>); --brief:
                                        no hex of the other changed runs

DIR holds log.txt (gltrace corpus's output, which gives the order the
cases ran in), baseline.pages (every page of the GL driver's GPU buffers
that was not zero before the first case) and NAME.pages per case (the pages
its draw changed), each a run of {u32 CPU address, 4096 bytes}.  Memory is
rebuilt case by case, so a case's bytes are exactly the ones its draw
changed; a program is a PHAS and the instructions after it up to the next
PHAS or a zero word (bit 50, the end flag, marked <end>).  Fragment programs
open with the driver's preamble (VTST, VLDST: the load that never happens),
which marks them.
"""
import importlib.util, os, re, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location('usse_dis', os.path.join(HERE, 'usse-dis.py'))
ud = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(ud)

PAGE = 0x1000
END = 1 << 50
MAX_PROGRAM = 96            # instructions


def pages(path):
    """{CPU address: bytes} of a .pages file"""
    d = open(path, 'rb').read()
    if len(d) % (4 + PAGE):
        raise SystemExit('%s: %d bytes, not whole records' % (path, len(d)))
    out = {}
    for o in range(0, len(d), 4 + PAGE):
        out[struct.unpack_from('<I', d, o)[0]] = d[o + 4:o + 4 + PAGE]
    return out


def changed_ranges(old, new):
    """[(start, end)] of the bytes that differ, runs closer than 16 bytes
    joined"""
    out = []
    i = 0
    while i < PAGE:
        if old[i] == new[i]:
            i += 1
            continue
        j = i
        while j < PAGE and (old[j] != new[j] or any(old[k] != new[k] for k in range(j, min(j + 16, PAGE)))):
            j += 1
        out.append((i, j))
        i = j
    return out


def word(b, o):
    return struct.unpack_from('<Q', b, o)[0]


def program_at(b, o):
    """the instructions from the PHAS at O up to the next PHAS, a zero word
    or one that does not decode -- not to the end flag, which the driver's
    own preambles have been seen to carry mid-program (corpus 0 will say)"""
    out = []
    while o + 8 <= len(b) and len(out) < MAX_PROGRAM:
        w = word(b, o)
        name, _ = ud.decode(w)
        if not w or not name or name.startswith('ILLEGAL') or (out and name == 'PHAS'):
            break
        out.append((w, name))
        o += 8
    return out if len(out) > 1 else None


def kind(prog):
    names = [n for _, n in prog[:6]]
    return 'fragment' if 'VTST' in names and 'VLDST' in names else 'other'


def hexdump(b, start, end, base):
    lines = []
    s = start & ~3
    for o in range(s, end, 16):
        ws = [struct.unpack_from('<I', b, k)[0] for k in range(o, min(o + 16, end + 3 & ~3, PAGE), 4)]
        lines.append('      %08x: %s' % (base + o, ' '.join('%08x' % w for w in ws)))
    return lines


BRIEF = False


def phas_before(b, o, limit=0x800):
    """the nearest PHAS at or before O (8-aligned) in B, within LIMIT bytes"""
    k = o & ~7
    while k >= 0 and o - k <= limit:
        if ud.decode(word(b, k))[0] == 'PHAS':
            return k
        k -= 8
    return None


def code_run(b, a, e):
    """the 8-aligned instructions over [A, E) if at least three in a row
    decode, else None"""
    o, run, best = a & ~7, [], []
    while o < e and o + 8 <= PAGE:
        w = word(b, o)
        n = ud.decode(w)[0]
        if w and n and not n.startswith('ILLEGAL'):
            run.append(o)
        else:
            best = max(best, run, key=len)
            run = []
        o += 8
    best = max(best, run, key=len)
    return best if len(best) >= 3 else None


def report(name, mem, case_pages, src):
    """A case's changed bytes: programs that overlap them (from the PHAS
    before, so a program rewritten in place counts; '*' marks the
    instructions that changed), code without a PHAS near, and the rest."""
    print('=' * 78)
    print('== %s' % name)
    if src:
        for line in src.rstrip().split('\n'):
            print('   | ' + line)
    progs, codes, other, seen = [], [], [], set()
    for addr in sorted(case_pages):
        new = case_pages[addr]
        old = mem.get(addr, bytes(PAGE))
        for a, b in changed_ranges(old, new):
            hit = False
            o = phas_before(new, a)
            o = a & ~7 if o is None else o
            while o < b:
                if (addr + o) not in seen and ud.decode(word(new, o))[0] == 'PHAS':
                    p = program_at(new, o)
                    if p and o + 8 * len(p) > a:
                        changed = [word(old, o + 8 * k) != w for k, (w, _) in enumerate(p)]
                        progs.append((addr + o, p, changed))
                        seen.update(addr + o + 8 * k for k in range(len(p)))
                        o += 8 * len(p)
                        hit = True
                        continue
                o += 8
            if hit:
                continue
            c = code_run(new, a, b)
            if c and (addr + c[0]) not in seen:
                codes.append((addr, c, new, old))
                seen.update(addr + k for k in c)
            else:
                other.append((addr, a, b, new))
        mem[addr] = new
    for at, p, changed in progs:
        print('-- %s program at CPU %08x, %d instructions, %d changed' %
              (kind(p), at, len(p), sum(changed)))
        for k, (w, n) in enumerate(p):
            print('  %s+%03x: %016x  %-9s%s%s' % ('*' if changed[k] else ' ', 8 * k, w, n,
                                                ud.operands(n, w),
                                                '  <end>' if n != 'PHAS' and w & END else ''))
    for addr, c, new, old in codes:
        print('-- code without a PHAS near, CPU %08x, %d instructions' % (addr + c[0], len(c)))
        for o in c:
            w = word(new, o)
            n = ud.decode(w)[0]
            print('  %s%08x: %016x  %-9s%s%s' % ('*' if word(old, o) != w else ' ', addr + o, w, n,
                                              ud.operands(n, w), '  <end>' if n != 'PHAS' and w & END else ''))
    small = [r for r in other if r[2] - r[1] <= 0x100]
    print('-- %d other changed run(s), %d of them up to 256 bytes%s' %
          (len(other), len(small), '' if BRIEF else ':'))
    if BRIEF:
        pagesum = {}
        for addr, a, b, _ in other:
            pagesum[addr] = pagesum.get(addr, 0) + b - a
        print('   by page: ' + ' '.join('%08x:%d' % (k, v) for k, v in sorted(pagesum.items())))
    else:
        for addr, a, b, new in small:
            print('   CPU %08x +0x%x bytes' % (addr + a, b - a))
            for line in hexdump(new, a, b, addr):
                print(line)
    return len(progs) + len(codes)


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    global BRIEF
    d = sys.argv[1]
    BRIEF = '--brief' in sys.argv
    only = set(a for a in sys.argv[2:] if a != '--brief')
    log = open(os.path.join(d, 'log.txt'), errors='replace').read()
    order = re.findall(r'^== case (\S+)', log, re.M)
    mem = pages(os.path.join(d, 'baseline.pages')) if os.path.exists(os.path.join(d, 'baseline.pages')) else {}
    summary = []
    for name in order:
        path = os.path.join(d, name + '.pages')
        if not os.path.exists(path):
            summary.append((name, None))
            continue
        cp = pages(path)
        src_path = os.path.join(HERE, 'corpus', name + '.glsl')
        src = open(src_path).read() if os.path.exists(src_path) else ''
        if only and name not in only:
            for a, b in cp.items():         # keep memory right for later cases
                mem[a] = b
            continue
        summary.append((name, report(name, mem, cp, src)))
    print('=' * 78)
    print('== summary: programs and code runs found per case (- not drawn)')
    for name, n in summary:
        print('   %-24s %s' % (name, '-' if n is None else n))


if __name__ == '__main__':
    main()
