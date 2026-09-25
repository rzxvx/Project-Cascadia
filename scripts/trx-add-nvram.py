#!/usr/bin/env python3
"""Append a Broadcom NVRAM file to a TRX firmware image, for USB download.

    trx-add-nvram.py FIRMWARE.trx NVRAM.txt OUT

The BCM4334 on the iPad's HSIC port runs Apple's wifi/4334b1/borg.trx, and
iOS hands it the module's NVRAM separately (wifi/4334b1/<module>.txt): the
crystal, the PA calibration, the country.  Without it the firmware was
downloaded, told to run, and never came up.  brcmfmac over USB sends a TRX
and nothing else, so the NVRAM rides inside it, the way Broadcom's own USB
host driver (bcmdhd's dbus) packs it:

  - the firmware part, header + offsets[0] bytes, as it is;
  - the NVRAM right after it, as brcmfmac lays NVRAM out for SDIO and PCIe
    (brcmf_fw_nvram_strip): "key=value" strings, each NUL-terminated,
    comments and blank lines dropped, one more NUL, padded to four bytes,
    then the length token  words | ~words << 16;
  - offsets[2] (the NVRAM length) set, len updated, and the CRC redone:
    the complement of CRC-32 over everything from flag_version to len --
    checked against Apple's own header before anything is written.

The kernel side is one line in brcmfmac's check_file(): download
offsets[0] + offsets[2] bytes after the header, not offsets[0] alone.
"""
import struct
import sys
import zlib

HDR = 28          # magic, len, crc32, flag_version, offsets[3]


def trx_crc(img):
    return zlib.crc32(img[12:]) ^ 0xFFFFFFFF


def nvram_blob(text):
    lines = []
    for raw in text.splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line or "=" not in line:
            continue
        key, val = line.split("=", 1)
        lines.append("%s=%s" % (key.strip(), val.strip()))
    body = "\0".join(lines).encode("ascii") + b"\0"
    length = (len(body) + 1 + 3) & ~3            # roundup(len + 1, 4)
    body = body.ljust(length, b"\0")
    words = length // 4
    token = ((~words & 0xFFFF) << 16) | (words & 0xFFFF)
    return body + struct.pack("<I", token)


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    fw = open(sys.argv[1], "rb").read()
    magic, ln, crc, flags = struct.unpack_from("<4sIII", fw, 0)
    offs = list(struct.unpack_from("<3I", fw, 16))
    if magic != b"HDR0":
        sys.exit("trx-add-nvram: %s is not a TRX image" % sys.argv[1])
    if trx_crc(fw[:ln]) != crc:
        sys.exit("trx-add-nvram: the input's own CRC does not check -- wrong idea of the format")
    if offs[2]:
        sys.exit("trx-add-nvram: the input already carries %d bytes of NVRAM" % offs[2])

    nv = nvram_blob(open(sys.argv[2], encoding="ascii", errors="strict").read())
    img = bytearray(fw[:HDR + offs[0]] + nv)
    offs[2] = len(nv)
    struct.pack_into("<I", img, 4, len(img))
    struct.pack_into("<3I", img, 16, *offs)
    struct.pack_into("<I", img, 8, trx_crc(bytes(img)))
    open(sys.argv[3], "wb").write(img)
    print("    %s + %d bytes of NVRAM -> %s (%d bytes)" % (sys.argv[1], len(nv), sys.argv[3], len(img)))


if __name__ == "__main__":
    main()
