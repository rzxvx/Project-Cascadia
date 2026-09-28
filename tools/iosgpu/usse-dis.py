#!/usr/bin/env python3
"""usse-dis.py -- a disassembler for the SGX543's USSE shader ISA, enough to
find and read the programs iOS's GL driver builds (captured by gltrace; see
docs/research/p105-gpu.md).

USSE instructions are 64 bits; the top 5 bits select the major opcode.  The
encoding here is the public one from the Vita3K project's decoder (GPLv2, like
this repo): each instruction is a 64-char bitstring, bit 63 first, where 0/1
are fixed bits and letters/'-' are operand fields.  We turn the fixed bits into
a (mask, value) matcher and identify instructions by them.  Operand decoding is
added field by field as it is worked out; for now this gives the opcode, the
raw word, and the end/predicate flags -- enough to locate a program and see its
shape.

    usse-dis.py dis  FILE [OFF [N]]   disassemble N instrs from byte OFF
    usse-dis.py scan FILE [MIN]       find runs of >= MIN (default 6) valid,
                                      non-zero, non-illegal instructions
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


FMT = {0: "f32", 1: "f16", 2: "c10", 3: "u8", 4: "s8", 5: "u16", 6: "s16"}

# 2-bit bank-select -> register bank, from Vita3K's usse_decode_helpers.cpp.
# The ext bit picks the alternate set; different operand roles decode differently.
def bank_dest(sel, ext):
    return (["sa", "special", "index", "idx2"] if ext else ["temp", "o", "pa", "idx1"])[sel]
def bank_src12(sel, ext):
    return (["idx1", "special", "imm", "idx2"] if ext else ["temp", "o", "pa", "sa"])[sel]
def bank_src0(sel, ext):
    return (["o", "sa"] if ext else ["temp", "pa"])[sel & 1]

# the NMAD-group operation select (op2), from Vita3K's Opcode order
NMAD = {0: "VMUL", 1: "VADD", 2: "VFRC", 3: "VDSX", 4: "VDSY", 5: "VMIN", 6: "VMAX", 7: "VDP"}

# per-opcode operand fields, from Vita3K's decoder.  Each entry:
#   (letters, label, kind[, ext_letter])
# kind: 'fmt' pack format, 'nmad' ALU op, 'dbank'/'s12bank'/'s0bank' a register
# bank (with its ext bit given by ext_letter), else a plain number.
OPERANDS = {
    "VMOV": [("ooo", "dtype", "fmt"), ("ll", "dbank", "dbank", "d"), ("jjjjjj", "dn", None),
             ("hhhh", "dmask", None), ("ff", "sbank", "s12bank", "c"), ("uuuuuu", "sn", None),
             ("e", "end", None)],
    "VPCK": [("fff", "sfmt", "fmt"), ("ttt", "dfmt", "fmt"), ("mmmm", "dmask", None),
             ("bb", "dbank", "dbank", "d"), ("ggggggg", "dn", None),
             ("kk", "s1bank", "s12bank", "r"), ("qqqqqq", "s1n", None),
             ("ll", "s2bank", "s12bank", "c"), ("wwwwww", "s2n", None), ("e", "end", None)],
    "V16NMAD": [("ggg", "op2", "nmad"), ("eeee", "dmask", None),
                ("tt", "dbank", "dbank", "d"), ("ffffff", "dn", None),
                ("kk", "s1bank", "s12bank", "b"), ("hhhhhh", "s1n", None),
                ("ll", "s2bank", "s12bank", "a"), ("jjjjjj", "s2n", None)],
    "V32NMAD": [("ggg", "op2", "nmad"), ("eeee", "dmask", None),
                ("tt", "dbank", "dbank", "d"), ("ffffff", "dn", None),
                ("kk", "s1bank", "s12bank", "b"), ("hhhhhh", "s1n", None),
                ("ll", "s2bank", "s12bank", "a"), ("jjjjjj", "s2n", None)],
}
BANKFN = {"dbank": bank_dest, "s12bank": bank_src12, "s0bank": bank_src0}


def field_multi(word, bits, letters):
    """Value of a run of one repeated letter (letters is like 'jjjjjj')."""
    return field(word, bits, letters[0])


def vpck_swizzle(word):
    """VPCK src1 component select -> a swizzle like 'rgba'.  comp0's high bit
    comes from src2_n&1 when the source is not F32 (Vita3K vpck())."""
    bs = bits_of("VPCK")
    ch = "rgba"
    s2n = field(word, bs, "w")
    sfmt = field(word, bs, "f")
    hi = (field(word, bs, "v") if sfmt == 0 else (s2n & 1))
    c0 = field(word, bs, "x") | (hi << 1)
    sel = [c0, field(word, bs, "i"), field(word, bs, "j"), field(word, bs, "o")]
    return "".join(ch[i] for i in sel)


def operands(name, word):
    spec = OPERANDS.get(name)
    if not spec:
        return ""
    bs = bits_of(name)
    out = []
    for entry in spec:
        letters, label, kind = entry[0], entry[1], entry[2]
        v = field(word, bs, letters[0])
        if v is None:
            continue
        if kind in BANKFN:
            ext = field(word, bs, entry[3]) or 0
            out.append(f"{label}={BANKFN[kind](v, ext)}")
        elif kind == "fmt":
            out.append(f"{label}={FMT.get(v, v)}")
        elif kind == "nmad":
            out.append(f"{label}={NMAD.get(v, v)}")
        else:
            out.append(f"{label}={v}")
    if name == "VPCK":
        out.append(f"src1.{vpck_swizzle(word)}")
    return "  " + " ".join(out)


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


def main():
    cmd, fn = sys.argv[1], sys.argv[2]
    data = open(fn, "rb").read()
    if cmd == "dis":
        off = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0
        n = int(sys.argv[4], 0) if len(sys.argv) > 4 else (len(data) - off) // 8
        dis(data, off, n)
    elif cmd == "scan":
        scan(data, int(sys.argv[3]) if len(sys.argv) > 3 else 6)


if __name__ == "__main__":
    main()
