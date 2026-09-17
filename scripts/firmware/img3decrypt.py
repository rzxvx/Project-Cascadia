#!/usr/bin/env python3
"""Decrypt an Apple IMG3/IMG4 DATA payload.

This is kept in-tree so the iBEC build pipeline is self-contained and does not
depend on external paths outside the repo.
outside the project directory.
"""
import struct
import sys
from Crypto.Cipher import AES


def parse_img3(data: bytes) -> tuple[bytes, int]:
    magic, _total_len, _data_len = struct.unpack_from("<4sII", data, 0)
    if magic != b"3gmI":
        raise SystemExit(f"not an IMG3 file (got {magic!r})")
    off = 20
    while off < len(data):
        tag, tag_total_len, tag_data_len = struct.unpack_from("<4sII", data, off)
        if tag == b"ATAD":
            # The ciphertext region is (tag_total_len - 12) bytes and is what is
            # actually AES-block-aligned. tag_data_len is the TRUE plaintext size
            # and must only be applied as a truncation AFTER decryption.
            cipher_len = tag_total_len - 12
            if cipher_len % 16 != 0:
                raise SystemExit(f"ciphertext len {cipher_len} not block-aligned")
            return data[off + 12 : off + 12 + cipher_len], tag_data_len
        off += tag_total_len
    raise SystemExit("no DATA tag found")


def main() -> int:
    if len(sys.argv) != 5:
        print(f"usage: {sys.argv[0]} <in.img3> <out.bin> <iv-hex> <key-hex>",
              file=sys.stderr)
        return 2
    infile, outfile, iv_hex, key_hex = sys.argv[1:]
    raw = open(infile, "rb").read()
    ciphertext, plain_len = parse_img3(raw)
    plain = AES.new(bytes.fromhex(key_hex), AES.MODE_CBC,
                    bytes.fromhex(iv_hex)).decrypt(ciphertext)[:plain_len]
    open(outfile, "wb").write(plain)
    print(f"wrote {len(plain)} bytes to {outfile} "
          f"(ciphertext was {len(ciphertext)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
