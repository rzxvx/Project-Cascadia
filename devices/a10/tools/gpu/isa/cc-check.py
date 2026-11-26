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
    words = [fb(v) if it == "float" else v for v in inp]
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
