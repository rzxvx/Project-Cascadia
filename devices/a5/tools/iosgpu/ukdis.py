#!/usr/bin/env python3
"""ukdis.py -- operand-level disassembler for the SGX543 microkernel (EDM code).

ALU/load-store field layouts are Vita3K's (usse_translator_entry.cpp, via
usse-dis.py).  The microkernel-only encodings were worked out from the
microkernel itself (docs/research/p105-gpu.md, "Reading the microkernel"):

  fe2x   HW register access.  bit 51 store, bit 49 immediate value (bits 13:7),
         bit 48 immediate address: word address = bit16<<9 | bits15:14<<7 |
         bits 6:0, bank = bits 20:19 (0 broadcast, 1 master, 2 own core);
         otherwise the address is in r(bits 6:0), a WORD address from
         0x35100000 (0x000+ broadcast, 0x1000+ master, 0x2000+ core 0, ...).
         Loads: destination bits 27:21, bits 39:32 = fetch slot (wdf N).
  BR     bit 38 set = relative (signed 20-bit, in instructions), else an
         absolute instruction index from the start of the code buffer;
         bit 41 = call.  f8000100 = save the link register in r(27:21),
         f80000c0 = return through r(13:7), f8000080 = plain return.

    ukdis.py FILE OFF N [BASE]   BASE = byte offset of FILE[0] in the code
                                 buffer (default 0x1000: the microkernel's
                                 code starts there)
"""
import os, sys, struct
import importlib.util
_here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('ud', os.path.join(_here, 'usse-dis.py'))
ud = importlib.util.module_from_spec(spec); spec.loader.exec_module(ud)

PC = [0]
PRED = ['', 'p0 ', 'p1 ', 'p2 ', 'p3 ', '!p0 ', '!p1 ', 'pn ']

def F(word, name, letter):
    return ud.field(word, ud.bits_of(name), letter)

def dreg(n, bank, ext):
    b = (['sa', 'sp', 'i', 'x2'] if ext else ['r', 'o', 'pa', 'x1'])[bank]
    return f'{b}{n}'

def s12reg(n, bank, ext):
    b = (['x1', 'sp', '#', 'x2'] if ext else ['r', 'o', 'pa', 'sa'])[bank]
    return f'#{n:#x}' if b == '#' else f'{b}{n}'

def s0reg(n, bank, ext):
    b = (['o', 'sa'] if ext else ['r', 'pa'])[bank & 1]
    return f'{b}{n}'

TST = {1: {6: 'add16', 7: 'sub16', 8: 'mul16', 9: 'addu16', 10: 'subu16', 11: 'mulu16',
           12: 'add', 13: 'addu', 14: 'sub', 15: 'subu'},
       3: {0: 'and', 1: 'or', 2: 'xor', 3: 'shl', 4: 'shr', 5: 'rol', 7: 'asr'},
       2: {0: 'add8', 1: 'sub8', 2: 'addu8', 3: 'subu8'}}
CMP = [['ne', 'lt', 'gt', 'inv'], ['eq', 'le', 'ge', 'inv']]

RN = {0x408: 'ISP_RGN_BASE', 0x480: 'ISP_ZLSCTL', 0x484: 'ZLOAD_BASE', 0x488: 'ZSTORE_BASE',
      0xa5c: 'PIXPDS_DATA', 0xa60: 'PIXPDS_SZ', 0xa64: 'PIXPDS_INFO', 0xa68: 'EVPDS_A', 0xa6c: 'EVPDS_B', 0xa70: 'EVPDS_C',
      0x4400: 'MASTER_ISP_STATUS', 0x451c: 'MASTER_ISP_RGN'}   # kext dump fn 0x80bf9230
# from the DDK's sgx544defs.h / sgxmpdefs.h (GPL headers)
for _o, _n in [
    (0x0000, 'CLKGATECTL'), (0x0004, 'CLKGATECTL2'), (0x0008, 'CLKGATESTATUS'),
    (0x000c, 'CLKGATECTLOVR'), (0x001c, 'POWER'), (0x0020, 'CORE_ID'), (0x0024, 'CORE_REVISION'),
    (0x0028, 'DESIGNER_REV_FIELD1'), (0x002c, 'DESIGNER_REV_FIELD2'), (0x0080, 'SOFT_RESET'),
    (0x0110, 'EVENT_HOST_ENABLE2'), (0x0114, 'EVENT_HOST_CLEAR2'), (0x0118, 'EVENT_STATUS2'),
    (0x012c, 'EVENT_STATUS'), (0x0130, 'EVENT_HOST_ENABLE'), (0x0134, 'EVENT_HOST_CLEAR'),
    (0x0144, 'TIMER'), (0x0a0c, 'USE_CODE_BASE_0'), (0x0a10, 'USE_CODE_BASE_1'),
    (0x0a14, 'USE_CODE_BASE_2'), (0x0a18, 'USE_CODE_BASE_3'), (0x0a1c, 'USE_CODE_BASE_4'),
    (0x0a20, 'USE_CODE_BASE_5'), (0x0a24, 'USE_CODE_BASE_6'), (0x0a28, 'USE_CODE_BASE_7'),
    (0x0a2c, 'USE_CODE_BASE_8'), (0x0a30, 'USE_CODE_BASE_9'), (0x0a34, 'USE_CODE_BASE_10'),
    (0x0a38, 'USE_CODE_BASE_11'), (0x0a3c, 'USE_CODE_BASE_12'), (0x0a40, 'USE_CODE_BASE_13'),
    (0x0a44, 'USE_CODE_BASE_14'), (0x0a48, 'USE_CODE_BASE_15'), (0x0ab0, 'EVENT_KICK1'),
    (0x0ac0, 'EVENT_KICK2'), (0x0ac4, 'EVENT_KICKER'), (0x0ac8, 'EVENT_KICK'),
    (0x0acc, 'EVENT_TIMER'), (0x0ad0, 'PDS_INV0'), (0x0ad4, 'PDS_INV1'), (0x0ad8, 'EVENT_KICK3'),
    (0x0adc, 'PDS_INV3'), (0x0ae0, 'PDS_INV_CSC'), (0x0c00, 'BIF_CTRL'), (0x0c04, 'BIF_INT_STAT'),
    (0x0c08, 'BIF_FAULT'), (0x0c0c, 'BIF_TILE0'), (0x0c10, 'BIF_TILE1'), (0x0c14, 'BIF_TILE2'),
    (0x0c18, 'BIF_TILE3'), (0x0c1c, 'BIF_TILE4'), (0x0c20, 'BIF_TILE5'), (0x0c24, 'BIF_TILE6'),
    (0x0c28, 'BIF_TILE7'), (0x0c2c, 'BIF_TILE8'), (0x0c30, 'BIF_TILE9'), (0x0c34, 'BIF_CTRL_INVAL'),
    (0x0c38, 'BIF_DIR_LIST_BASE1'), (0x0c3c, 'BIF_DIR_LIST_BASE2'), (0x0c40, 'BIF_DIR_LIST_BASE3'),
    (0x0c44, 'BIF_DIR_LIST_BASE4'), (0x0c48, 'BIF_DIR_LIST_BASE5'), (0x0c4c, 'BIF_DIR_LIST_BASE6'),
    (0x0c50, 'BIF_DIR_LIST_BASE7'), (0x0c74, 'BIF_BANK_SET'), (0x0c78, 'BIF_BANK0'),
    (0x0c7c, 'BIF_BANK1'), (0x0c84, 'BIF_DIR_LIST_BASE0'), (0x0c90, 'BIF_TA_REQ_BASE'),
    (0x0ca8, 'BIF_MEM_REQ_STAT'), (0x0cac, 'BIF_3D_REQ_BASE'), (0x0cb0, 'BIF_ZLS_REQ_BASE'),
    (0x0cb4, 'BIF_BANK_STATUS'), (0x0cd0, 'BIF_MMU_CTRL'), (0x0e04, '2D_BLIT_STATUS'),
    (0x0e10, '2D_VIRTUAL_FIFO_0'), (0x0e14, '2D_VIRTUAL_FIFO_1'), (0x0f44, 'BREAKPOINT0_START'),
    (0x0f48, 'BREAKPOINT0_END'), (0x0f4c, 'BREAKPOINT0'), (0x0f50, 'BREAKPOINT1_START'),
    (0x0f54, 'BREAKPOINT1_END'), (0x0f58, 'BREAKPOINT1'), (0x0f5c, 'BREAKPOINT2_START'),
    (0x0f60, 'BREAKPOINT2_END'), (0x0f64, 'BREAKPOINT2'), (0x0f68, 'BREAKPOINT3_START'),
    (0x0f6c, 'BREAKPOINT3_END'), (0x0f70, 'BREAKPOINT3'), (0x0f74, 'BREAKPOINT_READ'),
    (0x0f78, 'PARTITION_BREAKPOINT_TRAP'), (0x0f7c, 'PARTITION_BREAKPOINT'),
    (0x0f80, 'PARTITION_BREAKPOINT_TRAP_INFO0'), (0x0f84, 'PARTITION_BREAKPOINT_TRAP_INFO1'),
    (0x0f88, 'PIPE0_BREAKPOINT_TRAP'), (0x0f8c, 'PIPE0_BREAKPOINT'),
    (0x0f90, 'PIPE0_BREAKPOINT_TRAP_INFO0'), (0x0f94, 'PIPE0_BREAKPOINT_TRAP_INFO1'),
    (0x0f98, 'PIPE1_BREAKPOINT_TRAP'), (0x0f9c, 'PIPE1_BREAKPOINT'),
    (0x0fa0, 'PIPE1_BREAKPOINT_TRAP_INFO0'), (0x0fa4, 'PIPE1_BREAKPOINT_TRAP_INFO1'),
    (0x4000, 'MASTER_CORE'), (0x4010, 'MASTER_CORE_ID'), (0x4014, 'MASTER_CORE_REVISION'),
    (0x4080, 'MASTER_SOFT_RESET'), (0x4c00, 'MASTER_BIF_CTRL'), (0x4c34, 'MASTER_BIF_CTRL_INVAL'),
    (0x4cd0, 'MASTER_BIF_MMU_CTRL'), (0x4d00, 'MASTER_SLC_CTRL'),
    (0x4d04, 'MASTER_SLC_CTRL_BYPASS'), (0x4d08, 'MASTER_SLC_CTRL_USSE_INVAL'),
    (0x4d28, 'MASTER_SLC_CTRL_INVAL'), (0x4d2c, 'MASTER_SLC_CTRL_FLUSH'),
    (0x4d34, 'MASTER_SLC_CTRL_FLUSH_INV'), (0x4f18, 'MASTER_BREAKPOINT_READ'),
    (0x4f1c, 'MASTER_BREAKPOINT_TRAP'), (0x4f20, 'MASTER_BREAKPOINT'),
    (0x4f24, 'MASTER_BREAKPOINT_TRAP_INFO0'), (0x4f28, 'MASTER_BREAKPOINT_TRAP_INFO1'),
]:
    RN.setdefault(_o, _n)

def regtxt(byteoff, bank):
    b = ['B', 'M', 'L', 'X'][bank]
    full = byteoff + (0x4000 if bank == 1 else 0)
    n = RN.get(full) or RN.get(byteoff)
    return f'{b}:{byteoff:#05x}' + (f'({n})' if n else '')

def regname(w):
    """word address -> 'bank+byteoff' text for HW register constants"""
    bank = {0: 'B', 1: 'M', 2: 'C0', 3: 'C1'}.get(w >> 12, '?')
    return f'{bank}+{(w & 0xfff) * 4:#05x}'

def op(word, base):
    name, _ = ud.decode(word)
    top = word >> 48
    if word == 0:
        return 'zero'
    if name == 'LIMM':
        p = PRED[(word >> 41) & 7] if False else ''
        bank = (word >> 32) & 3; ext = (word >> 51) & 1
        n = (word >> 21) & 0x7f
        imm = ud.limm_imm(word)
        e = ' END' if (word >> 50) & 1 else ''
        return f'{p}mov {dreg(n, bank, ext)}, #{imm:#x}{e}'
    if (word >> 59) == 0x1f and ((word >> 52) & 7) == 0 and ((word >> 23) & 0 == 0) and ((word >> 39) & 3) != 0 and ud.decode(word)[0] in ('BR', 'SPEC', None):
        p = PRED[(word >> 56) & 7]
        bo = (word >> 39) & 3
        if bo == 1:
            return f'{p}ret'
        return f'{p}linkop{bo} r{(word >> 21) & 0x7f}  [{word:016x}]'
    if name == 'BR':
        p = PRED[(word >> 56) & 7]
        off = word & 0xfffff
        link = (word >> 41) & 1
        if (word >> 38) & 1:
            rel = off - (1 << 20) if off & (1 << 19) else off
            tgt = PC[0] + base + rel * 8
            off = tgt // 8
        else:
            tgt = off * 8
        s = f'{p}{"call" if link else "br"} {tgt - base:#x}' + (f' (idx {off:#x})')
        if (word >> 51) & 1: s += ' EXC'
        if (word >> 42) & 1: s += ' MON'
        return s
    if name == 'VBW':
        o1 = F(word, 'VBW', 'o'); o2 = F(word, 'VBW', 'b')
        opn = {2: 'or' if o2 else 'and', 3: 'xor', 4: 'rol' if o2 else 'shl', 5: 'asr' if o2 else 'shr'}.get(o1, f'bw{o1}')
        p = PRED[F(word, 'VBW', 'p')]
        d = dreg(F(word, 'VBW', 'j'), F(word, 'VBW', 'k'), F(word, 'VBW', 'd'))
        s1 = s12reg(F(word, 'VBW', 'm'), F(word, 'VBW', 'f'), F(word, 'VBW', 'c'))
        x = F(word, 'VBW', 'x'); b2 = F(word, 'VBW', 'g')
        rot = F(word, 'VBW', 't'); inv = F(word, 'VBW', 'i')
        rpt = F(word, 'VBW', 'a')
        if x and b2 == 2:
            v = F(word, 'VBW', 'q') | (F(word, 'VBW', 'l') << 7) | (F(word, 'VBW', 'h') << 14)
            if rot: v = ((v << rot) | (v >> (32 - rot))) & 0xffffffff
            if inv: v = (~v) & 0xffffffff
            s2 = f'#{v:#x}'
        else:
            s2 = s12reg(F(word, 'VBW', 'q'), b2, x)
            if rot: s2 += f'<<<{rot}'
            if inv: s2 = '~' + s2
        e = ' END' if F(word, 'VBW', 'e') else ''
        r = f' x{rpt+1}' if rpt else ''
        if F(word, 'VBW', 'w'): opn += '16'
        return f'{p}{opn} {d}, {s1}, {s2}{r}{e}'
    if name == 'VTST':
        p = PRED[F(word, 'VTST', 'p')]
        sel = F(word, 'VTST', 'l'); aop = F(word, 'VTST', 'u')
        o = TST.get(sel, {}).get(aop, f'alu{sel}.{aop}')
        s1 = s12reg(F(word, 'VTST', 'j'), F(word, 'VTST', 'k'), F(word, 'VTST', 'c'))
        s2 = s12reg(F(word, 'VTST', 'q'), F(word, 'VTST', 'f'), F(word, 'VTST', 'e'))
        if F(word, 'VTST', 'r'): s1 = '-' + s1
        zt = F(word, 'VTST', 'z'); st = F(word, 'VTST', 'i')
        cmp = CMP[1 if zt == 1 else 0][st] if zt else 'always'
        pd = F(word, 'VTST', 'n')
        wb = ''
        if F(word, 'VTST', 'w'):
            wb = ', ' + dreg(F(word, 'VTST', 'g'), F(word, 'VTST', 'b'), F(word, 'VTST', 'd')) + ' ='
        return f'{p}tst p{pd}{wb} ({s1} {o} {s2}) {cmp} 0'
    if name == 'I32MAD':
        p = PRED[F(word, 'I32MAD', 'p') & 3]
        d = dreg(F(word, 'I32MAD', 'l'), F(word, 'I32MAD', 'g'), F(word, 'I32MAD', 'd'))
        s0 = s0reg(F(word, 'I32MAD', 'm'), F(word, 'I32MAD', 'k'), 0)
        s1 = s12reg(F(word, 'I32MAD', 'o'), F(word, 'I32MAD', 'h'), F(word, 'I32MAD', 'b'))
        s2 = s12reg(F(word, 'I32MAD', 'q'), F(word, 'I32MAD', 'j'), F(word, 'I32MAD', 'a'))
        if F(word, 'I32MAD', 'r'): s1 += '.hi'
        if F(word, 'I32MAD', 'c'): s2 += '.hi'
        sg = 's' if F(word, 'I32MAD', 'i') else 'u'
        rpt = F(word, 'I32MAD', 't')
        r = f' x{rpt+1}' if rpt else ''
        e = ' END' if F(word, 'I32MAD', 'e') else ''
        return f'{p}imad{sg} {d}, {s0}*{s1} + {s2}{r}{e}'
    if name == 'I32MAD2':
        p = PRED[F(word, 'I32MAD2', 'p')]
        d = dreg(F(word, 'I32MAD2', 'j'), F(word, 'I32MAD2', 't'), F(word, 'I32MAD2', 'd'))
        s0 = s0reg(F(word, 'I32MAD2', 'l'), F(word, 'I32MAD2', 'k'), F(word, 'I32MAD2', 'b'))
        s1 = s12reg(F(word, 'I32MAD2', 'm'), F(word, 'I32MAD2', 'f'), F(word, 'I32MAD2', 'r'))
        s2 = s12reg(F(word, 'I32MAD2', 'q'), F(word, 'I32MAD2', 'h'), F(word, 'I32MAD2', 'c'))
        if F(word, 'I32MAD2', 'g'): s1 = '-' + s1
        if F(word, 'I32MAD2', 'a'): s2 = '-' + s2
        sn = F(word, 'I32MAD2', 's')
        e = ' END' if F(word, 'I32MAD2', 'e') else ''
        return f'{p}imad2 {d}, {s0}*{s1} + {s2} sn{sn}{e}'
    if name == 'VLDST':
        o1 = F(word, 'VLDST', 'o')
        p = PRED[F(word, 'VLDST', 'p')]
        dt = {0: 32, 1: 16, 2: 8}.get(F(word, 'VLDST', 'f'), '?')
        cnt = F(word, 'VLDST', 'k') + 1
        s0 = s0reg(F(word, 'VLDST', 'w'), F(word, 'VLDST', 'h'), F(word, 'VLDST', 'r'))
        b1 = F(word, 'VLDST', 'q'); x1 = F(word, 'VLDST', 'b')
        n1 = F(word, 'VLDST', 'x')
        if x1 and b1 == 2:
            s1 = f'#{n1 * (dt // 8 if dt != "?" else 4):#x}'
        else:
            s1 = s12reg(n1, b1, x1)
        s2 = s12reg(F(word, 'VLDST', 'z'), F(word, 'VLDST', 'u'), F(word, 'VLDST', 'a'))
        am = F(word, 'VLDST', 'd'); md = F(word, 'VLDST', 'e')
        extra = f' am{am}' if am else ''
        extra += f' m{md}' if md else ''
        if F(word, 'VLDST', 'j'): extra += ' nc'
        if F(word, 'VLDST', 'c'): extra += ' cx'
        if F(word, 'VLDST', 'l'): extra += f' drc1'
        if o1 == 1:
            dn = F(word, 'VLDST', 'v')
            db = 'pa' if F(word, 'VLDST', 't') else 'r'
            return f'{p}ld{dt} {db}{dn}' + (f'..+{cnt-1}' if cnt > 1 else '') + f', [{s0} + {s1}] (s2={s2}){extra}'
        if o1 == 2:
            return f'{p}st{dt} [{s0} + {s1}], {s2}' + (f' x{cnt}' if cnt > 1 else '') + extra
        return f'ldst?{o1} {word:016x}'
    if (word >> 59) == 0x1f:
        cat = (word >> 52) & 3
        op2 = (word >> 56) & 7
        sub = (word >> 48) & 0xf
        if cat == 2 and op2 == 6:          # fe2x: HW register access
            st = (word >> 51) & 1; immv = (word >> 49) & 1; imma = (word >> 48) & 1
            ext = (word >> 32) & 0xffff
            if imma:
                wa = (((word >> 16) & 1) << 9) | (((word >> 14) & 3) << 7) | (word & 0x7f)
                bank = (word >> 19) & 3
                A = regtxt(wa * 4, bank)
            else:
                A = f'[r{word & 0x7f}]'
            if st:
                V = f'#{(word >> 7) & 0x7f:#x}' if immv else f'r{(word >> 7) & 0x7f}'
                x = f' (ext {ext:#x})' if ext else ''
                return f'hwst {A}, {V}{x}'
            x = f' slot{ext}' if ext else ''
            return f'hwld r{(word >> 21) & 0x7f}, {A}{x}'
        if cat == 2 and op2 == 1:
            return f'wdf {(word >> 32) & 0xff}' if (word & 0xffffffff) == 0 else f'f92? {word:016x}'
        return f'{name or "???"} {word:016x}'
    return f'{name or "???"} {word:016x}'


def main():
    fn = sys.argv[1]; off = int(sys.argv[2], 0); n = int(sys.argv[3], 0)
    base = int(sys.argv[4], 0) if len(sys.argv) > 4 else 0x1000
    d = open(fn, 'rb').read()
    for k in range(n):
        p = off + k * 8
        if p + 8 > len(d): break
        w = struct.unpack_from('<Q', d, p)[0]
        PC[0] = p
        idx = (p + base) // 8
        print(f'{p:05x} [{idx:04x}] {w:016x}  {op(w, base)}')

if __name__ == '__main__':
    main()
