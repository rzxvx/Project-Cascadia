# SPDX-License-Identifier: MIT
"""Our USC code for the A10 (applegpu_g9p), built on the Mac by Xcode's offline
compiler (`metal-tt`), devices/a10/docs/gpu.md "G6 legalization" / "G5".
Compiler output stays in build/gpu/isa/, never in git.

    own_shaders()   -> {"tri_vs", "tri_fs", "bg_fs", "empty": bytes}  (OWN_SRC)
    g5_shaders()    -> {"empty", "store_const": bytes}               (G5_SRC)
"""
import os
import subprocess

import g9pbin

HERE = os.path.dirname(os.path.abspath(__file__))
ISA = os.path.join(HERE, "../../build/gpu/isa")

OWN_SRC = """#include <metal_stdlib>
using namespace metal;
vertex float4 tri_vs(uint vid [[vertex_id]]) {
    float2 p[3] = { float2(-1, -1), float2(0, -1), float2(-1, 1) };   // the compiler puts these 6
    return float4(p[vid], 0, 1);                                       // words in the common store
}
fragment float4 tri_fs() { return float4(1, 0, 0, 1); }
fragment float4 bg_fs(constant float4 &c [[buffer(0)]]) { return c; }
kernel void empty() {}
"""


def own_shaders():
    """Our USC code, built by Xcode's offline compiler for applegpu_g9p from
    OWN_SRC (build/gpu/isa/own.*; compiler output, not in git). Returns
    {name: bytes}."""
    d = ISA
    os.makedirs(d, exist_ok=True)
    src = os.path.join(d, "own.metal")
    if not os.path.exists(src) or open(src).read() != OWN_SRC:
        open(src, "w").write(OWN_SRC)
    lib, tt = os.path.join(d, "own.metallib"), os.path.join(d, "own.g9p.metallib")
    if not os.path.exists(tt) or os.path.getmtime(tt) < os.path.getmtime(src):
        subprocess.run(["xcrun", "-sdk", "iphoneos", "metal", "-std=metal3.0", "-mios-version-min=16.4",
                        "-c", src, "-o", lib[:-9] + ".air"], check=True)
        subprocess.run(["xcrun", "-sdk", "iphoneos", "metallib", lib[:-9] + ".air", "-o", lib], check=True)
        js = os.path.join(d, "own.mtlp-json")
        fmt = '{ "pixel_format": "BGRA8Unorm" }'
        open(js, "w").write(
            '{ "libraries": { "paths": [ { "label": "lib", "path": "%s" } ] },\n'
            '  "pipelines": { "render_pipelines": [\n'
            '    { "vertex_function": "alias:lib#tri_vs", "fragment_function": "alias:lib#tri_fs", "color_attachments": [ %s ] },\n'
            '    { "vertex_function": "alias:lib#tri_vs", "fragment_function": "alias:lib#bg_fs", "color_attachments": [ %s ] } ],\n'
            '  "compute_pipelines": [ { "compute_function": "alias:lib#empty" } ] } }\n' % (lib, fmt, fmt))
        subprocess.run(["xcrun", "metal-tt", "-arch", "applegpu_g9p", "-platform_version", "ios", "16.4", "16.4",
                        lib, js, "-o", tt], check=True)
    vs = g9pbin.kernels(tt, "__vertex")
    fs = g9pbin.kernels(tt, "__fragment")
    cs = g9pbin.kernels(tt, "__compute")
    return {"tri_vs": vs[0], "tri_fs": fs[0], "bg_fs": fs[1], "empty": cs[0]}


# devices/a13/tools/gpu/isa/g5-kernels.metal, the kernels of G5
G5_SRC = """#include <metal_stdlib>
using namespace metal;
kernel void store_const(device uint *out [[buffer(0)]], uint tid [[thread_position_in_grid]]) {
    out[tid] = 0xcafe0000u + tid;
}
kernel void empty() {}
"""


def g5_shaders():
    """store_const as in G5 (build/gpu/isa/g5.*: the same pipelines, empty then
    store_const). Returns {name: bytes}."""
    os.makedirs(ISA, exist_ok=True)
    src = os.path.join(ISA, "g5k.metal")
    if not os.path.exists(src) or open(src).read() != G5_SRC:
        open(src, "w").write(G5_SRC)
    lib, tt = os.path.join(ISA, "g5k.metallib"), os.path.join(ISA, "g5k.g9p.metallib")
    if not os.path.exists(tt) or os.path.getmtime(tt) < os.path.getmtime(src):
        subprocess.run(["xcrun", "-sdk", "iphoneos", "metal", "-std=metal3.0", "-mios-version-min=16.4",
                        "-c", src, "-o", lib[:-9] + ".air"], check=True)
        subprocess.run(["xcrun", "-sdk", "iphoneos", "metallib", lib[:-9] + ".air", "-o", lib], check=True)
        js = os.path.join(ISA, "g5k.mtlp-json")
        open(js, "w").write(
            '{ "libraries": { "paths": [ { "label": "lib", "path": "%s" } ] },\n'
            ' "pipelines": { "compute_pipelines": [ { "compute_function": "alias:lib#empty" }, '
            '{ "compute_function": "alias:lib#store_const" } ] } }\n' % lib)
        subprocess.run(["xcrun", "metal-tt", "-arch", "applegpu_g9p", "-platform_version", "ios", "16.4", "16.4",
                        lib, js, "-o", tt], check=True)
    cs = g9pbin.kernels(tt)
    return {"empty": cs[0], "store_const": cs[1]}
