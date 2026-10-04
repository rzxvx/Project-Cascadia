#!/usr/bin/env python3
"""make.py -- writes the shader corpus for the oracle (M11,
docs/research/p105-mesa.md): one NAME.glsl per case, its vertex shader, a
"// fragment" line, its fragment shader.  Neighbouring cases differ by one
thing, so iOS's USSE for the two differs by what that thing compiles to.

    python3 tools/iosgpu/corpus/make.py        (in this directory)

gltrace corpus feeds every attribute and uniform: the attribute named "p"
is a triangle over the whole target, the others and the uniforms get
recognisable values (gltrace.m, corpus_attribs / corpus_uniforms).
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))
VS = "attribute vec4 p;\nvoid main() { gl_Position = p; }\n"
cases = {}


def frag(body, decl="uniform vec4 u0, u1, u2;", prec="mediump"):
    return "precision %s float;\n%s\nvoid main() {\n    %s\n}\n" % (prec, decl, body)


def vert(decl, body):
    return "attribute vec4 p;\n%s\nvoid main() {\n    gl_Position = p;\n    %s\n}\n" % (decl, body)


def F(name, body, decl="uniform vec4 u0, u1, u2;", prec="mediump", vs=VS):
    """a fragment shader's case, behind the plainest vertex shader"""
    cases[name] = (vs, frag(body, decl, prec))


def V(name, vdecl, vbody, fdecl, fbody, prec="mediump"):
    """a case with varyings: both shaders"""
    cases[name] = (vert(vdecl, vbody), frag(fbody, fdecl, prec))


# --- fragment arithmetic on uniforms, mediump
F("f00_const", "gl_FragColor = vec4(0.25, 0.5, 0.75, 1.0);", decl="")
F("f01_uniform", "gl_FragColor = u0;", decl="uniform vec4 u0;")
F("f02_add", "gl_FragColor = u0 + u1;")
F("f03_sub", "gl_FragColor = u0 - u1;")
F("f04_mul", "gl_FragColor = u0 * u1;")
F("f05_mad", "gl_FragColor = u0 * u1 + u2;")
F("f06_div", "gl_FragColor = u0 / u1;")
F("f07_dot3", "gl_FragColor = vec4(dot(u0.xyz, u1.xyz));")
F("f08_dot4", "gl_FragColor = vec4(dot(u0, u1));")
F("f09_min", "gl_FragColor = min(u0, u1);")
F("f10_max", "gl_FragColor = max(u0, u1);")
F("f11_clamp", "gl_FragColor = clamp(u0, 0.0, 1.0);")
F("f12_abs", "gl_FragColor = abs(u0);")
F("f13_neg", "gl_FragColor = -u0;")
F("f14_floor", "gl_FragColor = floor(u0);")
F("f15_fract", "gl_FragColor = fract(u0);")
F("f16_ceil", "gl_FragColor = ceil(u0);")
F("f17_mix", "gl_FragColor = mix(u0, u1, u2.x);")
F("f18_step", "gl_FragColor = step(u0, u1);")
F("f19_smoothstep", "gl_FragColor = smoothstep(u0, u1, u2);")
F("f20_rcp", "gl_FragColor = 1.0 / u0;", decl="uniform vec4 u0;")
F("f21_rsq", "gl_FragColor = inversesqrt(u0);", decl="uniform vec4 u0;")
F("f22_sqrt", "gl_FragColor = sqrt(u0);", decl="uniform vec4 u0;")
F("f23_exp2", "gl_FragColor = exp2(u0);", decl="uniform vec4 u0;")
F("f24_log2", "gl_FragColor = log2(u0);", decl="uniform vec4 u0;")
F("f25_pow", "gl_FragColor = pow(u0, u1);")
F("f26_exp", "gl_FragColor = exp(u0);", decl="uniform vec4 u0;")
F("f27_log", "gl_FragColor = log(u0);", decl="uniform vec4 u0;")
F("f28_sin", "gl_FragColor = sin(u0);", decl="uniform vec4 u0;")
F("f29_cos", "gl_FragColor = cos(u0);", decl="uniform vec4 u0;")
F("f30_tan", "gl_FragColor = tan(u0);", decl="uniform vec4 u0;")
F("f31_length", "gl_FragColor = vec4(length(u0.xyz));", decl="uniform vec4 u0;")
F("f32_normalize", "gl_FragColor = vec4(normalize(u0.xyz), 1.0);", decl="uniform vec4 u0;")
F("f33_cross", "gl_FragColor = vec4(cross(u0.xyz, u1.xyz), 1.0);")
F("f34_mod", "gl_FragColor = mod(u0, u1);")
F("f35_sign", "gl_FragColor = sign(u0);", decl="uniform vec4 u0;")
F("f36_swizzle", "gl_FragColor = u0.wzyx;", decl="uniform vec4 u0;")
F("f37_scalar", "gl_FragColor = u0 * u1.x;")
F("f38_scalar_add", "gl_FragColor = vec4(u0.x + u1.y);")
F("f39_reflect", "gl_FragColor = vec4(reflect(u0.xyz, u1.xyz), 1.0);")

# --- precisions: the same operations three ways
F("p00_add_lowp", "gl_FragColor = u0 + u1;", prec="lowp")
F("p01_add_mediump", "gl_FragColor = u0 + u1;", prec="mediump")
F("p02_add_highp", "gl_FragColor = u0 + u1;", prec="highp")
F("p03_mul_lowp", "gl_FragColor = u0 * u1;", prec="lowp")
F("p04_mul_highp", "gl_FragColor = u0 * u1;", prec="highp")
F("p05_rcp_highp", "gl_FragColor = 1.0 / u0;", decl="uniform vec4 u0;", prec="highp")

# --- varyings: count, size, precision, built-ins
V("v00_vec4", "attribute vec4 a;\nvarying vec4 v;", "v = a;", "varying vec4 v;", "gl_FragColor = v;")
V("v01_vec2", "attribute vec4 a;\nvarying vec2 v;", "v = a.xy;", "varying vec2 v;",
  "gl_FragColor = vec4(v, 0.0, 1.0);")
V("v02_float", "attribute vec4 a;\nvarying float v;", "v = a.x;", "varying float v;", "gl_FragColor = vec4(v);")
V("v03_vec3", "attribute vec4 a;\nvarying vec3 v;", "v = a.xyz;", "varying vec3 v;",
  "gl_FragColor = vec4(v, 1.0);")
V("v04_two", "attribute vec4 a, b;\nvarying vec4 v;\nvarying vec2 w;", "v = a;\n    w = b.xy;",
  "varying vec4 v;\nvarying vec2 w;", "gl_FragColor = v + vec4(w, 0.0, 0.0);")
V("v05_eight", "attribute vec4 a;\nvarying vec4 v0, v1, v2, v3, v4, v5, v6, v7;",
  "v0 = a; v1 = a * 2.0; v2 = a * 3.0; v3 = a * 4.0;\n    v4 = a * 5.0; v5 = a * 6.0; v6 = a * 7.0; v7 = a * 8.0;",
  "varying vec4 v0, v1, v2, v3, v4, v5, v6, v7;",
  "gl_FragColor = v0 + v1 + v2 + v3 + v4 + v5 + v6 + v7;")
V("v06_lowp", "attribute vec4 a;\nvarying lowp vec4 v;", "v = a;", "varying lowp vec4 v;", "gl_FragColor = v;")
V("v07_highp", "attribute vec4 a;\nvarying highp vec4 v;", "v = a;", "varying highp vec4 v;",
  "gl_FragColor = v;", prec="highp")
F("v08_fragcoord", "gl_FragColor = gl_FragCoord * 0.015625;", decl="")
F("v09_frontfacing", "gl_FragColor = gl_FrontFacing ? vec4(1.0) : vec4(0.5);", decl="")
V("v10_vary_mul_uniform", "attribute vec4 a;\nvarying vec4 v;", "v = a;", "varying vec4 v;\nuniform vec4 u0;",
  "gl_FragColor = v * u0;")

# --- the vertex side
FS_V = frag("gl_FragColor = v;", "varying vec4 v;")
FS_WHITE = frag("gl_FragColor = vec4(1.0);", "")
cases["x00_mvp"] = ("attribute vec4 p;\nuniform mat4 m;\nvoid main() { gl_Position = m * p; }\n", FS_WHITE)
cases["x01_attr_scale"] = (vert("attribute vec4 a;\nvarying vec4 v;", "v = a * 2.0;"), FS_V)
cases["x02_attr_uniform"] = (vert("attribute vec4 a;\nuniform vec4 k;\nvarying vec4 v;", "v = a * k + k;"), FS_V)
cases["x03_vs_sin"] = (vert("attribute vec4 a;\nvarying vec4 v;", "v = sin(a);"), FS_V)
cases["x04_vs_dot"] = (vert("attribute vec4 a;\nuniform vec4 k;\nvarying vec4 v;", "v = vec4(dot(a, k));"), FS_V)
cases["x05_pointsize"] = (vert("", "gl_PointSize = 4.0;"), FS_WHITE)
cases["x06_attr_vec2"] = (vert("attribute vec2 a;\nvarying vec4 v;", "v = vec4(a, 0.0, 1.0);"), FS_V)
cases["x07_mat3_normal"] = (vert("attribute vec3 n;\nuniform mat3 m;\nvarying vec4 v;", "v = vec4(m * n, 1.0);"), FS_V)

# --- uniforms of each shape, in the fragment shader
F("u00_float", "gl_FragColor = vec4(k);", decl="uniform float k;")
F("u01_vec2", "gl_FragColor = vec4(k, 0.0, 1.0);", decl="uniform vec2 k;")
F("u02_vec3", "gl_FragColor = vec4(k, 1.0);", decl="uniform vec3 k;")
F("u03_mat2", "gl_FragColor = vec4(m * u0.xy, 0.0, 1.0);", decl="uniform mat2 m;\nuniform vec4 u0;")
F("u04_mat3", "gl_FragColor = vec4(m * u0.xyz, 1.0);", decl="uniform mat3 m;\nuniform vec4 u0;")
F("u05_mat4", "gl_FragColor = m * u0;", decl="uniform mat4 m;\nuniform vec4 u0;")
F("u06_array", "gl_FragColor = a[1] + a[3];", decl="uniform vec4 a[4];")
F("u07_int", "gl_FragColor = vec4(float(i));", decl="uniform int i;")
F("u08_bool", "gl_FragColor = b ? vec4(1.0) : vec4(0.0);", decl="uniform bool b;")
F("u09_many", "gl_FragColor = k0 + k1 + k2 + k3 + k4 + k5 + k6 + k7;",
  decl="uniform vec4 k0, k1, k2, k3, k4, k5, k6, k7;")

# --- textures (t = the position mapped to 0..1)
TVS = vert("varying vec4 t;", "t = p * 0.5 + 0.5;")


def T(name, decl, body):
    cases[name] = (TVS, frag(body, "varying vec4 t;\n" + decl))


T("t00_tex2d", "uniform sampler2D s;", "gl_FragColor = texture2D(s, t.xy);")
T("t01_dependent", "uniform sampler2D s;", "gl_FragColor = texture2D(s, t.xy * 2.0);")
T("t02_proj", "uniform sampler2D s;", "gl_FragColor = texture2DProj(s, t.xyw);")
T("t03_bias", "uniform sampler2D s;", "gl_FragColor = texture2D(s, t.xy, 1.0);")
T("t04_cube", "uniform samplerCube c;", "gl_FragColor = textureCube(c, t.xyz - 0.5);")
T("t05_two", "uniform sampler2D s0, s1;", "gl_FragColor = texture2D(s0, t.xy) + texture2D(s1, t.yx);")
T("t06_times_uniform", "uniform sampler2D s;\nuniform vec4 k;", "gl_FragColor = texture2D(s, t.xy) * k;")
T("t07_swizzled", "uniform sampler2D s;", "gl_FragColor = texture2D(s, t.yx);")
T("t08_from_uniform", "uniform sampler2D s;\nuniform vec4 k;", "gl_FragColor = texture2D(s, k.xy);")
cases["t09_mod_like_sgx2d"] = (
    vert("attribute vec2 a;\nattribute vec4 c;\nvarying mediump vec2 t;\nvarying lowp vec4 v;", "t = a;\n    v = c;"),
    frag("gl_FragColor = texture2D(s, t) * v;", "varying vec2 t;\nvarying lowp vec4 v;\nuniform sampler2D s;"))

# --- control flow
F("c00_discard", "if (u0.x > 1.5) discard;\n    gl_FragColor = u1;")
F("c01_if_uniform", "if (u0.x > 1.5) gl_FragColor = u1; else gl_FragColor = u2;")
V("c02_if_varying", "attribute vec4 a;\nvarying vec4 v;", "v = a;", "varying vec4 v;\nuniform vec4 u0, u1;",
  "if (v.x > 0.4) gl_FragColor = u0; else gl_FragColor = u1;")
F("c03_loop", "vec4 s = vec4(0.0);\n    for (int i = 0; i < 4; i++) s += a[i];\n    gl_FragColor = s;",
  decl="uniform vec4 a[4];")
F("c04_loop_break", "vec4 s = vec4(0.0);\n    for (int i = 0; i < 8; i++) { if (s.x > 3.0) break; s += a[i]; }\n"
  "    gl_FragColor = s;", decl="uniform vec4 a[8];")
F("c05_select", "gl_FragColor = u0.x > u1.x ? u0 : u1;")

if __name__ == "__main__":
    for name, (v, f) in sorted(cases.items()):
        with open(os.path.join(HERE, name + ".glsl"), "w") as fp:
            fp.write("// vertex\n" + v + "// fragment\n" + f)
    print(len(cases), "cases in", HERE)
