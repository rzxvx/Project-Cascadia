#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Fragment-side tests for tools/gpu/g9p-render: vertex and fragment shaders
from our assembler, the varying PDS program, the per-draw state words; the
64x64 result compared pixel by pixel with the CPU (barycentric
interpolation, pixel centres, edges left out).

    render-check.py build            -> build/gpu/render-check.bin + .json
    render-check.py check NAME RAW   -> compare RAW (64x64 BGRA8) with NAME's model
"""
import json
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, ".."))
import g9pasm as A  # noqa: E402
import g9pshaders  # noqa: E402

OUT = os.path.join(HERE, "../../../build/gpu/render-check")
# three vertices (clip space) and their colours: the lower-left half of the target
VERTS = [((-1, -1), (1, 0, 0, 1)), ((1, -1), (0, 1, 0, 1)), ((-1, 1), (0, 0, 1, 1))]
CLEAR = (0.25, 0.5, 0.75, 1)


def f16(f):
    return struct.unpack("<H", struct.pack("<e", f))[0]


def vertex_buffer(uv=None, tris=None):
    if tris:
        return b"".join(struct.pack("<8f", x, y, z, 1, *c) for t in tris for (x, y, z), c in t)
    if uv:
        return b"".join(struct.pack("<8f", x, y, 0, 1, u, v, 0, 0) for ((x, y), c), (u, v) in zip(VERTS, uv))
    return b"".join(struct.pack("<8f", x, y, 0, 1, *c) for (x, y), c in VERTS)


def vs(noutputs, base=0):
    """read vertex r0's 8 floats (position, colour) from the buffer whose
    address the PDS put in common-store dwords 0-1; outputs 0..noutputs-1.
    Eight registers (r0-r7): VDM_STATE5 0x1081 gives the vertex task 8
    registers x 32 instances, and a shader using more hangs the TA."""
    b = base
    body = [A.alu2("iadd", b + 1, 0, 0),                   # r1 = 2 * vertex (16-byte units)
            A.addr(b + 2, 0, b + 1, 4), A.mem_at(False, A.SIZE["4x32"], b + 4, b + 2), A.wait()]
    if noutputs == 4:
        body += [A.vs_output(b + 4 + s, s) for s in range(3)]
        return A.vertex_program(body, b + 7, 3)
    body += [A.vs_output(b + 4 + s, s) for s in range(4)]
    body += [A.mov_imm(b + 4, 1), A.alu2("iadd", b + 1, b + 1, b + 4),
             A.addr(b + 2, 0, b + 1, 4), A.mem_at(False, A.SIZE["4x32"], b + 4, b + 2), A.wait()]
    body += [A.vs_output(b + 4 + s, 4 + s) for s in range(noutputs - 5)]
    return A.vertex_program(body, b + 4 + noutputs - 5, noutputs - 1)


def cfg(outputs, varying0, sizeinfo1, sizeinfo2, vpds_data=0, tex=(0, 0, 0, 0, 0), nverts=3, ispa=0, ispctl=0,
        ispa_back=0, state5=0, vs_temps=0, fs_temps=0, ts=(0, 0, 0, 0, 0), ts3=0, ts2=0):
    return struct.pack("<24I", outputs, varying0, sizeinfo1, sizeinfo2, vpds_data, *tex, nverts, ispa, ispctl, ispa_back,
                       state5, vs_temps, fs_temps, *ts, ts3, ts2)


def vary_pds(varyings):
    """the varying PDS program: the W iterate, then one DOUTI per varying
    [(components, perspective)], END on the last; data padded to 16 bytes"""
    data = [0x03000000, 0]
    c = 0
    for i, (n, persp) in enumerate(varyings):
        L = 2 + 2 * c
        data += [0x03 << 24 | ((n - 1) << 2 | persp) << 16 | L << 8 | L,
                 (0x00400000 if persp else 0) | (c // 2) << 23 | (0x80000000 if i == len(varyings) - 1 else 0)]
        c += n
    data += [0] * (-len(data) % 4)
    ndout = len(data) // 2
    codew = [0xf0000004 | q << 3 for q in range(1 + len(varyings))]
    codew[-1] |= 0x04000000                                # END
    codew.append(0xd3000000)
    return struct.pack("<%dI" % len(data), *data), struct.pack("<%dI" % len(codew), *codew)


tests = {}
# 1. our vertex shader, a constant colour from our fragment shader
tests["flat"] = dict(
    vs=vs(4), fs=A.fragment_program([A.fs_mov_imm(0, f16(0.5) << 16 | f16(1.0)),
                                     A.fs_mov_imm(1, f16(1.0) << 16 | f16(0.25)), A.emit_colour(0)]),
    cfg=cfg(4, 0, 0x00800000, 0), model="flat", colour=(1.0, 0.5, 0.25, 1.0))
# 2. a vec4 varying: the vertex colours, interpolated
data, code = vary_pds([(4, 1)])
tests["vcol"] = dict(
    vs=vs(8), fs=A.fragment_program([A.iterate(0, 0, 4), A.wait(), A.emit_colour(0)]),
    cfg=cfg(8, 4, 0x00800420, 0, len(data)), vpds=data + code, model="vcol")

flat_fs = tests["flat"]["fs"]
tests["out8"] = dict(vs=vs(8), fs=flat_fs, cfg=cfg(8, 0, 0x00800000, 0), model="flat", colour=(1.0, 0.5, 0.25, 1.0))
tests["vpds_flatfs"] = dict(vs=vs(8), fs=flat_fs, cfg=cfg(8, 4, 0x00800420, 0, len(data)), vpds=data + code,
                            model="flat", colour=(1.0, 0.5, 0.25, 1.0))


# the vertex task's registers: a vertex shader using r8-r15 (16 registers)
# with VDM_STATE5's unified size and the DOUTU temps raised, and the vertex
# task's instances (STATE5 bits 25-29) cut
for nm, st5 in (("vs16_u8", 0x2081), ("vs16_u8_i16", 0x2081 | 16 << 25), ("vs16_u8_i8", 0x2081 | 8 << 25)):
    tests[nm] = dict(vs=vs(8, 8), fs=tests["vcol"]["fs"], cfg=cfg(8, 4, 0x00800420, 0, len(data), state5=st5,
                                                                  vs_temps=0x20), vpds=data + code, model="vcol")
# more: r16-r23 (24 registers) and r24-r31 (32)
for nm, base, st5, tm in (("vs24_u12_i8", 16, 0x3081 | 8 << 25, 0x30), ("vs32_u16_i8", 24, 0x4081 | 8 << 25, 0x40),
                          ("vs24_u16_i8", 16, 0x4081 | 8 << 25, 0x30), ("vs32_u16_i4", 24, 0x4081 | 4 << 25, 0x40)):
    tests[nm] = dict(vs=vs(8, base), fs=tests["vcol"]["fs"], cfg=cfg(8, 4, 0x00800420, 0, len(data), state5=st5,
                                                                     vs_temps=tm), vpds=data + code, model="vcol")

TW = TH = 8


def twiddle(x, y):
    """the texel's place: Morton order, the y bit first"""
    i = 0
    for b in range(3):
        i |= ((y >> b) & 1) << (2 * b) | ((x >> b) & 1) << (2 * b + 1)
    return i


def texel(x, y):
    """RGBA8: a pattern every texel of which differs"""
    return (x * 32 + 16, y * 32 + 16, ((x ^ y) & 1) * 255, 255)


def tex_bytes():
    t = [None] * (TW * TH)
    for y in range(TH):
        for x in range(TW):
            t[twiddle(x, y)] = bytes(texel(x, y))
    return b"".join(t)


# 3. a texture: the uv (vertex colour slots .xy) as a vec2 varying, nearest sampling
UV = [(0.0, 1.0), (1.0, 1.0), (0.0, 0.0)]
data_t, code_t = vary_pds([(2, 1)])
tests["tex"] = dict(
    vs=vs(6), fs=A.fragment_program([A.iterate_f32_coord(0, 0, 2), A.sample(0, 0), A.wait(), A.emit_colour(0)]),
    cfg=cfg(6, 2, 0x01000410, 0x00800000, len(data_t), (TW, TH, 0x000e0000, 0x00c00780, 0x0344050a)),
    vpds=data_t + code_t, tex=tex_bytes(), model="tex", uv=UV)


# 4. depth: two triangles, the second drawn after the first; LESS keeps the nearer
RED, GREEN = (1, 0, 0, 1), (0, 1, 0, 1)
T1 = [((-1, -1, 0.5), RED), ((1, -1, 0.5), RED), ((-1, 1, 0.5), RED)]


def t2(z):
    return [((-0.5, -1, z), GREEN), ((1, -1, z), GREEN), ((1, 0.5, z), GREEN)]


ISPA_ZLESS = 0x00120f00 | 1 << 17          # depth test LESS, depth write on
ISPA_ALWAYS = 0x00120f00 | 7 << 17         # depth test ALWAYS, depth write on
for name, z, ispa, cmpm in (("z_less_far", 0.7, ISPA_ZLESS, "less"), ("z_less_near", 0.3, ISPA_ZLESS, "less"),
                            ("z_always_far", 0.7, ISPA_ALWAYS, "always")):
    tests[name] = dict(vs=vs(8), fs=tests["vcol"]["fs"], vpds=data + code, model="tris", tris=[T1, t2(z)],
                       cmp=cmpm, cfg=cfg(8, 4, 0x00800420, 0, len(data), nverts=6, ispa=ispa))


# 5. blending (in the fragment shader): red, then green at alpha 0.5 over it. The
# ISP pass type TRANSLUCENT: an opaque object hides what it covers before any
# shader runs (only the top object's shader runs, the clear's neither)
ISPA_TRANSLUCENT = 0x00120f00 | 7 << 17 | 1 << 24
GREEN_HALF = (0, 1, 0, 0.5)
T2B = [((-0.5, -1, 0.5), GREEN_HALF), ((1, -1, 0.5), GREEN_HALF), ((1, 0.5, 0.5), GREEN_HALF)]
H = "#include <metal_stdlib>\nusing namespace metal;\n"
BSRC = H + """struct V { float4 p [[position]]; half4 c; };
vertex V vs(uint vid [[vertex_id]], constant float4 *pos [[buffer(0)]]) { V o; o.p = pos[vid]; o.c = half4(1); return o; }
fragment half4 fs(V in [[stage_in]]) { return in.c; }"""


def battach(sr, dr, sa, da):
    return (', "blending_enabled": true, "source_rgb_blend_factor": "%s", "destination_rgb_blend_factor": "%s", '
            '"source_alpha_blend_factor": "%s", "destination_alpha_blend_factor": "%s"' % (sr, dr, sa, da))


BLENDS = {"add": battach("One", "One", "One", "One"),
          "alpha": battach("SourceAlpha", "OneMinusSourceAlpha", "One", "OneMinusSourceAlpha"),
          "mul": battach("DestinationColor", "Zero", "DestinationAlpha", "Zero")}
for bname, att in BLENDS.items():
    fsc = g9pshaders.build_render("rbl_" + bname, BSRC, [("vs", "fs")], attach=att)[1][0]
    tests["blend_" + bname + "_apple"] = dict(vs=vs(8), fs=fsc, vpds=data + code, model="tris", tris=[T1, T2B],
                                             cmp="always", blend=bname,
                                             cfg=cfg(8, 4, 0x00800420, 0, len(data), nverts=6, ispa=ISPA_TRANSLUCENT))
# ours: src -> halves 0-3, the tile -> halves 4-7, add, emit
tests["blend_add_ours"] = dict(
    vs=vs(8), fs=A.fragment_program([A.iterate(0, 0, 4), A.load_tile(4), A.wait()]
                                    + [A.alu2h("hadd", i, i, 4 + i) for i in range(4)] + [A.emit_colour(0)]),
    vpds=data + code, model="tris", tris=[T1, T2B], cmp="always", blend="add",
    cfg=cfg(8, 4, 0x00800420, 0, len(data), nverts=6, ispa=ISPA_TRANSLUCENT))
tests["blend_mul_ours"] = dict(
    vs=vs(8), fs=A.fragment_program([A.iterate(0, 0, 4), A.load_tile(4), A.wait()]
                                    + [A.alu2h("hmul", i, i, 4 + i) for i in range(4)] + [A.emit_colour(0)]),
    vpds=data + code, model="tris", tris=[T1, T2B], cmp="always", blend="mul",
    cfg=cfg(8, 4, 0x00800420, 0, len(data), nverts=6, ispa=ISPA_TRANSLUCENT))


# --- shaders from GLSL through our compiler (devices/a10/mesa: g9p-cc) ------

A10 = os.path.abspath(os.path.join(HERE, "../../.."))
RC = os.path.join(A10, "build/mesa/rc")
VS_COL = """#version 300 es
layout(location = 0) in vec4 pos;
layout(location = 1) in vec4 col;
out vec4 v_col;
void main() {
   v_col = col;
   gl_Position = pos;
}
"""
FS_HEAD = "#version 300 es\nprecision highp float;\n"
VS_UV = """#version 300 es
layout(location = 0) in vec4 pos;
layout(location = 1) in vec4 col;
out vec2 v_uv;
void main() {
   v_uv = col.xy;
   gl_Position = pos;
}
"""
UV_CC = [(0.0, 1.0), (1.0, 1.0), (0.0, 0.0)]
# name: (vertex GLSL, fragment GLSL, model fields)
GLSL = {
    "cc_vcol": (VS_COL, FS_HEAD + "in vec4 v_col;\nout vec4 o;\nvoid main() { o = v_col; }\n", dict(model="vcol")),
    "cc_flat": (VS_COL, FS_HEAD + "out vec4 o;\nvoid main() { o = vec4(1.0, 0.5, 0.25, 1.0); }\n",
                dict(model="flat", colour=(1.0, 0.5, 0.25, 1.0))),
    # fragment arithmetic, swizzles, clamp
    "cc_math": (VS_COL, FS_HEAD + "in vec4 v_col;\nout vec4 o;\n"
                "void main() { o = clamp(v_col.bgra * 1.5 - vec4(0.25), 0.0, 1.0); }\n",
                dict(model="fn", fn="lambda c: [c[k] * 1.5 - 0.25 for k in (2, 1, 0, 3)]")),
    # varyings of 2 and 3 components
    "cc_vary23": ("""#version 300 es
layout(location = 0) in vec4 pos;
layout(location = 1) in vec4 col;
out vec2 v_a;
out vec3 v_b;
void main() {
   v_a = col.rg;
   v_b = col.gba * 0.5;
   gl_Position = pos;
}
""", FS_HEAD + "in vec2 v_a;\nin vec3 v_b;\nout vec4 o;\n"
                  "void main() { o = vec4(v_a.y, v_a.x, v_b.x + v_b.y, 1.0); }\n",
                  dict(model="fn", fn="lambda c: [c[1], c[0], 0.5 * (c[1] + c[2]), 1.0]")),
    # vertex arithmetic: the triangle scaled and moved
    "cc_vsxf": ("""#version 300 es
layout(location = 0) in vec4 pos;
layout(location = 1) in vec4 col;
out vec4 v_col;
void main() {
   v_col = col;
   gl_Position = vec4(pos.xy * 0.75 + vec2(0.1, -0.05), pos.z, pos.w);
}
""", FS_HEAD + "in vec4 v_col;\nout vec4 o;\nvoid main() { o = v_col; }\n",
                dict(model="fn", fn="lambda c: c", xform=[0.75, 0.1, -0.05])),
    # a select
    "cc_fssel": (VS_COL, FS_HEAD + "in vec4 v_col;\nout vec4 o;\nvoid main() {\n"
                 "   if (v_col.r > v_col.g) o = vec4(v_col.r, 0.0, 0.0, 1.0); else o = vec4(0.0, v_col.g, v_col.b, 1.0);\n}\n",
                 dict(model="fn", fn="lambda c: None if abs(c[0] - c[1]) < 0.02 else "
                      "([c[0], 0, 0, 1] if c[0] > c[1] else [0, c[1], c[2], 1])")),
    # a loop of a pixel-dependent count (the mask stack in the fragment bank)
    "cc_fsloop": (VS_COL, FS_HEAD + "in vec4 v_col;\nout vec4 o;\nvoid main() {\n"
                  "   float s = 0.0;\n   int n = int(v_col.b * 6.0);\n"
                  "   for (int i = 0; i < n; i++) s += 0.125;\n   o = vec4(s, v_col.g, 0.0, 1.0);\n}\n",
                  dict(model="fn", fn="lambda c: None if abs(c[2] * 6 - round(c[2] * 6)) < 0.03 else "
                       "[0.125 * int(c[2] * 6), c[1], 0, 1]")),
    # division, square root, fract, pow
    "cc_fsfn": (VS_COL, FS_HEAD + "in vec4 v_col;\nout vec4 o;\nvoid main() {\n"
                "   o = vec4(v_col.r / (v_col.g + 1.0), sqrt(v_col.b), fract(v_col.r * 3.0), pow(v_col.g, 2.0));\n}\n",
                dict(model="fn", fn="lambda c: None if abs(c[0] * 3 - round(c[0] * 3)) < 0.02 else "
                     "[c[0] / (c[1] + 1), c[2] ** 0.5, c[0] * 3 - int(c[0] * 3), c[1] ** 2]")),
    # a texture, sampled at the uv the vertex shader passes on (vertex attribute 1 .xy)
    "cc_tex": (VS_UV, FS_HEAD + "uniform sampler2D t;\nin vec2 v_uv;\nout vec4 o;\n"
               "void main() { o = texture(t, v_uv); }\n", dict(model="tex", uv=UV_CC, tex=True)),
    "cc_texmod": (VS_UV, FS_HEAD + "uniform sampler2D t;\nin vec2 v_uv;\nout vec4 o;\n"
                  "void main() { vec4 c = texture(t, v_uv * 0.5 + vec2(0.25)); o = vec4(c.g, c.r * 0.5, c.b, 1.0); }\n",
                  dict(model="tex", uv=[(u * 0.5 + 0.25, v * 0.5 + 0.25) for u, v in [(0.0, 1.0), (1.0, 1.0), (0.0, 0.0)]],
                       vb_uv=[(0.0, 1.0), (1.0, 1.0), (0.0, 0.0)], tex=True,
                       post="lambda c: (c[1], c[0] * 0.5, c[2], 1.0)")),
    # blending compiled into the fragment shader (nir_lower_blend, the tile
    # read): red, then green at alpha 0.5 over it, as the blend_*_apple tests
    **{"cc_blend_" + m: (VS_COL, FS_HEAD + "in vec4 v_col;\nout vec4 o;\nvoid main() { o = v_col; }\n",
                         dict(model="tris", tris=[T1, T2B], cmp="always", blend=m, blendmode=m, nverts=6,
                              ispa=ISPA_TRANSLUCENT))
       for m in ("add", "alpha", "mul")},
    # many values live at once (more than 8 fragment registers)
    "cc_fsregs": (VS_COL, FS_HEAD + "in vec4 v_col;\nout vec4 o;\nvoid main() {\n"
                  "   vec4 a = v_col * 2.0;\n   vec4 b = v_col.wzyx * 0.5;\n   vec4 d = a * b + v_col;\n"
                  "   o = clamp(d * 0.3 + a * 0.1 - b * 0.2, 0.0, 1.0);\n}\n",
                  dict(model="fn", fn="lambda c: [(2 * c[k] * 0.5 * c[3 - k] + c[k]) * 0.3 + 0.2 * c[k] "
                       "- 0.1 * c[3 - k] for k in range(4)]")),
}


def entries(path):
    d = open(path, "rb").read()
    n, = struct.unpack_from("<I", d, 4)
    out = {}
    for i in range(n):
        name = d[8 + 32 * i: 8 + 32 * i + 24].rstrip(b"\0").decode()
        off, size = struct.unpack_from("<II", d, 8 + 32 * i + 24)
        out[name] = d[off:off + size]
    return out


def regs_state(nregs):
    """VDM_STATE5 and the DOUTU temps word for a vertex shader of nregs
    registers: registers x instances within 256"""
    for regs, st5 in ((8, 0x1081), (16, 0x2081 | 16 << 25), (32, 0x4081 | 8 << 25)):
        if nregs <= regs:
            return st5, regs // 4 << 3
    raise ValueError("vertex shader of %d registers" % nregs)


# SAMPLER_SWEEP=1: the texture test with each bit of the two sampler words
# (000e0000 00c00780) flipped in turn, uv from -0.5 to 1.5 (so wrapping
# shows) over the 8x8 texture (4 pixels a texel: filtering shows); and with
# each bit of texture state words 0, 1 and 4-7 (samp_* / tsw*_*)
if os.environ.get("SAMPLER_SWEEP"):
    UVW = [(-0.5, 1.5), (1.5, 1.5), (-0.5, -0.5)]
    S0, S1, T0 = 0x000e0000, 0x00c00780, 0x0344050a

    def sweep_test(s0, s1, t0=T0):
        return dict(vs=vs(6), fs=tests["tex"]["fs"],
                    cfg=cfg(6, 2, 0x01000410, 0x00800000, len(data_t), (TW, TH, s0, s1, t0)),
                    vpds=data_t + code_t, tex=tex_bytes(), model="tex", uv=UVW)
    tests["samp_base"] = sweep_test(S0, S1)
    for b in range(32):
        tests["samp0_%02d" % b] = sweep_test(S0 ^ 1 << b, S1)
        tests["samp1_%02d" % b] = sweep_test(S0, S1 ^ 1 << b)

    # mipmaps: a 16x16 texture, levels 16 / 8 / 4 / 2 / 1 each one colour
    # (red, green, blue, yellow, magenta), one after the other twiddled;
    # uv 0..16 repeated: 4 texels a pixel (level 2 when mipmapped). The
    # texture state's word 1 (bits 23-31), words 4-7 and the sampler words
    # with each bit flipped in turn (mip_*)
    LV = [(255, 0, 0, 255), (0, 255, 0, 255), (0, 0, 255, 255), (255, 255, 0, 255), (255, 0, 255, 255)]
    MIPS = b"".join(bytes(LV[l]) * ((16 >> l) * (16 >> l)) for l in range(5))
    UVM = [(0.0, 16.0), (16.0, 16.0), (0.0, 0.0)]
    R0, R1 = S0 | 1 << 29, S1 | 1                            # repeat in u and v

    def mip_test(s0=R0, s1=R1, ts=(0, 0, 0, 0, 0)):
        return dict(vs=vs(6), fs=tests["tex"]["fs"],
                    cfg=cfg(6, 2, 0x01000410, 0x00800000, len(data_t), (16, 16, s0, s1, T0), ts=ts),
                    vpds=data_t + code_t, tex=MIPS, model="tex", uv=UVM)
    tests["mip_base"] = mip_test()
    # the texels numbered (R = index & 255, G = index >> 8 & 255, B = 0x80):
    # a pixel's colour names the dword the sampler read; 4096 of them
    NUM = b"".join(bytes((i & 255, i >> 8 & 255, 0x80, 255)) for i in range(4096))

    def num_test(s1=R1, ts3=0, ts2=0, ts=(0, 0, 0, 0, 0)):
        return dict(vs=vs(6), fs=tests["tex"]["fs"],
                    cfg=cfg(6, 2, 0x01000410, 0x00800000, len(data_t), (16, 16, R0, s1, T0), ts=ts, ts3=ts3, ts2=ts2),
                    vpds=data_t + code_t, tex=NUM, model="tex", uv=UVM)
    tests["num_base"] = num_test()
    tests["num_s1_06"] = num_test(s1=R1 | 1 << 6)
    for b in range(1, 32):
        tests["num_w3_%02d" % b] = num_test(ts3=1 << b)
        tests["num_w3s_%02d" % b] = num_test(s1=R1 | 1 << 6, ts3=1 << b)
    tests["num_w2_00"] = num_test(ts2=1)
    tests["num_w2s_00"] = num_test(s1=R1 | 1 << 6, ts2=1)
    for b in range(23, 32):
        tests["mip_t1_%02d" % b] = mip_test(ts=(1 << b, 0, 0, 0, 0))
    for w in range(4):
        for b in range(32):
            ts = [0, 0, 0, 0, 0]
            ts[1 + w] = 1 << b
            tests["mip_t%d_%02d" % (4 + w, b)] = mip_test(ts=tuple(ts))
    for b in range(32):
        tests["mip_s0_%02d" % b] = mip_test(s0=R0 ^ 1 << b)
        tests["mip_s1_%02d" % b] = mip_test(s1=R1 ^ 1 << b)


def compile_glsl():
    os.makedirs(RC, exist_ok=True)
    cmds = []
    for n, (v, f, model) in GLSL.items():
        open(os.path.join(RC, n + ".vert"), "w").write(v)
        open(os.path.join(RC, n + ".frag"), "w").write(f)
        b = " -b " + model["blendmode"] if "blendmode" in model else ""
        cmds.append("rm -f /a10/build/mesa/rc/{0}.bin; /a10/build/mesa/out/g9p-cc -v 300 -a 2{1} "
                    "/a10/build/mesa/rc/{0}.vert /a10/build/mesa/rc/{0}.frag "
                    "/a10/build/mesa/rc/{0}.bin {0} >&2 || echo COMPILE-FAIL {0} >&2".format(n, b))
    subprocess.run(["docker", "run", "--rm", "--platform", "linux/arm64", "-v", A10 + ":/a10", "cascadia-a10-mesa",
                    "sh", "-c", "; ".join(cmds)], check=True)
    for n, (v, f, model) in GLSL.items():
        path = os.path.join(RC, n + ".bin")
        if not os.path.exists(path):
            continue
        e = entries(path)
        vi = struct.unpack("<%dI" % (len(e[n + ".vsinfo"]) // 4), e[n + ".vsinfo"])
        fi = struct.unpack("<I", e[n + ".fsinfo"])
        vregs, noutputs, nattribs, nvary = vi[:4]
        vary = [(vi[4 + 2 * i], vi[5 + 2 * i]) for i in range(nvary)]
        total = sum(c for c, p in vary)
        st5, vtemps = regs_state(vregs)
        ftemps = (fi[0] + 3) // 4 << 3
        t = dict(vs=e[n + ".vs"], fs=e[n + ".fs"],
                 **{k: v for k, v in model.items() if k not in ("tex", "vb_uv", "blendmode", "nverts", "ispa")})
        draw = dict(nverts=model.get("nverts", 3), ispa=model.get("ispa", 0))
        if model.get("tex"):
            t["tex"] = tex_bytes()
            t["uv"] = model.get("vb_uv", model["uv"])
            t["model_uv"] = model["uv"]
        if vary:
            vd, vc = vary_pds(vary)
            t["vpds"] = vd + vc
            sz1 = (0x01000000 if model.get("tex") else 0x00800000) | (len(vd) // 16) << 10 | (total + 1) // 2 << 4
            texw = (TW, TH, 0x000e0000, 0x00c00780, 0x0344050a) if model.get("tex") else (0, 0, 0, 0, 0)
            t["cfg"] = cfg(noutputs, total, sz1, 0x00800000 if model.get("tex") else 0, len(vd), texw,
                           state5=st5, vs_temps=vtemps, fs_temps=ftemps, **draw)
        else:
            t["cfg"] = cfg(noutputs, 0, 0x00800000, 0, state5=st5, vs_temps=vtemps, fs_temps=ftemps, **draw)
        tests[n] = t


def build():
    compile_glsl()
    out, meta = {}, {}
    for name, t in tests.items():
        out[name + ".vs"] = t["vs"]
        out[name + ".fs"] = t["fs"]
        out[name + ".cfg"] = t["cfg"]
        out[name + ".vb"] = vertex_buffer(t.get("uv"), t.get("tris"))
        if "vpds" in t:
            out[name + ".vpds"] = t["vpds"]
        if "tex" in t:
            out[name + ".tex"] = t["tex"]
        meta[name] = {k: v for k, v in t.items() if k in ("model", "colour", "uv", "tris", "cmp", "blend", "fn", "xform",
                                                             "model_uv", "post")}
    g9pshaders.pack(out, OUT + ".bin")
    json.dump(meta, open(OUT + ".json", "w"))
    print(" ".join(tests))


def bary(tri, px, py):
    sx = [32 + 32 * v[0][0] for v in tri]
    sy = [32 - 32 * v[0][1] for v in tri]
    d = (sy[1] - sy[2]) * (sx[0] - sx[2]) + (sx[2] - sx[1]) * (sy[0] - sy[2])
    b0 = ((sy[1] - sy[2]) * (px - sx[2]) + (sx[2] - sx[1]) * (py - sy[2])) / d
    b1 = ((sy[2] - sy[0]) * (px - sx[2]) + (sx[0] - sx[2]) * (py - sy[2])) / d
    return b0, b1, 1 - b0 - b1


def blend(mode, s, d):
    c = lambda v: min(max(v, 0.0), 1.0)
    if mode == "add":
        return tuple(c(s[k] + d[k]) for k in range(4))
    if mode == "alpha":
        return tuple(c(s[k] * s[3] + d[k] * (1 - s[3])) for k in range(3)) + (c(s[3] + d[3] * (1 - s[3])),)
    if mode == "mul":
        return tuple(c(s[k] * d[k]) for k in range(4))
    return s


def model_tris(t, px, py):
    """triangles in order with a depth test: (colour or None on an edge)"""
    colour, depth = CLEAR, 1.0
    for tri in t["tris"]:
        b = bary(tri, px, py)
        if min(b) < -0.02:
            continue
        if min(b) < 0.02:
            return None
        z = sum(b[i] * tri[i][0][2] for i in range(3))
        if t["cmp"] == "always" or (t["cmp"] == "less" and z < depth):
            src = tuple(sum(b[i] * tri[i][1][k] for i in range(3)) for k in range(4))
            colour, depth = blend(t.get("blend"), src, colour), z
    return colour


def model(name, x, y):
    """the expected (r, g, b, a) at pixel centre (x, y), or None on an edge"""
    t = json.load(open(OUT + ".json"))[name]
    if t["model"] == "tris":
        return model_tris(t, x + 0.5, y + 0.5)
    xf = t.get("xform", [1.0, 0.0, 0.0])
    sx = [32 + 32 * (vx * xf[0] + xf[1]) for (vx, vy), c in VERTS]
    sy = [32 - 32 * (vy * xf[0] + xf[2]) for (vx, vy), c in VERTS]
    px, py = x + 0.5, y + 0.5
    d = (sy[1] - sy[2]) * (sx[0] - sx[2]) + (sx[2] - sx[1]) * (sy[0] - sy[2])
    b0 = ((sy[1] - sy[2]) * (px - sx[2]) + (sx[2] - sx[1]) * (py - sy[2])) / d
    b1 = ((sy[2] - sy[0]) * (px - sx[2]) + (sx[0] - sx[2]) * (py - sy[2])) / d
    b2 = 1 - b0 - b1
    if min(b0, b1, b2) < -0.02:
        return CLEAR
    if min(b0, b1, b2) < 0.02:
        return None
    if t["model"] == "flat":
        return tuple(t["colour"])
    if t["model"] == "tex":
        uv = t.get("model_uv", t["uv"])
        u = b0 * uv[0][0] + b1 * uv[1][0] + b2 * uv[2][0]
        v = b0 * uv[0][1] + b1 * uv[1][1] + b2 * uv[2][1]
        fu, fv = u * TW, v * TH
        if min(abs(fu - round(fu)), abs(fv - round(fv))) < 0.06:
            return None                                    # on a texel edge
        tx, ty = min(int(fu), TW - 1), min(int(fv), TH - 1)
        c = tuple(c / 255 for c in texel(tx, ty))
        return eval(t["post"])(c) if "post" in t else c
    cols = [c for v, c in VERTS]
    col = tuple(b0 * cols[0][i] + b1 * cols[1][i] + b2 * cols[2][i] for i in range(4))
    if t["model"] == "fn":
        r = eval(t["fn"])(col)
        return None if r is None else tuple(min(max(v, 0.0), 1.0) for v in r)
    return col


def check(name, raw):
    px = struct.unpack("<4096I", open(raw, "rb").read())
    bad = n = 0
    for y in range(64):
        for x in range(64):
            m = model(name, x, y)
            if m is None:
                continue
            n += 1
            v = px[y * 64 + x]
            got = ((v >> 16) & 255, (v >> 8) & 255, v & 255, v >> 24)      # BGRA8 in memory
            want = tuple(round(c * 255) for c in m)
            if max(abs(a - b) for a, b in zip(got, want)) > 3:
                if bad < 4:
                    print("  (%d, %d): got %s want %s" % (x, y, got, want))
                bad += 1
    print("%s  %s: %d of %d pixels right" % ("ok  " if not bad else "FAIL", name, n - bad, n))
    return bad == 0


if __name__ == "__main__":
    if sys.argv[1] == "build":
        build()
    else:
        sys.exit(0 if check(sys.argv[2], sys.argv[3]) else 1)
