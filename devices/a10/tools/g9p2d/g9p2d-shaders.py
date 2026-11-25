#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""g9p2d's compute kernel (Metal source below), built by Xcode's offline
compiler for applegpu_g9p into build/gpu/g9p2d.bin (G9PT, as
tools/gpu/g9pshaders.pack): "comp" = the USC code, "comp.temps" = u32, the
temporary registers the compiler's metadata asks for. Compiler output: stays
in build/, never in git.

One workgroup of 128 threads composites one 64 x 32 tile of the target, a
quarter at a time: each thread reads its four pixels of the quarter once,
applies the tile's quads in order and writes the pixels back. Its
arguments, as the A10's compute ABI has them (docs/gpu.md "2D on the GPU"):
buffer(0) -> the frame's tile records {x0 | y0 << 16, first list entry,
count, env offset}, buffer(1) -> the arena (textures, the targets, the frame
data; the display at its end); the threadgroup position is the tile's index
(from the kernel record's own PDS data).
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "../gpu"))
import g9pshaders  # noqa: E402

SRC = r"""#include <metal_stdlib>
using namespace metal;
struct op {
    float4 s, t;        // quad parameters: s = s.x * x + s.y * y + s.z, t alike; inside: [0, 1)
    float4 uv;          // texel (uv.x + s * uv.z, uv.y + t * uv.w)
    float4 color;       // multiplies the texel
    uint tex, tstride, tw, th;      // arena offset (0: no texture), texels per row, size
    uint mode, bb0, bb1, pad;       // 0 NONE, 1 BLEND, 2 ADD, 3 MOD; bounding box: int16 x | y << 16
};
struct env { uint ops, list, target, stride, width, height, rgba, pad; };

static float3 unpack(uint v, uint rgba)
{
    float3 c = float3((v >> 16) & 255, (v >> 8) & 255, v & 255) * (1.0f / 255);
    return rgba ? c.bgr : c;
}

#define WG 128      // threads; 48 temps each: 256 do not fit (the job never ends)
#define PX 4        // pixels per thread per part (2: 40 temps, no faster)
#define ROWS (WG * PX / 64)
kernel void comp(device const uint4 *tiles [[buffer(0)]], device uchar *arena [[buffer(1)]],
                 uint tile [[threadgroup_position_in_grid]], uint lid [[thread_index_in_threadgroup]])
{
    uint4 r = tiles[tile];
    device const env &e = *(device const env *)(arena + r.w);
    device uint *fb = (device uint *)(arena + e.target);
    device const uint *l = (device const uint *)(arena + e.list) + r.y;
    uint x0 = r.x & 0xffff, y0 = r.x >> 16;
    for (uint part = 0; part < 2048 / (WG * PX); part++) {   // ROWS rows of the tile, PX pixels per thread
    float3 c[PX];
    for (uint k = 0; k < PX; k++) {
        uint i = lid + WG * k + PX * WG * part, px = x0 + (i & 63), py = y0 + (i >> 6);
        c[k] = px < e.width && py < e.height ? unpack(fb[py * e.stride + px], e.rgba) : float3(0);
    }
    for (uint n = 0; n < r.z; n++) {
        device const op &o = *(device const op *)(arena + e.ops + 128 * l[n]);
        int by0 = int(short(o.bb0 >> 16)), by1 = int(short(o.bb1 >> 16));
        int py0 = int(y0 + ROWS * part);
        if (by1 <= py0 || by0 >= py0 + ROWS)       // the quad misses this quarter: the whole group skips it
            continue;
        for (uint k = 0; k < PX; k++) {
            uint i = lid + WG * k + PX * WG * part;
            float fx = float(x0 + (i & 63)) + 0.5f, fy = float(y0 + (i >> 6)) + 0.5f;
            float s = o.s.x * fx + o.s.y * fy + o.s.z, t = o.t.x * fx + o.t.y * fy + o.t.z;
            if (!(s >= 0 && s < 1 && t >= 0 && t < 1))
                continue;
            float4 src = o.color;
            if (o.tex) {
                uint u = uint(clamp(o.uv.x + s * o.uv.z, 0.0f, float(o.tw - 1)));
                uint v = uint(clamp(o.uv.y + t * o.uv.w, 0.0f, float(o.th - 1)));
                uint x = ((device const uint *)(arena + o.tex))[v * o.tstride + u];
                src *= float4(x & 255, (x >> 8) & 255, (x >> 16) & 255, x >> 24) * (1.0f / 255);
            }
            if (o.mode == 0)
                c[k] = src.rgb;
            else if (o.mode == 1)
                c[k] = src.rgb * src.a + c[k] * (1 - src.a);
            else if (o.mode == 2)
                c[k] = min(src.rgb * src.a + c[k], 1.0f);
            else
                c[k] = src.rgb * c[k];
        }
    }
    for (uint k = 0; k < PX; k++) {
        uint i = lid + WG * k + PX * WG * part, px = x0 + (i & 63), py = y0 + (i >> 6);
        if (px < e.width && py < e.height) {
            uint3 q = uint3(saturate(c[k]) * 255 + 0.5f);
            fb[py * e.stride + px] = e.rgba ? 0xff000000u | (q.b << 16) | (q.g << 8) | q.r
                                            : 0xff000000u | (q.r << 16) | (q.g << 8) | q.b;
        }
    }
    }
}
"""


def temps(tag, index):
    """The temporary register count in the kernel's __GPU_METADATA: the word
    after 0x01010000 (8 for store_const, which runs with DOUTU TEMPS 2 x 4)."""
    path = os.path.join(g9pshaders.ISA, tag + ".g9p.metallib")
    buf = open(path, "rb").read()
    found = []
    pos = 0
    while True:
        pos = buf.find(b"\xcf\xfa\xed\xfe", pos + 4)
        if pos < 0:
            break
        ncmds = struct.unpack_from("<8I", buf, pos)[4]
        off = pos + 32
        if struct.unpack_from("<I", buf, off)[0] != 0x19:
            continue
        for i in range(struct.unpack_from("<I", buf, off + 64)[0]):
            s = off + 72 + 80 * i
            if buf[s + 16:s + 32].rstrip(b"\0") == b"__GPU_METADATA":
                _, size, fo = struct.unpack_from("<QQI", buf, s + 32)
                md = buf[pos + fo:pos + fo + size]
                w = struct.unpack_from("<%dI" % (len(md) // 4), md)
                found.append(w[w.index(0x01010000) + 1] if 0x01010000 in w else None)
        del ncmds
    return found[index]


code = g9pshaders.build("g9p2d", SRC + "kernel void empty() {}\n", ["comp", "empty"])
t = temps("g9p2d", 0)
out = os.path.join(HERE, "../../build/gpu/g9p2d.bin")
g9pshaders.pack({"comp": code["comp"], "comp.temps": struct.pack("<I", t), "empty": code["empty"]}, out)
print(f"comp: {len(code['comp']):#x} bytes, {t} temps -> {os.path.relpath(out)}")
