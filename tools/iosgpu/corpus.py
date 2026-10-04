#!/usr/bin/env python3
"""corpus.py -- read what the shader oracle brought back (M11,
docs/research/p105-mesa.md): for each case, the USSE programs iOS's GL
driver made for it, disassembled, and the other bytes the draw changed.

    corpus.py DIR [--brief|--own] [CASE...]   DIR from tools/shadercap.sh
                                        (logs/ios/corpus/<date>)
      --brief  no hex of the other changed runs
      --own    also only the programs found in at most 5 cases: the
               shader's own, not the driver's per-draw ones
      --list   a line per program: where, how long, the opening mnemonics
      --catalog  every case's programs by kind, words and disassembly only

DIR holds log.txt (gltrace corpus's output, which gives the order the
cases ran in), baseline.pages (every page of the GL driver's GPU buffers
that was not zero before the first case) and NAME.pages per case (the pages
its draw changed), each a run of {u32 CPU address, 4096 bytes}.  Memory is
rebuilt case by case, so a case's bytes are exactly the ones its draw
changed; a program is a PHAS and the instructions after it up to the one
that ends it (bit 50 on VBW, VPCK, VMOV, SOP2, LIMM, NOP and the emits).
The GL driver splits a shader: a vertex program (ends with the vertex
emit), a pixel program (ends writing o0), and a secondary program run once
per draw (its preamble VTST, VLDST) that does the arithmetic that only
depends on uniforms, into a secondary attribute the pixel program reads.
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


def is_phas(w):
    """a PHAS as the GL driver writes them: fa44 07.. (one phase) or
    fa44 .. next phase.  The table's PHAS leaves most bits free, and almost
    any word decodes as something, so this is the anchor and not decode()"""
    return w >> 48 == 0xfa44


# Bit 50 ends a program on these (the layouts' 'e' there, or the special
# instructions'); on V*NMAD, VMAD2, VTST, VCOMP, SMLSI it is another field
# (a swizzle bit and the like) -- the first corpus run has V16NMADs with it
# set in the middle of programs.
END_AT_50 = {'VBW', 'SOP2', 'SOP2M', 'SOP3', 'VPCK', 'VMOV', 'LIMM', 'NOP', 'SPEC'}
EMIT_VERTEX = 0xfb275000a0200000


def ends(w, name):
    return name in END_AT_50 and bool(w & END)


def program_at(b, o):
    """the instructions from the PHAS at O to the one that ends the program;
    None if the run breaks first (the driver writes programs over older
    ones, so what follows the end is often the rest of an older one)"""
    out = []
    while o + 8 <= len(b) and len(out) < MAX_PROGRAM:
        w = word(b, o)
        name, _ = ud.decode(w)
        if not w or not name or name.startswith('ILLEGAL') or (out and is_phas(w)):
            return None
        out.append((w, name))
        if len(out) > 1 and ends(w, name):
            return out
        o += 8
    return None


def kind(prog):
    """vertex (ends emitting the vertex), secondary (the driver's per-draw
    program: its preamble, VTST VLDST -- uniform-only arithmetic goes here,
    the result to a secondary attribute), pixel (ends writing o0), or
    empty (PHAS, NOP)"""
    names = [n for _, n in prog]
    last_w, last = prog[-1]
    if last_w == EMIT_VERTEX:
        return 'vertex'
    if len(prog) == 2 and last == 'NOP':
        return 'empty'
    if 'VTST' in names[:4] and 'VLDST' in names[:5]:
        return 'secondary'
    if last in ('VBW', 'VPCK', 'SOP2', 'VMOV'):
        return 'pixel'
    return 'other'


def hexdump(b, start, end, base):
    lines = []
    s = start & ~3
    for o in range(s, end, 16):
        ws = [struct.unpack_from('<I', b, k)[0] for k in range(o, min(o + 16, end + 3 & ~3, PAGE), 4)]
        lines.append('      %08x: %s' % (base + o, ' '.join('%08x' % w for w in ws)))
    return lines


BRIEF = False
OWN_MAX = 5         # --own: a program in at most this many cases is the case's own


def phas_before(b, o, limit=0x800):
    """the nearest PHAS at or before O (8-aligned) in B, within LIMIT bytes"""
    k = o & ~7
    while k >= 0 and o - k <= limit:
        if is_phas(word(b, k)):
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


def analyse(mem, case_pages):
    """A case's changed bytes: programs that overlap them (from the PHAS
    before, so a program rewritten in place counts; CHANGED marks the
    instructions that did), code without a PHAS near, and the rest.
    Brings MEM up to date."""
    progs, codes, other, seen = [], [], [], set()
    for addr in sorted(case_pages):
        new = case_pages[addr]
        old = mem.get(addr, bytes(PAGE))
        code_page = any(is_phas(word(new, o)) for o in range(0, PAGE, 8))
        for a, b in changed_ranges(old, new):
            hit = False
            o = phas_before(new, a)
            o = a & ~7 if o is None else o
            while o < b:
                if (addr + o) not in seen and is_phas(word(new, o)):
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
            c = code_run(new, a, b) if code_page else None
            if c and (addr + c[0]) not in seen:
                p = [(word(new, k), ud.decode(word(new, k))[0]) for k in c]
                changed = [word(old, k) != word(new, k) for k in c]
                codes.append((addr + c[0], p, changed))
                seen.update(addr + k for k in c)
            else:
                other.append((addr, a, b, new))
        mem[addr] = new
    return progs, codes, other


def key(p):
    return tuple(w for w, _ in p)


def show(title, at, p, changed):
    print('-- %s at CPU %08x, %d instructions, %d changed' % (title, at, len(p), sum(changed)))
    for k, (w, n) in enumerate(p):
        print('  %s+%03x: %016x  %-9s%s%s' % ('*' if changed[k] else ' ', 8 * k, w, n,
                                            ud.operands(n, w), '  <end>' if ends(w, n) else ''))


def listing(name, result):
    """one line per program and code run: where, how long, what changed,
    the opening mnemonics"""
    progs, codes, other = result
    print('== %s: %d programs, %d code runs, %d other runs' % (name, len(progs), len(codes), len(other)))
    for title, items in (('prog', progs), ('code', codes)):
        for at, p, changed in items:
            print('   %-9s %08x %3d instr %3d changed  %s' %
                  (kind(p) if title == 'prog' else 'code', at, len(p), sum(changed),
                   ' '.join(n for _, n in p[:14])))


GROUP = {'vertex': 'vertex', 'secondary': 'secondary', 'empty': 'secondary',
         'pixel': 'pixel', 'other': 'other'}


def catalog(name, result, last):
    """a case's programs, by kind, as words and their disassembly -- test
    vectors for our compiler: what iOS's compiler made of the same GLSL.
    A program the draw did not change is not in the case's pages; LAST
    holds the latest one of each group, printed with the case it is from."""
    order = ('vertex', 'secondary', 'pixel', 'other')
    found = {}
    for at, p, _ in result[0]:
        found.setdefault(GROUP[kind(p)], p)
    print('== %s' % name)
    for g in order:
        if g in found:
            p, note = found[g], ''
            last[g] = (name, p)
        elif g in last and g != 'other':
            p, note = last[g][1], ' (unchanged since %s)' % last[g][0]
        else:
            continue
        print('-- %s%s' % (kind(p), note))
        for w, n in p:
            print('   %016x  %-9s%s%s' % (w, n, ud.operands(n, w), '  <end>' if ends(w, n) else ''))


def report(name, src, result, keep=None):
    """print a case; with KEEP, only the programs and code KEEP(p) passes"""
    progs, codes, other = result
    print('=' * 78)
    print('== %s' % name)
    if src:
        for line in src.rstrip().split('\n'):
            print('   | ' + line)
    hidden = 0
    for at, p, changed in progs:
        if keep and not keep(p):
            hidden += 1
            continue
        show('%s program' % kind(p), at, p, changed)
    for at, p, changed in codes:
        if keep and not keep(p):
            hidden += 1
            continue
        show('code without a PHAS near', at, p, changed)
    if hidden:
        print('-- %d more, also in other cases (not shown)' % hidden)
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
    global BRIEF
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    d = sys.argv[1]
    BRIEF = '--brief' in sys.argv or '--own' in sys.argv
    own = '--own' in sys.argv
    only = set(a for a in sys.argv[2:] if not a.startswith('--'))
    log = open(os.path.join(d, 'log.txt'), errors='replace').read()
    order = re.findall(r'^== case (\S+)', log, re.M)
    base = os.path.join(d, 'baseline.pages')
    mem = pages(base) if os.path.exists(base) else {}
    results = []
    for name in order:
        path = os.path.join(d, name + '.pages')
        results.append((name, analyse(mem, pages(path)) if os.path.exists(path) else None))
    # how many cases each program (by its words) turns up in
    seen_in = {}
    for name, r in results:
        if r:
            for k in set(key(p) for _, p, _ in r[0] + r[1]):
                seen_in[k] = seen_in.get(k, 0) + 1
    keep = (lambda p: seen_in.get(key(p), 0) <= OWN_MAX) if own else None
    summary, last = [], {}
    for name, r in results:
        if r is None:
            summary.append((name, None, None))
            continue
        n_own = sum(1 for _, p, _ in r[0] + r[1] if seen_in.get(key(p), 0) <= OWN_MAX)
        summary.append((name, len(r[0]) + len(r[1]), n_own))
        if only and name not in only:
            for at, p, _ in r[0]:
                last[GROUP[kind(p)]] = (name, p)
            continue
        if '--list' in sys.argv:
            listing(name, r)
            continue
        if '--catalog' in sys.argv:
            catalog(name, r, last)
            continue
        src_path = os.path.join(HERE, 'corpus', name + '.glsl')
        report(name, open(src_path).read() if os.path.exists(src_path) else '', r, keep)
    if '--catalog' in sys.argv:
        return
    print('=' * 78)
    print('== summary: programs and code runs per case, and those in at most %d cases '
          '(- not drawn)' % OWN_MAX)
    for name, n, n_own in summary:
        print('   %-24s %s' % (name, '-' if n is None else '%3d %3d' % (n, n_own)))


if __name__ == '__main__':
    main()
