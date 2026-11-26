#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The G9P compiler (devices/a10/mesa: g9p-cc, GLSL -> NIR -> G9P) kernel by
kernel on the GPU: each test a GLSL compute shader over in[] (binding 1) ->
o[] (binding 0), compiled in the cascadia-a10-mesa image, run by g9p-probe
on the iPad (HOST), every output word compared with Python's.

    cc-check.py [NAME ...]
"""
import math
import os
import re
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
A10 = os.path.abspath(os.path.join(HERE, "../../.."))
CC = os.path.join(A10, "build/mesa/cc")
HOST = os.environ.get("HOST", "root@172.16.43.1")
N = 64              # threads: 2 groups of 32

fb = lambda f: struct.unpack("<I", struct.pack("<f", f))[0]
bf = lambda u: struct.unpack("<f", struct.pack("<I", u))[0]
u32 = lambda v: v & 0xffffffff
HEAD = """#version 430
layout(local_size_x = 32) in;
layout(std430, binding = 0) buffer Out { %s o[]; };
layout(std430, binding = 1) buffer In { %s a[]; };
void main() {
   uint i = gl_GlobalInvocationID.x;
   %s
}
"""
INT_IN = [u32(i * 2654435761) >> 7 for i in range(N)]          # varied unsigned inputs
FLT_IN = [(i - 20) * 0.37 for i in range(N)]

# name: (out type, in type, body, inputs, expected(i, a) -> value)
TESTS = {
    "imadd": ("uint", "uint", "o[i] = a[i] * 3u + 7u;", INT_IN, lambda i, a: u32(a * 3 + 7)),
    "ilogic": ("uint", "uint", "o[i] = (a[i] ^ 0x5a5a5a5au) & (a[i] | i);", INT_IN,
               lambda i, a: (a ^ 0x5a5a5a5a) & (a | i)),
    "ishift": ("uint", "uint", "o[i] = (a[i] << (i & 7u)) + (a[i] >> 3u);", INT_IN,
               lambda i, a: u32((a << (i & 7)) + (a >> 3))),
    "isub": ("uint", "uint", "o[i] = i - a[i];", INT_IN, lambda i, a: u32(i - a)),
    "fmadd": ("float", "float", "o[i] = a[i] * 2.5 + float(i);", FLT_IN, lambda i, a: a * 2.5 + i),
    "ffma": ("float", "float", "o[i] = fma(a[i], a[i], -1.0);", FLT_IN, lambda i, a: a * a - 1.0),
    "fsub": ("float", "float", "o[i] = float(i) - a[i];", FLT_IN, lambda i, a: i - a),
    "conv": ("uint", "float", "o[i] = uint(abs(a[i]) * 3.0) + uint(float(i) * 0.5);", FLT_IN,
             lambda i, a: int(abs(a) * 3.0) + int(i * 0.5)),
    "rcp": ("float", "float", "o[i] = 1.0 / (a[i] + 100.0);", FLT_IN, lambda i, a: 1.0 / (a + 100.0)),
    "exp2log2": ("float", "float", "o[i] = exp2(a[i] * 0.25) + log2(float(i) + 1.0);", FLT_IN,
                 lambda i, a: 2 ** (a * 0.25) + math.log2(i + 1.0)),
    "floor": ("float", "float", "o[i] = floor(a[i]);", FLT_IN, lambda i, a: math.floor(a)),
}


# control flow: the expected values from the whole input (A)
def _collatz(x):
    n = 0
    while x != 1:
        x = x // 2 if x % 2 == 0 else 3 * x + 1
        n += 1
    return n


def _brk(i):
    s = 0
    for k in range(N):
        if (INT_IN[k] & 7) == (i & 7):
            break
        s += INT_IN[k] >> 4
    return u32(s)


def _cont2(i):
    s = 0
    for k in range(i):
        a = INT_IN[k]
        if a & 1 == 0:
            s += 1
            if a & 2 == 0:
                continue
            s += a >> 20
        s ^= a >> 8
        if s > 0x8000000:
            break
    return u32(s)


def _nested(i):
    s = 0
    for j in range(i & 3):
        for k in range(i >> 3):
            s += j * k + INT_IN[j + k]
    return u32(s)


def _longif(i, a):
    if (a & 1) == 0:
        return a
    x = a
    for k in range(40):
        x = u32(x * 3 + k) ^ (x >> 1)
    return x


TESTS.update({
    "ifelse": ("uint", "uint", "if ((i & 3u) == 1u) o[i] = a[i] * 3u; else o[i] = a[i ^ 1u] ^ i;", INT_IN,
               lambda i, a: u32(a * 3) if i & 3 == 1 else INT_IN[i ^ 1] ^ i),
    "ifnest": ("uint", "uint", """uint r = a[i];
   if (r > 0x800000u) { if ((i & 1u) == 0u) r = a[i ^ 2u] + 1u; else r = r - a[i ^ 1u]; }
   else if (i < 40u) { r = a[63u - i] * 5u; }
   o[i] = r;""", INT_IN,
               lambda i, a: (u32(INT_IN[i ^ 2] + 1) if i & 1 == 0 else u32(a - INT_IN[i ^ 1])) if a > 0x800000
               else (u32(INT_IN[63 - i] * 5) if i < 40 else a)),
    "loop": ("uint", "uint", "uint s = 0u; for (uint k = 0u; k < (i & 15u); k++) s += a[k] ^ k; o[i] = s;",
             INT_IN, lambda i, a: u32(sum(INT_IN[k] ^ k for k in range(i & 15)))),
    "brk": ("uint", "uint", """uint s = 0u;
   for (uint k = 0u; k < 64u; k++) { if ((a[k] & 7u) == (i & 7u)) break; s += a[k] >> 4u; }
   o[i] = s;""", INT_IN, lambda i, a: _brk(i)),
    "cont": ("uint", "uint", """uint s = 0u;
   for (uint k = 0u; k < i; k++) { if ((a[k] & 1u) == 0u) continue; s += a[k] >> 8u; }
   o[i] = s;""", INT_IN, lambda i, a: u32(sum(INT_IN[k] >> 8 for k in range(i) if INT_IN[k] & 1))),
    "cont2": ("uint", "uint", """uint s = 0u;
   for (uint k = 0u; k < i; k++) {
      if ((a[k] & 1u) == 0u) { s += 1u; o[i] = s; if ((a[k] & 2u) == 0u) continue; s += a[k] >> 20u; }
      s ^= a[k] >> 8u;
      if (s > 0x8000000u) break;
   }
   o[i] = s;""", INT_IN, lambda i, a: _cont2(i)),
    "nested": ("uint", "uint", """uint s = 0u;
   for (uint j = 0u; j < (i & 3u); j++) for (uint k = 0u; k < (i >> 3u); k++) s += j * k + a[j + k];
   o[i] = s;""", INT_IN, lambda i, a: _nested(i)),
    "collatz": ("uint", "uint", """uint x = (a[i] & 255u) + 1u, n = 0u;
   while (x != 1u) { if ((x & 1u) == 0u) x = x >> 1u; else x = 3u * x + 1u; n++; }
   o[i] = n;""", INT_IN, lambda i, a: _collatz((a & 255) + 1)),
    "minmax": ("uint", "uint", "o[i] = min(a[i], 0x400000u) + max(a[i ^ 1u], 0x200000u);", INT_IN,
               lambda i, a: u32(min(a, 0x400000) + max(INT_IN[i ^ 1], 0x200000))),
    "iminmax": ("int", "int", "o[i] = min(a[i], 3) * 2 + max(a[i ^ 1u], -7);", [(i * 7) % 23 - 11 for i in range(N)],
                lambda i, a: min(a, 3) * 2 + max(((i ^ 1) * 7) % 23 - 11, -7)),
    "fclamp": ("float", "float", "o[i] = max(min(a[i], 3.0), -2.5) + clamp(a[i] * 0.1, 0.0, 1.0);", FLT_IN,
               lambda i, a: max(min(a, 3.0), -2.5) + min(max(a * 0.1, 0.0), 1.0)),
    "cmpsel": ("uint", "uint", "o[i] = uint(a[i] > 0x900000u) + (a[i] < i * 0x40000u ? 7u : 9u) + (a[i] == a[i] ? 16u : 0u);",
               INT_IN, lambda i, a: (1 if a > 0x900000 else 0) + (7 if a < i * 0x40000 else 9) + 16),
    "fcmp": ("float", "float", "o[i] = a[i] < 0.5 ? a[i] * 2.0 : (a[i] >= 3.0 ? 1.0 : -1.0);", FLT_IN,
             lambda i, a: a * 2.0 if a < 0.5 else (1.0 if a >= 3.0 else -1.0)),
    "extract": ("uint", "uint", "o[i] = ((a[i] >> 8u) & 255u) + (a[i] >> 16u & 0xffu) * 3u + (a[i] & 0xffffu);",
                INT_IN, lambda i, a: ((a >> 8) & 255) + ((a >> 16) & 255) * 3 + (a & 0xffff)),
    "longif": ("uint", "uint", """uint x = a[i];
   if ((x & 1u) != 0u) { for (uint k = 0u; k < 40u; k++) x = (x * 3u + k) ^ (x >> 1u); }
   o[i] = x;""", INT_IN, _longif),
})


def compile_all(names):
    os.makedirs(CC, exist_ok=True)
    for n in names:
        ot, it, body, _, _ = TESTS[n]
        open(os.path.join(CC, n + ".comp"), "w").write(HEAD % (ot, it, body))
    script = "; ".join("/a10/build/mesa/out/g9p-cc -v 430 /a10/build/mesa/cc/%s.comp /a10/build/mesa/cc/%s.bin %s"
                       " || echo COMPILE-FAIL %s" % (n, n, n, n) for n in names)
    r = subprocess.run(["docker", "run", "--rm", "--platform", "linux/arm64", "-v", A10 + ":/a10",
                        "cascadia-a10-mesa", "sh", "-c", script], capture_output=True, text=True)
    print(r.stdout + r.stderr, end="")


def run(n):
    ot, it, body, inp, want = TESTS[n]
    info = open(os.path.join(CC, n + ".bin"), "rb").read()
    nregs = struct.unpack_from("<I", info, len(info) - 16)[0]
    temps = (nregs + 1 + 3) // 4                  # + the mask stack register, units of 4
    subprocess.run(["scp", "-q", os.path.join(CC, n + ".bin"), HOST + ":/run/cc-%s.bin" % n], check=True)
    words = [fb(v) if it == "float" else u32(v) for v in inp]
    args = ["x=%d" % (N // 32 - 1), "w6=1", "w7=%d" % (31 << 20), "tab=30:32", "tail=1", "dump=%d" % N,
            "temps=0x%x" % (temps << 3), "k0=0x%x" % (0x103 | temps << 12)]
    args += ["in=1:%d:0x%x" % (k, w) for k, w in enumerate(words)]
    out = subprocess.run(["ssh", HOST, "/run/g9p-probe %s file=/run/cc-%s.bin %s" % (n, n, " ".join(args))],
                         capture_output=True, text=True).stdout
    got = {}
    for m in re.finditer(r"buf 0 \[\s*(\d+)\] = (\w+)", out):
        got[int(m.group(1))] = int(m.group(2), 16)
    bad = 0
    for k in range(N):
        w = want(k, inp[k])
        g = got.get(k)
        if ot == "float":
            ok = g is not None and abs(bf(g) - w) <= 1e-5 * max(1.0, abs(w)) + 1e-6
        else:
            ok = g == u32(w)
        if not ok:
            if bad < 3:
                print("   %s[%d]: got %s want %s" % (n, k, None if g is None else
                                                    (bf(g) if ot == "float" else hex(g)), w))
            bad += 1
    print("%s  %-10s %d of %d right (%d registers)" % ("ok  " if not bad else "FAIL", n, N - bad, N, nregs))
    return bad == 0


if __name__ == "__main__":
    names = sys.argv[1:] or list(TESTS)
    compile_all(names)
    fails = sum(not run(n) for n in names if os.path.exists(os.path.join(CC, n + ".bin")))
    print("cc-check: %d failed" % fails)
