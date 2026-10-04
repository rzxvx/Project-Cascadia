#!/usr/bin/env python3
"""usse-dis.py -- a disassembler for the SGX543's USSE shader ISA, enough to
find and read the programs iOS's GL driver builds (captured by gltrace; see
docs/research/p105-gpu.md).

USSE instructions are 64 bits; the top 5 bits select the major opcode.  The
encoding here is the public one from the Vita3K project's decoder (GPLv2, like
this repo): each instruction is a 64-char bitstring, bit 63 first, where 0/1
are fixed bits and letters/'-' are operand fields.  We turn the fixed bits into
a (mask, value) matcher and identify instructions by them.  The ALU, move,
pack, test, load and sample instructions also get their operands, as Vita3K's
translator reads them (see "operands" below); the rest print bare.

    usse-dis.py dis  FILE [OFF [N]]   disassemble N instrs from byte OFF
    usse-dis.py scan FILE [MIN]       find runs of >= MIN (default 6) valid,
                                      non-zero, non-illegal instructions
    usse-dis.py words [--second] HEX...   disassemble words (stdin if none);
                                      --second: a secondary program's
"""
import sys

# (mnemonic, bitstring) -- from Vita3K vita3k/shader/src/usse_translator_entry.cpp
OPS = [
    ("VMAD2",   "00000dpps-ry-cbawwwineeeemmookttffgghhhhhhzzjjllllllqqqqqquuuuuu"),
    ("V32NMAD", "00001pppsrrydcbawwwwneeeemmoiittkkllffffffzzzzzzzggghhhhhhjjjjjj"),
    ("V16NMAD", "00010pppsrrydcbawwwwneeeemmoiittkkllffffffzzzzzzzggghhhhhhjjjjjj"),
    ("VMAD",    "00011pppsg1oderaaittnwwwwcbfhzkkjjllmmmmmmqqqquuuuvvxyAAAABBBBBB"),
    ("VDP",     "00011pppsc0oderaagttnwwwwbflllkkhhiijjjjjjzzzzmmmqqqyyyxxxuuuuuu"),
    ("VDUAL",   "0010cgsskdtpuuuunaaalriiiiwwwwmmffeebbbbbbbooohhjqvvxxyyyzzzzzzz"),
    ("VCOMP",   "00110pppsddyenr-aaaaobbccmmff-ttkk--ggggggg-------hhhhhhh---wwww"),
    ("VMOV",    "00111pppstrydecbmmaanoooiwwwwkllffgghhhhjjjjjjqqqqqquuuuuuvvvvvv"),
    ("VPCK",    "01000pppsnuydercaaaaffftttmmmmbbkkllgggggggoohiijjqqqqqqvwwwwwwx"),
    ("VTST",    "01001ppps-oydrceavttiizzmhhhnnbbkkffgggggggwlluuuujjjjjjjqqqqqqq"),
    ("VTSTMSK", "01111ppps-oydtrcevuuiizzm-aa--bbnnkkfffffffwllgggghhhhhhhjjjjjjj"),
    ("VBW",     "01ooopppsnrydecxaaaaittttthhbwkkffggjjjjjjjlllllllmmmmmmmqqqqqqq"),
    ("SOP2",    "10000ppcsnaaderbmooofllggghhhittkkjjqqqqqqquvvwwxyzzzzzzzAAAAAAA"),
    ("SOP2M",   "10010ppmsnccderbowwwwaalllfff-ttkkgguuuuuuu-------hhhhhhhiiiiiii"),
    ("SOP3",    "10001ppcsnooderbmallfgghhhiiikttjjqquuuuuuuvvvvvvvwwwwwwwxxxxxxx"),
    ("I8MAD",   "10011ppcsneedarbmtttuolfghijkqvvwwxxyyyyyyyzzzzzzzAAAAAAABBBBBBB"),
    ("I16MAD",  "10100ppasnredbck-tttmmffoolhhgiijjqquuuuuuuvvvvvvvwwwwwwwxxxxxxx"),
    ("I32MAD",  "10101pps-nrcdeba0tttif00yy000kgghhjjlllllllmmmmmmmoooooooqqqqqqq"),
    ("ILLEGAL22", "10110-----------------------------------------------------------"),
    ("ILLEGAL23", "10111-----------------------------------------------------------"),
    ("ILLEGAL24", "11000-----------------------------------------------------------"),
    ("I8MAD2",  "11001-----------------------------------------------------------"),
    ("I32MAD2", "11010ppp-nssdercbooo00iga0000kttffhhjjjjjjjlllllllmmmmmmmqqqqqqq"),
    ("ILLEGAL27", "11011-----------------------------------------------------------"),
    ("SMP",     "11100pppsn-ymrceffaaddlltbbggkhhiijjoooooooqqqqqqquuuuuuuvvvvvvv"),
    ("PHAS",    "11111010s100eirc--matwwwppppppppbbnn--------xxxxxxoooooooddddddd"),
    ("NOP",     "11111----000-----------101--------------------------------------"),
    ("BR",      "11111ppps000e-----wynba00r----------------iloooooooooooooooooooo"),
    ("SMLSI",   "11111010--01-n--ttttppppssssdrcieeeeeeeeaaaaaaaabbbbbbbbffffffff"),
    ("SMBO",    "11111011--01-n--ddddddddddddssssssssssssrrrrrrrrrrrrcccccccccccc"),
    ("KILL",    "11111001--11000000000pp0000001101111----------------------------"),
    ("LIMM",    "11111100sn10deiiiiiipppmmmmm--tt----uuuuuuuvvvvvvvvvvvvvvvvvvvvv"),
    ("DEPTHF",  "11111011s-11recb----npp---tffa--kkddggggggghhhhhhhiiiiiiijjjjjjj"),
    ("SPEC",    "11111----scc----------------------------------------------------"),
    ("VLDST",   "111oopppsnmycrbakkkkddeetgffihjlqquuvvvvvvvwwwwwwwxxxxxxxzzzzzzz"),
]


def matcher(bits):
    mask = val = 0
    for i, ch in enumerate(bits):        # i=0 is bit 63
        b = 63 - i
        if ch == "0":
            mask |= 1 << b
        elif ch == "1":
            mask |= 1 << b
            val |= 1 << b
    return mask, val


MATCH = [(name, *matcher(bits), bits.count("0") + bits.count("1")) for name, bits in OPS]


def decode(word):
    """Return (mnemonic, fixed_bits) of the best match, or (None, 0)."""
    best = (None, -1)
    for name, mask, val, nfix in MATCH:
        if word & mask == val and nfix > best[1]:
            best = (name, nfix)
    return best


def field(word, bits, letter):
    """Assemble the value of a lettered field (MSB-first in the bitstring)."""
    v = 0
    got = False
    for i, ch in enumerate(bits):
        if ch == letter:
            v = (v << 1) | ((word >> (63 - i)) & 1)
            got = True
    return v if got else None


def bits_of(name):
    for n, b in OPS:
        if n == name:
            return b
    return None


# --- operands -------------------------------------------------------------
# Vita3K's operand decoding (vita3k/shader/src/translator/*.cpp and
# usse_decode_helpers.cpp), boiled down to text.
#
# Registers print in 32-bit units, the numbers VBW and the PDS use.  The
# register fields of vector (F16/F32/C10) operands count 64-bit registers, so
# they are doubled: V16NMAD's dest field 3 is sa6, which the pixel program's
# VBW reads as sa6.  An F16 vec4 takes two registers (sa6 = x,y; sa7 = z,w),
# an F32 vec4 four.  The top temporaries a field can name (the last 4, or 8
# when doubled) are the FP internal registers i0..i3.  A special-bank operand
# is a constant from the hardware's table, c<n> (in an F16 operand c<n>.x and
# .y are the low and high halves of the F32 table's bank-0 word, .z and .w of
# its bank-1 word: F16 c15.y is 1.0, c0.x is 0), or with bit 6 a global, g<n>.
# A secondary program's primary-attribute bank is the secondary attributes:
# what it writes there the pixel program reads as sa, so operands(...,
# second=True) prints it as sa.
#
# Text: OP.TYPE DEST.MASK, SRC... as the instruction computes it (VMAD2's
# mad is DEST = SRC0 * SRC1 + SRC2); a "pN?" in front is a predicate.

def _fields(name, word):
    bs = bits_of(name)
    return lambda letter: field(word, bs, letter)


DEST_BANKS = (("temp", "o", "pa", "idx1"), ("sa", "special", "index", "idx2"))
SRC12_BANKS = (("temp", "o", "pa", "sa"), ("idx1", "special", "imm", "idx2"))
SRC0_BANKS = (("temp", "pa"), ("o", "sa"))


def reg(bank, n, double=False, bits=7, second=False):
    """the register a bank and field value name (see above)"""
    if double and bank not in ("special", "imm"):
        n = (n << 1) & 0xff
    if bank == "temp":
        limit = (1 << bits) - (8 if double else 4)
        if n >= limit:
            return "i%d" % ((n - limit) >> 1 if double else n - limit)
    if bank == "special":
        return "g%d" % (n & 0x3f) if n & 0x40 else "c%d" % n
    if bank == "imm":
        return "#%d" % n
    if second and bank == "pa":
        bank = "sa"
    return "%s%d" % ("r" if bank == "temp" else bank, n)


def internal(r):
    return r[:1] == "i" and r[1:].isdigit()


CHAN = "xyzw012h"           # Vita3K's SwizzleChannel order: x y z w, 0 1 2 0.5
VEC4_STD = ("xxxx", "yyyy", "zzzz", "wwww", "xyzw", "yzww", "xyzz", "xxyz",
            "xyxy", "xywz", "zxyw", "zwzw", "yzxz", "xxyy", "xzww", "xyz1")
VEC4_EXT = ("yzxw", "zwxy", "xzwy", "yyww", "wyzw", "wzwz", "xyzx", "zzww",
            "xwzx", "yyyx", "yyyz", "xzyw", "xxxy", "zyxw", "yyzz", "zzzy")
MAD2_S0 = ("xxxx", "yyyy", "zzzz", "wwww", "xyzw", "yzxw", "xyww", "zwxy")
MAD2_S1 = ("xxxx", "yyyy", "zzzz", "wwww", "xyzw", "xyyz", "yyww", "wyzw")
MAD2_S2 = ("xxxx", "yyyy", "zzzz", "wwww", "xyzw", "xzww", "xxyz", "xyzz")
PRED = ("", "p0? ", "p1? ", "p2? ", "p3? ", "!p0? ", "!p1? ", "pn? ")       # ExtPredicate
VPRED = ("", "p0? ", "p1? ", "p2? ", "!p0? ", "!p1? ", "!p2? ", "pn? ")     # ExtVecPredicate
SPRED = ("", "p0? ", "p1? ", "!p0? ")                                      # ShortPredicate
PCK_T = ("u8", "s8", "o8", "u16", "s16", "f16", "f32", "c10")              # VPCK formats
MOV_T = ("s8", "s16", "s32", "c10", "f16", "f32", "u8", "u16")             # Vita3K DataType
NMAD_OP = ("mul", "add", "frc", "dsx", "dsy", "min", "max", "dp")
COMP_OP = ("rcp", "rsq", "log", "exp")
COMP_T = ("f32", "f16", "c10", "?")
# VTST/VTSTMSK: the operation tested (alu_sel 0: float, by prec f16/f32)
TST_F = (None, None, "add", "frc", "rcp", "rsq", "log", "exp", "dp", "min", "max",
         "dsx", "dsy", "mul", "sub", None)
TST_I = {1: (None, None, None, None, None, None, "add.s16", "sub.s16", "mul.s16",
             "add.u16", "sub.u16", "mul.u16", "add.s32", "add.u32", "sub.s32", "sub.u32"),
         2: ("add.s8", "sub.s8", "add.u8", "sub.u8", "mul.s8", "fpmul.u8", "mul.u8",
             "fpadd.u8", "fpsub.u8") + (None,) * 7,
         3: ("and", "or", "xor", "shl", "shr", "rol", None, "asr") + (None,) * 8}


def mask(m):
    return "".join("xyzw"[i] for i in range(4) if m >> i & 1) or "-"


def mod(m, s):
    return (s, "-" + s, "|%s|" % s, "-|%s|" % s)[m]


def wmask(dest, m, f16):
    """VMAD2/VMOV's write mask as the channels it writes (Vita3K's
    decode_write_mask): an F16 mask bit 0 writes x,y and bit 2 z,w; an F32
    mask keeps x,y; the internal registers take it as it is"""
    if internal(dest):
        return m
    if f16:
        return (0b11 if m & 1 else 0) | (0b1100 if m & 4 else 0)
    return m & 0b11


def rpt(n):
    return " rpt%d" % (n + 1) if n else ""


def op_nmad(name, w, second):
    F = _fields(name, w)
    t = "f16" if name == "V16NMAD" else "f32"
    d = reg(DEST_BANKS[F("d")][F("t")], F("f"), True, 7, second)
    s1 = reg(SRC12_BANKS[F("b")][F("k")], F("h"), True, 7, second)
    s2 = reg(SRC12_BANKS[F("a")][F("l")], F("j"), True, 7, second)
    sw1 = F("z") | F("i") << 7 | F("c") << 9 | F("r") << 10
    sw1 = "".join(CHAN[sw1 >> 3 * k & 7] for k in range(4))
    s2 = "%s.%s" % (s2, VEC4_STD[F("w")])
    return "%s%s.%s %s.%s, %s, %s" % (VPRED[F("p")], NMAD_OP[F("g")], t, d, mask(F("e")),
                                      mod(F("m"), "%s.%s" % (s1, sw1)),
                                      "|%s|" % s2 if F("o") else s2)


def op_vmad2(name, w, second):
    F = _fields(name, w)
    f16 = F("d")
    d = reg(DEST_BANKS[0][F("t")], F("h"), True, 7, second)
    s0 = "%s.%s" % (reg(SRC0_BANKS[0][F("k")], F("l"), True, 7, second), MAD2_S0[F("j") | F("r") << 2])
    s1 = "%s.%s" % (reg(SRC12_BANKS[F("b")][F("f")], F("q"), True, 7, second), MAD2_S1[F("z") | F("i") << 2])
    s2 = "%s.%s" % (reg(SRC12_BANKS[F("a")][F("g")], F("u"), True, 7, second), MAD2_S2[F("w")])
    return "%smad.%s %s.%s, %s, %s, %s" % (SPRED[F("p")], "f16" if f16 else "f32", d,
                                           mask(wmask(d, F("e"), f16)),
                                           "|%s|" % s0 if F("c") else s0,
                                           mod(F("m"), s1), mod(F("o"), s2))


def op_vcomp(name, w, second):
    F = _fields(name, w)
    d = reg(DEST_BANKS[F("e")][F("t")], F("g"), True, 8, second)
    s = reg(SRC12_BANKS[F("r")][F("k")], F("h"), True, 8, second)
    return "%s%s.%s.%s %s.%s, %s%s" % (PRED[F("p")], COMP_OP[F("b")], COMP_T[F("d")], COMP_T[F("c")],
                                       d, mask(F("w")), mod(F("m"), "%s.%s" % (s, "xyzw"[F("f")])),
                                       rpt(F("a")))


def op_vmov(name, w, second):
    F = _fields(name, w)
    t = MOV_T[F("o")]
    double = t in ("c10", "f16", "f32")
    bits = 7 if double else 6
    d = reg(DEST_BANKS[F("d")][F("l")], F("j"), double, bits, second)
    s1 = reg(SRC12_BANKS[F("c")][F("f")], F("u"), double, bits, second)
    m = F("h")
    if t in ("f16", "f32"):
        m = wmask(d, m, t == "f16")
    elif not double:
        m = 1
    sw = "." + VEC4_STD[F("w")] if double else ""
    kind = F("m")
    if kind == 0:
        return "%smov.%s %s.%s, %s%s%s" % (PRED[F("p")], t, d, mask(m), s1, sw, rpt(F("a")))
    s0 = reg(SRC0_BANKS[F("e")][F("k")], F("q"), double, bits, second)
    s2 = reg(SRC12_BANKS[F("b")][F("g")], F("v"), double, bits, second)
    test = ("== 0", "!= 0", "< 0", "<= 0")[F("t") << 1 | F("i")]
    return "%smovc%s.%s %s.%s, %s%s %s ? %s%s : %s%s%s" % (
        PRED[F("p")], ".u8" if kind == 2 else "", t, d, mask(m), s0, sw if F("r") else "",
        test, s1, sw, s2, sw, rpt(F("a")))


def op_vpck(name, w, second):
    F = _fields(name, w)
    sf, df = PCK_T[F("f")], PCK_T[F("t")]
    fl = sf in ("c10", "f16", "f32")
    d = reg(DEST_BANKS[F("d")][F("b")], F("g"), False, 7, second)
    n1 = F("q") if fl else F("v") | F("q") << 1
    s1 = reg(SRC12_BANKS[F("r")][F("k")], n1, fl, 7, second)
    s2bank = SRC12_BANKS[F("c")][F("l")]
    c0 = F("x") | (F("v") if sf == "f32" else F("w") & 1) << 1
    sel = "".join("xyzw"[c] for c in (c0, F("i"), F("j"), F("o")))
    s2 = ", " + reg(s2bank, F("w"), True, 7, second) if sf == "f32" and s2bank != "imm" else ""
    return "%spck.%s.%s %s.%s, %s.%s%s%s%s" % (PRED[F("p")], df, sf, d, mask(F("m")), s1, sel, s2,
                                              " scale" if F("h") else "", rpt(F("a")))


def op_vbw(name, w, second):
    F = _fields(name, w)
    op = {2: ("and", "or"), 3: ("xor", "xor"), 4: ("shl", "rol"), 5: ("shr", "asr")}.get(F("o"))
    if not op:
        return "op1=%d?" % F("o")
    u16 = F("w")
    d = reg(DEST_BANKS[F("d")][F("k")], F("j"), False, 7, second)
    s1 = reg(SRC12_BANKS[F("c")][F("f")], F("m"), False, 7, second)
    bank2 = SRC12_BANKS[F("x")][F("g")]
    rot, inv = F("t"), F("i")
    if bank2 == "imm":
        v = F("q") | F("l") << 7 | F("h") << 14
        size = 16 if u16 else 32
        rot &= size - 1
        if rot:
            v = (v << rot | v >> (size - rot)) & ((1 << size) - 1)
        if inv:
            v = ~v & ((1 << size) - 1)
        s2 = "#0x%x" % v
    else:
        s2 = reg(bank2, F("q"), False, 7, second)
        s2 = ("~" if inv else "") + s2 + (" rol %d" % rot if rot else "")
    return "%s%s%s %s, %s, %s%s" % (PRED[F("p")], op[F("b")], ".u16" if u16 else "", d, s1, s2, rpt(F("a")))


def _tst_op(sel, op, prec):
    if sel == 0:
        o = TST_F[op]
        return "%s.%s" % (o, "f32" if prec else "f16") if o else None
    return TST_I[sel][op]


def _cmp(zero, sign):
    """VTST's condition on the result: Vita3K's names (zero test 1 takes
    equality in, sign test 1 below zero, 2 above, 3 any)"""
    return (("ne", "lt", "gt", "any"), ("eq", "le", "ge", "any"))[zero == 1][sign]


def op_vtst(name, w, second):
    F = _fields(name, w)
    sel = F("l")
    op = _tst_op(sel, F("u"), F("a")) or "op%d.%d" % (sel, F("u"))
    double, bits = sel == 0, 8 if sel == 0 else 7
    s1 = reg(SRC12_BANKS[F("c")][F("k")], F("j"), double, bits, second)
    s2 = reg(SRC12_BANKS[F("e")][F("f")], F("q"), double, bits, second)
    if F("v") and sel == 0:
        s2 += ".x"
    chan = F("h")
    out = "%stst p%d%s = %s(%s, %s) %s 0" % (
        PRED[F("p")], F("n"), ".%s" % "xyzw"[chan] if chan < 4 else " chan%d" % chan,
        op, "-" + s1 if F("r") else s1, s2, _cmp(F("z"), F("i")))
    if F("w"):
        out += ", result to %s" % reg(DEST_BANKS[F("d")][F("b")], F("g"), double, bits, second)
    return out + (" (and)" if F("m") else "") + rpt(F("t"))


def op_vtstmsk(name, w, second):
    F = _fields(name, w)
    sel = F("l")
    op = _tst_op(sel, F("g"), F("e")) or "op%d.%d" % (sel, F("g"))
    double, bits = sel == 0, 8 if sel == 0 else 7
    s1 = reg(SRC12_BANKS[F("r")][F("n")], F("h"), double, bits, second)
    s2 = reg(SRC12_BANKS[F("c")][F("k")], F("j"), double, bits, second)
    if F("v") and sel == 0:
        s2 += ".x"
    d = reg(DEST_BANKS[F("d")][F("b")], F("f"), double, bits, second)
    return "%ststmsk %s = %s(%s, %s) %s 0 ? 1 : 0 (mask type %d)%s%s" % (
        PRED[F("p")], d, op, "-" + s1 if F("t") else s1, s2,
        _cmp(F("z"), F("i")), F("a"), "" if F("w") else " nowb", rpt(F("u")))


def op_vldst(name, w, second):
    F = _fields(name, w)
    op = {1: "ld", 2: "st"}.get(F("o"), "ldst%d" % F("o"))
    t = ("32", "16", "8", "?")[F("f")]
    d = reg("pa" if F("t") else "temp", F("v"), False, 7, second)
    s0 = reg(SRC0_BANKS[F("r")][F("h")], F("w"), False, 7, second)
    s1 = reg(SRC12_BANKS[F("b")][F("q")], F("x"), False, 7, second)
    s2 = reg(SRC12_BANKS[F("a")][F("u")], F("z"), False, 7, second)
    return "%s%s%s %s, [%s, %s, %s] drc%d" % (PRED[F("p")], op, t, d, s0, s1, s2, F("l"))


SMP_DIM = ("1d", "2d", "3d", "dim3")
SMP_LOD = ("", " bias", " lod", " grad")
SMP_OUT = ("raw", "?", "f16", "f32")


def op_smp(name, w, second):
    F = _fields(name, w)
    d = reg("pa" if F("t") else "temp", F("o"), False, 7, second)
    s0 = reg(SRC0_BANKS[F("r")][F("k")], F("q"), True, 8, second)
    s1 = reg(SRC12_BANKS[F("c")][F("i")], F("u"), True, 8, second)
    s2 = reg(SRC12_BANKS[F("e")][F("j")], F("v"), True, 8, second)
    return "%ssmp%s.%s.%s %s, %s, state %s%s drc%d%s" % (
        PRED[F("p")], SMP_DIM[F("d")], SMP_OUT[F("f")], COMP_T[F("g")], d, s0, s1,
        ", " + s2 if F("l") else "", F("h"), SMP_LOD[F("l")] + (" sb%d" % F("b") if F("b") else ""))


def op_phas(name, w, second):
    F = _fields(name, w)
    addr = F("x") << 14 | F("o") << 7 | F("d")
    out = "wait %d temps %d" % (F("w"), F("p"))
    if F("i") and addr:
        out += ", next at %d (code base + 0x%x)" % (addr, addr * 8)
    return out + (" end" if F("e") else "") + (" mode %d" % F("m") if F("m") else "")


def op_br(name, w, second):
    """bit 38 (br_type) set: the offset is relative, in instructions from
    this one, 20 bits signed (Vita3K's usse_program_analyzer.cpp)"""
    F = _fields(name, w)
    off = F("o")
    if F("r"):
        off = off - (1 << 20) if off & 1 << 19 else off
        to = "%+d" % off
    else:
        to = "@%d" % off
    return "%s%s %s%s" % (PRED[F("p")], "call" if F("a") else "br", to,
                         " (any)" if F("i") else " (all)" if F("l") else "")


def op_limm(name, w, second):
    return "r%d <- #0x%08x" % ((w >> 21) & 0x7f, limm_imm(w))


OPERANDS = {"V16NMAD": op_nmad, "V32NMAD": op_nmad, "VMAD2": op_vmad2, "VCOMP": op_vcomp,
            "VMOV": op_vmov, "VPCK": op_vpck, "VBW": op_vbw, "VTST": op_vtst,
            "VTSTMSK": op_vtstmsk, "VLDST": op_vldst, "SMP": op_smp, "PHAS": op_phas,
            "LIMM": op_limm, "BR": op_br}


def limm_imm(word):
    """LIMM's 32-bit immediate.  The pattern's letters do not follow it: bits
    49:44 are imm[31:26], bits 40:36 imm[25:21], bits 20:0 imm[20:0], and
    bits 27:21 are the destination register.  Checked on the microkernel's
    LIMM rN, #0xdeadbeef fills (fc237150000dbeef, ...004dbeef, ...008dbeef)."""
    return (((word >> 44) & 0x3f) << 26) | (((word >> 36) & 0x1f) << 21) | (word & 0x1fffff)


def operands(name, word, second=False):
    """NAME's operands as text, with two spaces in front; '' when not known.
    SECOND: the word is from a secondary program (pa is sa there)."""
    fn = OPERANDS.get(name)
    if not fn:
        return ""
    try:
        return "  " + fn(name, word, second)
    except (IndexError, KeyError, TypeError) as e:     # a field this does not know
        return "  (%s)" % e


def dis(data, off, n):
    for k in range(n):
        p = off + k * 8
        if p + 8 > len(data):
            break
        w = int.from_bytes(data[p:p + 8], "little")
        name, nfix = decode(w)
        print(f"  +{p:04x}: {w:016x}  {name or '???':10}{operands(name, w) if name else ''}")


def valid(w):
    if w == 0:
        return False
    name, _ = decode(w)
    return name is not None and not name.startswith("ILLEGAL")


def scan(data, minrun):
    n = len(data) // 8
    words = [int.from_bytes(data[i * 8:i * 8 + 8], "little") for i in range(n)]
    i = 0
    hits = 0
    while i < n:
        if valid(words[i]):
            j = i
            while j < n and valid(words[j]):
                j += 1
            if j - i >= minrun:
                print(f"\n== run at +0x{i*8:04x}, {j-i} instrs")
                for k in range(i, min(j, i + 40)):
                    name, _ = decode(words[k])
                    print(f"  +{k*8:04x}: {words[k]:016x}  {name}")
                hits += 1
            i = j
        else:
            i += 1
    if not hits:
        print("no USSE-looking runs found (try LE/BE, or the wrong buffer)")


def words(args):
    """disassemble 64-bit words given as hex (arguments, or stdin when
    none); '--second' reads them as a secondary program's"""
    second = "--second" in args
    text = " ".join(a for a in args if a != "--second") or sys.stdin.read()
    for tok in text.replace(",", " ").split():
        try:
            w = int(tok, 16)
        except ValueError:
            continue
        name, _ = decode(w)
        print(f"  {w:016x}  {name or '???':10}{operands(name, w, second) if name else ''}")


def main():
    cmd = sys.argv[1]
    if cmd == "words":
        return words(sys.argv[2:])
    fn = sys.argv[2]
    data = open(fn, "rb").read()
    if cmd == "dis":
        off = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0
        n = int(sys.argv[4], 0) if len(sys.argv) > 4 else (len(data) - off) // 8
        dis(data, off, n)
    elif cmd == "scan":
        scan(data, int(sys.argv[3]) if len(sys.argv) > 3 else 6)


if __name__ == "__main__":
    main()
