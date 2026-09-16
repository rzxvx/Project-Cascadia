#!/usr/bin/env python3
import struct, sys, os, zlib

COMPLZSS_HEADER_SIZE = 0x180

def lzss_decompress(data, expected_size):
    N, F, THR = 4096, 18, 2
    ring = bytearray(b' ' * N)
    r = N - F
    out = bytearray()
    src = 0; flags = 0; flag_bits = 0
    while src < len(data) and len(out) < expected_size:
        if flag_bits == 0:
            if src >= len(data): break
            flags = data[src]; src += 1; flag_bits = 8
        if flags & 1:
            if src >= len(data): break
            c = data[src]; src += 1
            out.append(c); ring[r] = c; r = (r+1) & (N-1)
        else:
            if src+1 >= len(data): break
            b1 = data[src]; src += 1
            b2 = data[src]; src += 1
            pos = b1 | ((b2 & 0xF0) << 4)
            length = (b2 & 0x0F) + THR + 1
            for k in range(length):
                c = ring[(pos+k) & (N-1)]
                out.append(c); ring[r] = c; r = (r+1) & (N-1)
                if len(out) >= expected_size: break
        flags >>= 1; flag_bits -= 1
    return bytes(out)

def make_payload(compressed, unc_size, checksum):
    hdr = struct.pack(">6I", 0x636f6d70, 0x6c7a7373, checksum, unc_size, len(compressed), 0)
    pad = b'\x00' * (COMPLZSS_HEADER_SIZE - len(hdr))
    return hdr + pad + compressed

with open(sys.argv[1], 'rb') as f:
    raw = f.read()

_, _, cur_cs, unc_size, cmp_size, _ = struct.unpack_from(">6I", raw, 0)
compressed = raw[COMPLZSS_HEADER_SIZE:COMPLZSS_HEADER_SIZE + cmp_size]
print(f"unc_size={unc_size}, cmp_size={cmp_size}, current_cs=0x{cur_cs:08x}")

print("Распаковываем...")
decompressed = lzss_decompress(compressed, unc_size)
print(f"распаковано: {len(decompressed)} байт")

outdir = sys.argv[2]; os.makedirs(outdir, exist_ok=True)
variants = {
    "adler_unc_1.complzss": zlib.adler32(decompressed, 1) & 0xFFFFFFFF,
    "adler_unc_0.complzss": zlib.adler32(decompressed, 0) & 0xFFFFFFFF,
    "adler_cmp_1.complzss": zlib.adler32(compressed,   1) & 0xFFFFFFFF,
    "adler_cmp_0.complzss": zlib.adler32(compressed,   0) & 0xFFFFFFFF,
}
for name, cs in variants.items():
    payload = make_payload(compressed, unc_size, cs)
    with open(os.path.join(outdir, name), 'wb') as f: f.write(payload)
    print(f"  {name}: 0x{cs:08x}")
