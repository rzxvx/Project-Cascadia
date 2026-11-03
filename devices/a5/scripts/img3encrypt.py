#!/usr/bin/env python3
"""Re-encrypt a patched iBoot image into Apple IPSW DFU/IMG3 format.

Bootrom DFU expects the stock layout: TYPE tag + encrypted ATAD payload
(AES-CBC), not plaintext DATA. Keys/IV match the IPSW manifest (P105AP).

Usage:
  img3encrypt.py <template.dfu> <patched-plain.bin> <out.dfu> <iv-hex> <key-hex>
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

from Crypto.Cipher import AES


def parse_tags(data: bytes) -> list[tuple[int, bytes, int, int]]:
    if data[:4] != b"3gmI":
        raise SystemExit("not an IMG3/DFU file")
    tags: list[tuple[int, bytes, int, int]] = []
    off = 20
    while off + 12 <= len(data):
        tag, total_len, data_len = struct.unpack_from("<4sII", data, off)
        if total_len < 12:
            break
        tags.append((off, tag, total_len, data_len))
        off += total_len
    return tags


def encrypt_payload(plain: bytes, iv: bytes, key: bytes) -> bytes:
    pad = (16 - (len(plain) % 16)) % 16
    padded = plain + (b"\0" * pad)
    return AES.new(key, AES.MODE_CBC, iv).encrypt(padded)


def replace_atad(template: bytes, plain: bytes, iv: bytes, key: bytes) -> bytes:
    tags = parse_tags(template)
    atad = next(((o, t, tl, dl) for o, t, tl, dl in tags if t == b"ATAD"), None)
    if not atad:
        raise SystemExit("template has no ATAD tag")

    atad_off, _, tag_total, plain_len = atad
    if len(plain) != plain_len:
        raise SystemExit(
            f"patched size {len(plain)} != template plaintext {plain_len}"
        )

    cipher_len = tag_total - 12
    cipher = encrypt_payload(plain, iv, key)
    if len(cipher) != cipher_len:
        raise SystemExit(
            f"cipher size {len(cipher)} != template ATAD cipher {cipher_len}"
        )

    out = bytearray(template)
    out[atad_off + 12 : atad_off + tag_total] = cipher
    return bytes(out)


def main() -> int:
    if len(sys.argv) != 6:
        print(
            f"usage: {sys.argv[0]} <template.dfu> <patched.bin> <out.dfu> "
            "<iv-hex> <key-hex>",
            file=sys.stderr,
        )
        return 2

    template = Path(sys.argv[1]).read_bytes()
    plain = Path(sys.argv[2]).read_bytes()
    out_path = Path(sys.argv[3])
    iv = bytes.fromhex(sys.argv[4])
    key = bytes.fromhex(sys.argv[5])

    out = replace_atad(template, plain, iv, key)
    out_path.write_bytes(out)
    print(f"wrote {out_path} ({len(out)} bytes, plaintext {len(plain)})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
