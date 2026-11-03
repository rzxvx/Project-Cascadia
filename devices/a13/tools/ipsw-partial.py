#!/usr/bin/env python3
"""
Extract individual files from a remote IPSW (ZIP) using HTTP range requests.

Usage:  python3 ipsw-partial.py <url> <path-in-zip> [-o outdir]
        python3 ipsw-partial.py <url> <path1> <path2> ... [-o outdir]

Downloads only the central directory (~64 KB from the end) and the requested
files themselves.  An iPhone 11 16.4 IPSW is ~6 GB; the two files make-inputs
needs are ~1.7 MB total.

Replaces `ipsw download ipsw --pattern`, which relies on the iTunes API
lookup that fails on Linux ("failed to create itunes API: unexpected EOF").
"""
import io
import os
import struct
import sys
import urllib.request
import zlib


def http_range(url, start, end):
    """Fetch bytes [start, end) from url."""
    req = urllib.request.Request(url)
    req.add_header("Range", f"bytes={start}-{end - 1}")
    with urllib.request.urlopen(req) as r:
        return r.read()


def find_eocd(url):
    """Download enough of the tail to find the End-of-Central-Directory."""
    # EOCD is at least 22 bytes; with a comment it can be up to 22 + 65535.
    # ZIP64 EOCD locator is 20 bytes before it.  64 KB covers any realistic
    # case.  We need the file size first.
    req = urllib.request.Request(url, method="HEAD")
    with urllib.request.urlopen(req) as r:
        size = int(r.headers["Content-Length"])

    tail_size = min(size, 65536 + 22)
    tail = http_range(url, size - tail_size, size)

    # Scan backwards for EOCD signature (PK\x05\x06)
    sig = b"PK\x05\x06"
    pos = tail.rfind(sig)
    if pos == -1:
        raise ValueError("no EOCD found")

    eocd = tail[pos:]
    # EOCD: sig(4) disk(2) cd_disk(2) cd_entries_disk(2) cd_entries(2)
    #       cd_size(4) cd_offset(4) comment_len(2) = 22 bytes
    _, _, _, _, _, cd_size, cd_offset, _ = struct.unpack_from("<4sHHHHIIH", eocd)

    # ZIP64: if cd_offset == 0xFFFFFFFF, look for the ZIP64 EOCD locator
    if cd_offset == 0xFFFFFFFF:
        loc_sig = b"PK\x06\x07"
        lpos = tail.rfind(loc_sig)
        if lpos == -1:
            raise ValueError("ZIP64 EOCD locator not found")
        z64_offset = struct.unpack_from("<Q", tail, lpos + 8)[0]
        z64_eocd = http_range(url, z64_offset, z64_offset + 64)
        # ZIP64 EOCD: sig(4) size_of_record(8) version_made(2) version_needed(2)
        #   disk(4) cd_disk(4) cd_entries_disk(8) cd_entries(8) cd_size(8) cd_offset(8)
        cd_size_64, cd_offset_64 = struct.unpack_from("<QQ", z64_eocd, 40)
        cd_size = cd_size_64
        cd_offset = cd_offset_64

    return size, cd_offset, cd_size


def parse_central_directory(cd_data):
    """Parse the central directory into a dict of {path: (offset, comp_size, uncomp_size, method)}."""
    entries = {}
    pos = 0
    while pos + 46 <= len(cd_data):
        if cd_data[pos : pos + 4] != b"PK\x01\x02":
            break
        # Central directory file header (46 bytes fixed):
        #  0: sig(4)  4: version_made(2)  6: version_needed(2)  8: flags(2)
        # 10: method(2) 12: mod_time(2) 14: mod_date(2) 16: crc(4)
        # 20: comp_size(4) 24: uncomp_size(4) 28: name_len(2) 30: extra_len(2)
        # 32: comment_len(2) 34: disk_start(2) 36: int_attr(2) 38: ext_attr(4)
        # 42: local_offset(4)
        method = struct.unpack_from("<H", cd_data, pos + 10)[0]
        comp_size = struct.unpack_from("<I", cd_data, pos + 20)[0]
        uncomp_size = struct.unpack_from("<I", cd_data, pos + 24)[0]
        name_len = struct.unpack_from("<H", cd_data, pos + 28)[0]
        extra_len = struct.unpack_from("<H", cd_data, pos + 30)[0]
        comment_len = struct.unpack_from("<H", cd_data, pos + 32)[0]
        local_offset = struct.unpack_from("<I", cd_data, pos + 42)[0]

        name = cd_data[pos + 46 : pos + 46 + name_len].decode("utf-8", errors="replace")

        # ZIP64 extra field: if sizes or offset are 0xFFFFFFFF, read from extra
        if comp_size == 0xFFFFFFFF or uncomp_size == 0xFFFFFFFF or local_offset == 0xFFFFFFFF:
            extra = cd_data[pos + 46 + name_len : pos + 46 + name_len + extra_len]
            epos = 0
            while epos + 4 <= len(extra):
                eid, esz = struct.unpack_from("<HH", extra, epos)
                if eid == 1:  # ZIP64
                    vals = []
                    vpos = epos + 4
                    while vpos + 8 <= epos + 4 + esz:
                        vals.append(struct.unpack_from("<Q", extra, vpos)[0])
                        vpos += 8
                    vi = 0
                    if uncomp_size == 0xFFFFFFFF and vi < len(vals):
                        uncomp_size = vals[vi]; vi += 1
                    if comp_size == 0xFFFFFFFF and vi < len(vals):
                        comp_size = vals[vi]; vi += 1
                    if local_offset == 0xFFFFFFFF and vi < len(vals):
                        local_offset = vals[vi]
                    break
                epos += 4 + esz

        entries[name] = (local_offset, comp_size, uncomp_size, method)
        pos += 46 + name_len + extra_len + comment_len

    return entries


def extract_file(url, entry):
    """Download and decompress one file from the remote ZIP."""
    local_offset, comp_size, uncomp_size, method = entry

    # Read local file header to find the start of file data
    header = http_range(url, local_offset, local_offset + 30)
    name_len, extra_len = struct.unpack_from("<HH", header, 26)
    data_offset = local_offset + 30 + name_len + extra_len

    raw = http_range(url, data_offset, data_offset + comp_size)

    if method == 0:  # stored
        return raw
    elif method == 8:  # deflated
        return zlib.decompress(raw, -15)
    else:
        raise ValueError(f"unsupported compression method {method}")


def main():
    import argparse

    p = argparse.ArgumentParser(description="Extract files from a remote IPSW (ZIP)")
    p.add_argument("url", help="IPSW URL")
    p.add_argument("paths", nargs="+", help="paths inside the ZIP to extract")
    p.add_argument("-o", "--output", default=".", help="output directory")
    args = p.parse_args()

    print(f"   reading central directory...", flush=True)
    size, cd_offset, cd_size = find_eocd(args.url)
    cd_data = http_range(args.url, cd_offset, cd_offset + cd_size)
    entries = parse_central_directory(cd_data)

    os.makedirs(args.output, exist_ok=True)

    for path in args.paths:
        if path not in entries:
            print(f"   error: '{path}' not in the IPSW", file=sys.stderr)
            sys.exit(1)
        entry = entries[path]
        print(f"   downloading {path} ({entry[2]} bytes)...", flush=True)
        data = extract_file(args.url, entry)
        out = os.path.join(args.output, os.path.basename(path))
        with open(out, "wb") as f:
            f.write(data)
        print(f"   saved {out}")


if __name__ == "__main__":
    main()
