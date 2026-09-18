#!/usr/bin/env python3
"""Wrap a patched, PLAINTEXT iBoot image in an img3 that carries no KBAG.

Why this exists.  An image sent to a device that reached DFU through kloader
(``./cascadia flash --kdfu``) must not be encrypted.  By the time iOS has
booted, the AES engine's GID key is no longer usable, so the KBAG of a
stock-layout img3 decrypts to nothing and the pwned iBSS jumps into garbage.
The symptom is misleading: irecovery reports 100%, and then the device drops
off the bus and looks switched off.  Legacy iOS Kit ships its own pwned iBSS
exactly this way -- xpwntool repacks the decrypted, patched binary with the
KBAG tags gone and sigCheckArea zeroed -- and that is the image the device has
been booting from kDFU all along.

The encrypted image stays the one for the checkm8 route: there the device came
up from a cold DFU and the GID key still works.  ./cascadia firmware builds
both from the same patched plaintext, so there is still exactly one source for
the boot chain -- the user's own IPSW.

Layout notes, checked against Legacy iOS Kit's pwnediBSS.dfu for this device:

    header    magic "Img3", fullSize = file size, sizeNoPack = fullSize - 20,
              sigCheckArea = 0, ident unchanged from the template
    tags      the template's, in order, minus every KBAG, with DATA's payload
              replaced by the plaintext

Usage:
  img3pack.py <template.dfu> <patched-plain.bin> <out.dfu>
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

HEADER = 20


def parse_tags(data: bytes) -> list[tuple[int, bytes, int, int]]:
    if data[:4] != b"3gmI":
        raise SystemExit("not an IMG3/DFU file")
    tags: list[tuple[int, bytes, int, int]] = []
    off = HEADER
    while off + 12 <= len(data):
        tag, total_len, data_len = struct.unpack_from("<4sII", data, off)
        if total_len < 12 or off + total_len > len(data):
            raise SystemExit(f"malformed tag at offset {off}: {tag[::-1]!r}")
        tags.append((off, tag, total_len, data_len))
        off += total_len
    return tags


def pack_plain(template: bytes, plain: bytes) -> bytes:
    tags = parse_tags(template)
    if not any(t == b"ATAD" for _, t, _, _ in tags):
        raise SystemExit("template has no ATAD tag")

    ident = template[16:20]
    out = bytearray(b"\0" * HEADER)

    for off, tag, total_len, data_len in tags:
        if tag == b"GABK":
            # The whole point.  Leaving it in would tell iBoot to decrypt a
            # payload that is already plaintext.
            continue
        if tag == b"ATAD":
            # Pad to the 16-byte boundary the encrypted payload would have
            # occupied, and keep the real length in the tag's data field.
            pad = (16 - (len(plain) % 16)) % 16
            payload = plain + b"\0" * pad
            out += struct.pack("<4sII", b"ATAD", 12 + len(payload), len(plain))
            out += payload
        else:
            out += template[off : off + total_len]

    full = len(out)
    struct.pack_into("<4sIII4s", out, 0, b"3gmI", full, full - HEADER, 0, ident)
    return bytes(out)


def describe(data: bytes) -> str:
    return " ".join(
        tag[::-1].decode("ascii", "replace") for _, tag, _, _ in parse_tags(data)
    )


def main() -> int:
    if len(sys.argv) != 4:
        print(
            f"usage: {sys.argv[0]} <template.dfu> <patched-plain.bin> <out.dfu>",
            file=sys.stderr,
        )
        return 2

    template = Path(sys.argv[1]).read_bytes()
    plain = Path(sys.argv[2]).read_bytes()
    out_path = Path(sys.argv[3])

    out = pack_plain(template, plain)

    # Re-read what was built rather than trusting the arithmetic above: a
    # wrong size here does not fail loudly, it fails on the device, at the
    # far end of a flash cycle.
    tags = parse_tags(out)
    if any(t == b"GABK" for _, t, _, _ in tags):
        raise SystemExit("KBAG survived the repack")
    magic, full, nopack, sig, _ = struct.unpack("<4sIII4s", out[:HEADER])
    if (full, nopack, sig) != (len(out), len(out) - HEADER, 0):
        raise SystemExit(f"header does not describe the file: {full} {nopack} {sig}")
    atad = next(t for t in tags if t[1] == b"ATAD")
    if atad[3] != len(plain):
        raise SystemExit(f"ATAD says {atad[3]} bytes, plaintext is {len(plain)}")

    out_path.write_bytes(out)
    print(f"wrote {out_path} ({len(out)} bytes, plaintext {len(plain)}, no KBAG)")
    print(f"    tags: {describe(out)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
