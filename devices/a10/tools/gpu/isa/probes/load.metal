kernel void l0(device uint *o [[buffer(0)]], device const uint *a [[buffer(1)]]) { o[0] = a[0]; }
kernel void l1(device uint *o [[buffer(0)]], device const uint *a [[buffer(1)]]) { o[0] = a[1]; }
kernel void l5(device uint *o [[buffer(0)]], device const uint *a [[buffer(1)]]) { o[0] = a[5]; }
kernel void l300(device uint *o [[buffer(0)]], device const uint *a [[buffer(1)]]) { o[0] = a[300]; }
kernel void lo3(device uint *o [[buffer(0)]], device const uint *a [[buffer(1)]]) { o[3] = a[0]; }
kernel void l2x(device uint2 *o [[buffer(0)]], device const uint2 *a [[buffer(1)]]) { o[0] = a[0]; }
kernel void l4x(device uint4 *o [[buffer(0)]], device const uint4 *a [[buffer(1)]]) { o[0] = a[0]; }
kernel void l8(device uchar *o [[buffer(0)]], device const uchar *a [[buffer(1)]]) { o[0] = a[0]; }
kernel void l16(device ushort *o [[buffer(0)]], device const ushort *a [[buffer(1)]]) { o[0] = a[0]; }
kernel void lsame(device uint *o [[buffer(0)]]) { o[1] = o[0]; }
kernel void l3b(device uint *o [[buffer(0)]], device const uint *a [[buffer(1)]], device const uint *b [[buffer(2)]]) { o[0] = b[0]; o[1] = a[0]; }
