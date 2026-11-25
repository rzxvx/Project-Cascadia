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


def vs(noutputs):
    """read vertex r0's 8 floats (position, colour) from the buffer whose
    address the PDS put in common-store dwords 0-1; outputs 0..noutputs-1.
    Eight registers (r0-r7): VDM_STATE5 0x1081 gives the vertex task 8
    registers x 32 instances, and a shader using more hangs the TA."""
    body = [A.alu2("iadd", 1, 0, 0),                       # r1 = 2 * vertex (16-byte units)
            A.addr(2, 0, 1, 4), A.mem_at(False, A.SIZE["4x32"], 4, 2), A.wait()]
    if noutputs == 4:
        body += [A.vs_output(4 + s, s) for s in range(3)]
        return A.vertex_program(body, 7, 3)
    body += [A.vs_output(4 + s, s) for s in range(4)]
    body += [A.mov_imm(4, 1), A.alu2("iadd", 1, 1, 4),
             A.addr(2, 0, 1, 4), A.mem_at(False, A.SIZE["4x32"], 4, 2), A.wait()]
    body += [A.vs_output(4 + s, 4 + s) for s in range(noutputs - 5)]
    return A.vertex_program(body, 4 + noutputs - 5, noutputs - 1)


def cfg(outputs, varying0, sizeinfo1, sizeinfo2, vpds_data=0, tex=(0, 0, 0, 0, 0), nverts=3, ispa=0, ispctl=0,
        ispa_back=0):
    return struct.pack("<14I", outputs, varying0, sizeinfo1, sizeinfo2, vpds_data, *tex, nverts, ispa, ispctl, ispa_back)


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


def build():
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
        meta[name] = {k: v for k, v in t.items() if k in ("model", "colour", "uv", "tris", "cmp", "blend")}
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
    sx = [32 + 32 * vx for (vx, vy), c in VERTS]
    sy = [32 - 32 * vy for (vx, vy), c in VERTS]
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
        u = b0 * t["uv"][0][0] + b1 * t["uv"][1][0] + b2 * t["uv"][2][0]
        v = b0 * t["uv"][0][1] + b1 * t["uv"][1][1] + b2 * t["uv"][2][1]
        fu, fv = u * TW, v * TH
        if min(abs(fu - round(fu)), abs(fv - round(fv))) < 0.06:
            return None                                    # on a texel edge
        tx, ty = min(int(fu), TW - 1), min(int(fv), TH - 1)
        return tuple(c / 255 for c in texel(tx, ty))
    cols = [c for v, c in VERTS]
    return tuple(b0 * cols[0][i] + b1 * cols[1][i] + b2 * cols[2][i] for i in range(4))


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
