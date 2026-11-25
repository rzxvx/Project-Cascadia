#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Probe kernels for the compute ABI (thread / threadgroup IDs, several
buffers): build/gpu/g9p-probe.bin for tools/gpu/g9p-probe.c."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import g9pshaders  # noqa: E402

SRC = """#include <metal_stdlib>
using namespace metal;
kernel void store_const(device uint *out [[buffer(0)]], uint tid [[thread_position_in_grid]]) {
    out[tid] = 0xcafe0000u + tid;
}
kernel void ids(device uint *out [[buffer(0)]], uint3 tg [[threadgroup_position_in_grid]],
                uint3 tl [[thread_position_in_threadgroup]], uint3 tpg [[threads_per_threadgroup]]) {
    uint i = (tg.x + 8 * tg.y) * 64 + tl.x + 8 * tl.y;
    out[i] = 0xa0000000u | (tg.y << 20) | (tg.x << 14) | (tl.y << 7) | tl.x;
    out[1024 + i] = (tpg.x << 16) | tpg.y;
}
kernel void two(device uint *a [[buffer(0)]], device uint *b [[buffer(1)]]) {
    a[0] = 0x11111111u;
    b[0] = 0x22222222u;
}
kernel void four(device uint *a [[buffer(0)]], device uint *b [[buffer(1)]], device uint *c [[buffer(2)]],
                 device uint *d [[buffer(3)]]) {
    a[0] = 0x11111111u; b[0] = 0x22222222u; c[0] = 0x33333333u; d[0] = 0x44444444u;
}
kernel void tg1(device uint *out [[buffer(0)]], uint tg [[threadgroup_position_in_grid]],
                uint tl [[thread_index_in_threadgroup]]) {
    out[(tg & 15) * 64 + (tl & 63)] = (tg << 16) | tl;
}
kernel void tg3(device uint *out [[buffer(0)]], uint3 tg [[threadgroup_position_in_grid]],
                uint tl [[thread_index_in_threadgroup]]) {
    out[((tg.x + tg.y * 4) & 15) * 64 + (tl & 63)] = (tg.x << 20) | (tg.y << 10) | tg.z;
}
kernel void tpg(device uint *out [[buffer(0)]], uint3 n [[threads_per_threadgroup]], uint3 g [[threadgroups_per_grid]],
                uint tl [[thread_index_in_threadgroup]]) {
    if (tl == 0) { out[0] = n.x; out[1] = n.y; out[2] = n.z; out[3] = g.x; out[4] = g.y; out[5] = g.z; }
}
kernel void tgw(device uint *out [[buffer(0)]], uint tg [[threadgroup_position_in_grid]],
                uint tl [[thread_index_in_threadgroup]]) {
    out[tg * 128 + tl] = (tg << 16) | tl;
}
kernel void tgw2(device uint *out [[buffer(0)]], device uint *b [[buffer(1)]], uint tg [[threadgroup_position_in_grid]],
                 uint tl [[thread_index_in_threadgroup]]) {
    out[tg * 128 + tl] = (tg << 16) | tl;
    if (tl == 0) b[tg] = 0xb0000000u | tg;
}
kernel void tgw3(device const uint4 *tiles [[buffer(0)]], device uchar *arena [[buffer(1)]],
                 uint tile [[threadgroup_position_in_grid]], uint lid [[thread_index_in_threadgroup]]) {
    uint4 r = tiles[tile];
    ((device uint *)(arena + r.x))[lid] = (tile << 16) | lid;
}
kernel void load(device uint *a [[buffer(0)]], uint tid [[thread_position_in_grid]]) {
    a[256 + tid] = a[tid] * 3u + 1u;
}
"""
code = g9pshaders.build("probe1", SRC, ["store_const", "ids", "two", "four", "load", "tg1", "tg3", "tpg", "tgw", "tgw2", "tgw3"])
out = os.path.join(HERE, "../../build/gpu/g9p-probe.bin")
g9pshaders.pack(code, out)
for k, c in code.items():
    print(f"{k}: {len(c):#x} bytes")
