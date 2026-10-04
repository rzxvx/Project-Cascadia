#!/usr/bin/env python3
"""corpus.py -- read what the shader oracle brought back (M11,
docs/research/p105-mesa.md): for each case, the USSE programs iOS's GL
driver made for it, disassembled, and the other bytes the draw changed.

    corpus.py DIR [--brief|--own] [CASE...]   DIR from tools/shadercap.sh
                                        (logs/ios/corpus/<date>); CASE
                                        a name or a pattern ('f*')
      --brief  no hex of the other changed runs
      --own    also only the programs found in at most 5 cases: the
               shader's own, not the driver's per-draw ones
      --list   a line per program: where, how long, the opening mnemonics
      --catalog  every case's programs by kind, words and disassembly only
      --diff A B  memory after case A against after case B, without the
               USSE programs and the driver's bookkeeping: the state words
               and PDS programs the draws set apart
      --state  the TA's full state block(s) each case's draw wrote
      --pds    the PDS programs that start each case's USSE programs

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
import fnmatch, importlib.util, os, re, struct, sys

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
    Programs are read across page boundaries: each page is looked at with
    its neighbours, as memory is before the draw and after.  Brings MEM up
    to date."""
    progs, codes, other, seen = [], [], [], set()
    after = dict(mem)
    after.update(case_pages)

    def span(view, addr):
        return b''.join(view.get(addr + d, bytes(PAGE)) for d in (-PAGE, 0, PAGE))

    for addr in sorted(case_pages):
        new = case_pages[addr]
        old = mem.get(addr, bytes(PAGE))
        nb, ob, base = span(after, addr), span(mem, addr), addr - PAGE
        code_page = any(is_phas(word(new, o)) for o in range(0, PAGE, 8))
        for a, b in changed_ranges(old, new):
            hit = False
            A, B = PAGE + a, PAGE + b
            o = phas_before(nb, A)
            o = A & ~7 if o is None else o
            while o < B:
                if (base + o) not in seen and is_phas(word(nb, o)):
                    p = program_at(nb, o)
                    if p and o + 8 * len(p) > A:
                        changed = [word(ob, o + 8 * k) != w for k, (w, _) in enumerate(p)]
                        progs.append((base + o, p, changed))
                        seen.update(base + o + 8 * k for k in range(len(p)))
                        o += 8 * len(p)
                        hit = True
                        continue
                o += 8
            if hit or all((addr + k) in seen for k in range(a & ~7, b, 8)):
                continue        # (or a program found from the page before covers it)
            c = code_run(new, a, b) if code_page else None
            if c and (addr + c[0]) not in seen:
                p = [(word(new, k), ud.decode(word(new, k))[0]) for k in c]
                changed = [word(old, k) != word(new, k) for k in c]
                more = None
                if is_phas(p[0][0]) and c[-1] == PAGE - 8 and c == list(range(c[0], PAGE, 8)):
                    more = continuation(case_pages, after, addr)
                if more:
                    at2, rest = more
                    CONTINUED[addr + c[0]] = at2
                    progs.append((addr + c[0], p + rest, changed + [True] * len(rest)))
                    seen.update(at2 + 8 * k for k in range(len(rest)))
                else:
                    codes.append((addr + c[0], p, changed))
                seen.update(addr + k for k in c)
            else:
                other.append((addr, a, b, new))
    mem.update(case_pages)
    return progs, codes, other


CONTINUED = {}      # program address -> where its continuation was found


def continuation(case_pages, after, cut):
    """A program that runs to the end of the page at CUT, with the next
    page's CPU address not holding its rest: the GPU's next page is mapped
    elsewhere.  The rest is taken from the first page the draw changed that
    starts with instructions (no PHAS) running to an end; (address,
    instructions) or None."""
    for addr in sorted(case_pages):
        if addr == cut + PAGE:
            continue
        b = after[addr]
        if is_phas(word(b, 0)) or not any(is_phas(word(b, o)) for o in range(0, PAGE, 8)):
            continue        # (the rest is code: a page that holds programs)
        out, o = [], 0
        while o + 8 <= PAGE and len(out) < MAX_PROGRAM:
            w = word(b, o)
            name, _ = ud.decode(w)
            if not w or not name or name.startswith('ILLEGAL') or is_phas(w):
                break
            out.append((w, name))
            if ends(w, name):
                return addr, out
            o += 8
    return None


def key(p):
    return tuple(w for w, _ in p)


def second(p):
    """a secondary program's: its pa bank is the secondary attributes"""
    return kind(p) in ('secondary', 'empty')


def show(title, at, p, changed):
    print('-- %s at CPU %08x, %d instructions, %d changed' % (title, at, len(p), sum(changed)))
    for k, (w, n) in enumerate(p):
        print('  %s+%03x: %016x  %-9s%s%s' % ('*' if changed[k] else ' ', 8 * k, w, n,
                                            ud.operands(n, w, second(p)),
                                            '  <end>' if ends(w, n) else ''))


def listing(name, result):
    """one line per program and code run: where, how long, what changed,
    the opening mnemonics"""
    progs, codes, other = result
    print('== %s: %d programs, %d code runs, %d other runs' % (name, len(progs), len(codes), len(other)))
    for title, items in (('prog', progs), ('code', codes)):
        for at, p, changed in items:
            print('   %-9s %08x %3d instr %3d changed  %s%s' %
                  (kind(p) if title == 'prog' else 'code', at, len(p), sum(changed),
                   ' '.join(n for _, n in p[:14]),
                   '  (continues at %08x)' % CONTINUED[at] if at in CONTINUED else ''))


GROUP = {'vertex': 'vertex', 'secondary': 'secondary', 'empty': 'secondary',
         'pixel': 'pixel', 'other': 'other'}


def catalog(name, result, last):
    """a case's programs, by kind, as words and their disassembly -- test
    vectors for our compiler: what iOS's compiler made of the same GLSL.
    A kind the draw wrote no program of is only named: the driver used one
    already in GPU memory (the same code an earlier case's draw left there),
    or the shader needs none -- which of the two, the capture does not say."""
    order = ('vertex', 'secondary', 'pixel', 'other')
    found = {}
    for at, p, _ in result[0]:
        found.setdefault(GROUP[kind(p)], []).append((at, p))
    print('== %s' % name)
    for g in order:
        if g not in found:
            if g in last:
                print('-- %s: none written by this draw (the last one was in %s)' % (g, last[g]))
            continue
        last[g] = name
        for at, p in found[g]:
            print('-- %s%s' % (kind(p), '  (continues at CPU %08x: the next page is mapped '
                               'elsewhere; a guess)' % CONTINUED[at] if at in CONTINUED else ''))
            for w, n in p:
                print('   %016x  %-9s%s%s' % (w, n, ud.operands(n, w, second(p)),
                                             '  <end>' if ends(w, n) else ''))


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


# The driver's own bookkeeping in its shared memory, not GPU state: the
# event records ("EVTLIVE", "EVTINIT") and the heap records that follow the
# pattern {address, 0, serial, index, size, 0x19fc, 0x19fc, 0, ...}.
NOISE_WORDS = {0x4c545645, 0x00455649, 0x49545645, 0x0054494e, 0x000019fc}


def state_diff(na, sa, nb, sb, code_words, maxrun=32, maxruns=120):
    """GPU memory after case NA against after case NB, as 32-bit words:
    the runs that differ, leaving out the USSE programs found (CODE_WORDS,
    their word addresses) and runs next to the driver's bookkeeping -- the
    state words, PDS programs, vertex data and constants the two draws set
    differently.  Neighbouring cases differ least."""
    print('== %s -> %s: what differs, USSE programs and driver bookkeeping left out' % (na, nb))
    shown, more, noise = 0, 0, 0
    for addr in sorted(set(sa) | set(sb)):
        a, b = sa.get(addr, bytes(PAGE)), sb.get(addr, bytes(PAGE))
        if a == b:
            continue
        wa, wb = struct.unpack('<1024I', a), struct.unpack('<1024I', b)
        diff = [wa[k] != wb[k] and (addr + 4 * k) not in code_words for k in range(1024)]
        k = 0
        while k < 1024:
            if not diff[k]:
                k += 1
                continue
            j = k
            while j < 1024 and any(diff[j:j + 4]):
                j += 1
            near = set(wa[max(0, k - 8):j + 8]) | set(wb[max(0, k - 8):j + 8])
            if near & NOISE_WORDS:
                noise += 1
                k = j
                continue
            if shown >= maxruns:
                more += 1
                k = j
                continue
            shown += 1
            print('   %08x  %d word%s' % (addr + 4 * k, j - k, '' if j - k == 1 else 's'))
            if j - k <= maxrun:
                for r in range(k, j, 8):
                    e = min(r + 8, j)
                    print('     - ' + ' '.join('%08x' % w for w in wa[r:e]))
                    print('     + ' + ' '.join('%08x' % w for w in wb[r:e]))
            k = j
    if more:
        print('   ... %d more runs' % more)
    print('   (%d runs of driver bookkeeping not shown)' % noise)


STATE_MARK = 0x3727c5ac     # f32(1e-5), word 17 of the TA's full state (tools/sgx/frame.py)


def state_blocks(name, case_pages, mem_after):
    """the 21-word full state each draw of the case wrote (frame.py's
    full_state: ISP A/B, the pixel program's PDS pointers, tile clip,
    viewport, then 0x0a001000, 1e-5, 0x00088000, 0x39, 0x3 in sgx2d's
    frame), found by its word 17, among the bytes the case changed"""
    hits = []
    for addr in sorted(case_pages):
        b = mem_after[addr]
        ws = struct.unpack('<1024I', b)
        for k in range(17, 1024 - 3):
            if ws[k] == STATE_MARK:
                hits.append((addr + 4 * (k - 17), ws[k - 17:k + 4]))
    print('== %s: %d full state block%s' % (name, len(hits), '' if len(hits) == 1 else 's'))
    for at, ws in hits:
        print('   %08x  %s' % (at, ' '.join('%08x' % w for w in ws[:7])))
        print('             %s' % ' '.join('%08x' % w for w in ws[7:15]))
        print('             %s' % ' '.join('%08x' % w for w in ws[15:]))
        print('   vertex size %d words; varyings %s; pixel PDS info %08x; '
              'pixel PDS %08x (size field %d)' % (
                  ws[16] >> 24, varyings(ws[19], ws[20]), ws[5], tag5_va(ws[6]), ws[6] >> 27 & 3))


def varyings(fmt, half):
    """words 19 and 20 of the full state: three bits a varying (the
    components after the first: 001 two, 011 three, 111 four -- a float
    takes two), one bit a varying in word 20 (set: F16, clear: F32)"""
    out = []
    for i in range(10):
        f = fmt >> 3 * i & 7
        if not f:
            break
        out.append('%d%s' % ({1: 2, 3: 3, 7: 4}.get(f, f), 'h' if half >> i & 1 else 'f'))
    return '[%s]' % ' '.join(out) if out else 'none'


def tag5_va(w):
    """a PDS data pointer back to its GPU address (frame.py's tag5: bits
    26:0 are address >> 4, bit 31 implied)"""
    return 0x80000000 | (w & 0x07ffffff) << 4


# PDS instruction words iOS's GL driver writes (tools/sgx/pds.py)
PDS_NAMES = {0xaf000000: 'end', 0x070001b5: 'doutu row0 (then iterate/fetch)',
             0x07000185: 'doutu row0', 0x070401a5: 'doutu row1 (after)',
             0x07018113: 'dma row0', 0x07040c12: 'iterate, control w3 (texture)',
             0x07040c02: 'iterate, control w3', 0x07000c02: 'iterate (background)',
             0x07041004: 'texture fetch, state row1', 0x67800072: 'fetch index'}


def pds_name(w):
    if w in PDS_NAMES:
        return PDS_NAMES[w]
    if w & 0xffc0ffff == 0x2f0091a3:
        return 'fetch attribute, row %d' % ((w >> 16 & 0x3f) // 4)
    if w >> 24 == 0x07:
        return 'dout %03x, operands at %03x' % (w & 0xfff, w >> 12 & 0xfff)
    return ''


def pds_programs(name, case_pages, mem_after, targets):
    """the PDS programs the case's draw wrote that start a USSE program
    (TARGETS: address -> kind, every program seen up to this case): found
    by their DOUTU word ((address - code base) / 8 << 4 | selector; the
    code base 64 KiB-aligned) and the PDS shape (data rows, then
    instruction words up to an end, 0xaf000000)"""
    print('== %s' % name)
    for addr in sorted(case_pages):
        ws = struct.unpack('<1024I', mem_after[addr])
        for k in range(1024):
            w = ws[k]
            if not w or w & 0xf not in (3, 8) or k + 1 >= 1024 or ws[k + 1] >= 0x80:
                continue
            for at, what in targets.items():
                base = at - (w >> 4) * 8
                if base & 0xffff or base < 0:
                    continue
                end = next((e for e in range(k + 1, min(k + 40, 1024)) if ws[e] == 0xaf000000), None)
                if end is None:
                    continue
                first = k & ~3          # back over the data rows before the DOUTU's
                while first >= 4 and any(ws[first - 4:first]) and 0xaf000000 not in ws[first - 4:first] \
                        and k - first < 12:
                    first -= 4
                print('-- PDS at CPU %08x, starts the %s program at %08x (code base CPU %08x)' %
                      (addr + 4 * first, what, at, base))
                for e in range(first, end + 1):
                    print('   %08x  %08x  %s%s' % (addr + 4 * e, ws[e], pds_name(ws[e]) if e > k else '',
                                                  '  <- doutu' if e == k else ''))


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
    results, after, changed = [], {}, {}
    for name in order:
        path = os.path.join(d, name + '.pages')
        changed[name] = pages(path) if os.path.exists(path) else {}
        results.append((name, analyse(mem, changed[name]) if os.path.exists(path) else None))
        after[name] = dict(mem)
    if '--diff' in sys.argv:
        names = [a for a in sys.argv[2:] if not a.startswith('--')]
        if len(names) != 2 or not all(n in after for n in names):
            raise SystemExit('--diff takes two case names (as in log.txt)')
        code_words = set()
        for _, r in results:
            for at, p, _ in (r[0] + r[1]) if r else []:
                code_words.update(at + 4 * k for k in range(2 * len(p)))
        return state_diff(names[0], after[names[0]], names[1], after[names[1]], code_words)
    if '--pds' in sys.argv:
        known = {}
        for (name, r) in results:
            known.update((at, kind(p)) for at, p, _ in (r[0] if r else []))
            if r and (not only or any(fnmatch.fnmatchcase(name, o) for o in only)):
                pds_programs(name, changed[name], after[name], dict(known))
        return
    if '--state' in sys.argv:
        for name in order:
            if not only or any(fnmatch.fnmatchcase(name, o) for o in only):
                state_blocks(name, changed[name], after[name])
        return
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
        if only and not any(fnmatch.fnmatchcase(name, o) for o in only):
            for at, p, _ in r[0]:
                last[GROUP[kind(p)]] = name
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
