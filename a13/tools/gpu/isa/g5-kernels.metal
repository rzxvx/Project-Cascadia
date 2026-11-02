#include <metal_stdlib>
using namespace metal;
kernel void store_const(device uint *out [[buffer(0)]], uint tid [[thread_position_in_grid]]) {
    out[tid] = 0xcafe0000u + tid;
}
kernel void empty() {}
