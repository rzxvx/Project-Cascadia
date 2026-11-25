# SPDX-License-Identifier: MIT
"""G9P (applegpu_g9p, the A10's USC) machine code: splitting and printing,
for decoding the ISA with Apple's offline compiler as the oracle
(devices/a10/docs/gpu.md "G8"). Compiler output stays in build/, never in git.

    split(code)        -> [[word, ...], ...]   instructions, 16-bit LE words
    fmt(insn)          -> "op 0x98: 0198 0100 ..." (words as 4-digit hex)
    corpus(tag, src, kernels) -> {name: [insn, ...]}   compile + split

An instruction runs to the first word whose bit 8 is clear; its opcode is the
first word's low byte. The code ends with the word 0x8000, after 0xc000
padding words (split() drops both).
"""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import g9pshaders  # noqa: E402


def split(code):
    words = struct.unpack("<%dH" % (len(code) // 2), code[:len(code) // 2 * 2])
    out, cur = [], []
    for w in words:
        cur.append(w)
        if not (w >> 8) & 1:
            out.append(cur)
            cur = []
    if cur:
        out.append(cur)
    # the end word (0x8000) and the padding (0xc000) before it
    while out and out[-1] in ([0xc000], [0x8000]):
        out.pop()
    return out


def fmt(insn):
    return "op %02x: %s" % (insn[0] & 0xff, " ".join("%04x" % w for w in insn))


def unique_bits(variants):
    """{key: instruction}, every key setting one operand bit: {key: [(word,
    bit), ...]} -- the encoding bits set only in that variant, among the
    variants of the same length (so the forms with an extra word are
    compared with each other)."""
    out = {}
    for k, ins in variants.items():
        same = [v for k2, v in variants.items() if k2 != k and len(v) == len(ins)]
        bits = []
        for w, word in enumerate(ins):
            others = 0
            for v in same:
                others |= v[w]
            bits += [(w, b) for b in range(16) if (word & ~others) >> b & 1]
        out[k] = bits
    return out


def bitmap(base, variants):
    """base: an instruction (words); variants: {operand bit: instruction} for
    the operand = 1 << bit. Returns {operand bit: [(word, bit), ...]} -- the
    encoding bits each operand bit flips."""
    out = {}
    for ob, ins in variants.items():
        flips = []
        for w in range(max(len(base), len(ins))):
            a = base[w] if w < len(base) else 0
            b = ins[w] if w < len(ins) else 0
            flips += [(w, b2) for b2 in range(16) if (a ^ b) >> b2 & 1]
        out[ob] = flips
    return out


HEADER = "#include <metal_stdlib>\nusing namespace metal;\n"


def corpus_raw(kernels, jobs=8):
    """{name: Metal source of one kernel} -> {name: code bytes} (see corpus)."""
    import hashlib
    from concurrent.futures import ThreadPoolExecutor

    def one(item):
        name, body = item
        src = HEADER + body + "\n"
        h = hashlib.sha1(src.encode()).hexdigest()[:16]
        return name, g9pshaders.build("c/" + h, src, [name])[name]

    os.makedirs(os.path.join(g9pshaders.ISA, "c"), exist_ok=True)
    with ThreadPoolExecutor(jobs) as ex:
        return dict(ex.map(one, kernels.items()))


def corpus(tag, kernels, jobs=8):
    """{name: Metal source of one kernel named `name`} -> {name: [insn, ...]}.
    One library per kernel (metal-tt does not keep a library's function
    order), built in parallel, cached under build/gpu/isa/c/<hash>."""
    import hashlib
    from concurrent.futures import ThreadPoolExecutor

    def one(item):
        name, body = item
        src = HEADER + body + "\n"
        h = hashlib.sha1(src.encode()).hexdigest()[:16]
        return name, split(g9pshaders.build("c/" + h, src, [name])[name])

    os.makedirs(os.path.join(g9pshaders.ISA, "c"), exist_ok=True)
    with ThreadPoolExecutor(jobs) as ex:
        return dict(ex.map(one, kernels.items()))


if __name__ == "__main__":
    import g9pbin
    for path in sys.argv[1:]:
        print("==", path)
        for i, c in enumerate(g9pbin.kernels(path)):
            print(" kernel", i)
            for ins in split(c):
                print("   ", fmt(ins))
