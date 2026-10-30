#!/usr/bin/env python3
"""Remove ipsw 3.1.714's invalid LZSS ASN.1 compression record.

Keep the compressed payload, manifest, type and description byte-for-byte.
The input is preserved; the destination must be a different file.
"""

import argparse
from pathlib import Path


def item(data, start=0):
    pos = start
    tag = data[pos]
    pos += 1
    if tag & 31 == 31:
        while data[pos] & 128:
            pos += 1
        pos += 1
    length = data[pos]
    pos += 1
    if length & 128:
        count = length & 127
        if not count:
            raise ValueError("Indefinite DER length")
        length = int.from_bytes(data[pos:pos+count], 'big')
        pos += count
    end = pos + length
    if end > len(data):
        raise ValueError("Truncated DER object")
    return tag, data[pos:end], data[start:end], end


def children(data):
    result = []
    pos = 0
    while pos < len(data):
        tag, body, encoded, pos = item(data, pos)
        result.append((tag, body, encoded))
    return result


def sequence(body):
    length = len(body)
    if length < 128:
        encoded_length = bytes([length])
    else:
        encoded = length.to_bytes((length.bit_length()+7)//8, 'big')
        encoded_length = bytes([128+len(encoded)]) + encoded
    return b'\x30' + encoded_length + body


def repair(data):
    tag, body, _, end = item(data)
    if tag != 0x30 or end != len(data):
        raise ValueError("Expected one IMG4 SEQUENCE")
    outer = children(body)
    if outer[0][:2] != (0x16, b'IMG4') or outer[1][0] != 0x30:
        raise ValueError("Expected IMG4 with IM4P")
    payload = children(outer[1][1])
    if len(payload) != 5 or payload[0][:2] != (0x16, b'IM4P'):
        raise ValueError("Expected four IM4P fields and one compression record")
    if payload[3][0] != 4 or not payload[3][1].startswith(b'complzss'):
        raise ValueError("Expected LZSS payload")
    if payload[4][0] != 0x30:
        raise ValueError("Expected ASN.1 compression SEQUENCE")
    record = children(payload[4][1])
    uncompressed = int.from_bytes(payload[3][1][12:16], 'big')
    if len(record) != 2 or any(part[0] != 2 for part in record):
        raise ValueError("Unexpected compression record")
    if int.from_bytes(record[0][1], 'big') != 0:
        raise ValueError("Compression record is not LZSS enum 0")
    if int.from_bytes(record[1][1], 'big') != uncompressed:
        raise ValueError("Compression record size does not match complzss header")
    new_payload = sequence(b''.join(part[2] for part in payload[:4]))
    result = sequence(outer[0][2] + new_payload + b''.join(part[2] for part in outer[2:]))
    new_outer = children(item(result)[1])
    new_parts = children(new_outer[1][1])
    assert new_parts == payload[:4]
    assert new_outer[0] == outer[0] and new_outer[2:] == outer[2:]
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('destination', type=Path)
    args = parser.parse_args()
    if args.source.resolve() == args.destination.resolve():
        raise ValueError("Keep the original; use a different destination")
    data = args.source.read_bytes()
    fixed = repair(data)
    args.destination.write_bytes(fixed)
    print(f"Removed compression record: {len(data)} -> {len(fixed)} bytes")


if __name__ == '__main__':
    main()
