#!/usr/bin/env python3
"""Compare existing 128-byte iOS AppleRegisterDump files completely offline.

These are *observations*, not a register-write recipe. No I2C, sysfs, SSH,
debugfs, subprocess or device API is accessed. Capture provenance belongs
beside the private input files; don't publish Apple's dump data by default.
"""
import argparse
import hashlib
import json
from pathlib import Path


def dump(path):
    data = path.read_bytes()
    if len(data) != 0x80:
        raise ValueError(f'{path}: expected exactly 128 bytes, found {len(data)}')
    return data


def compare(before, after):
    return [{'offset': f'0x{offset:02x}', 'before': f'0x{old:02x}',
             'after': f'0x{new:02x}', 'changed_bits': f'0x{old ^ new:02x}'}
            for offset, (old, new) in enumerate(zip(before, after)) if old != new]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('before', type=Path)
    parser.add_argument('after', type=Path)
    args = parser.parse_args()
    try:
        before, after = dump(args.before), dump(args.after)
    except (OSError, ValueError) as exc:
        parser.error(str(exc))
    print(json.dumps({
        'format': 'p105-pmu-diff-v1',
        'interpretation': 'Observed changes only; not instructions to write registers.',
        'before_sha256': hashlib.sha256(before).hexdigest(),
        'after_sha256': hashlib.sha256(after).hexdigest(),
        'changed': compare(before, after),
    }, indent=2))


if __name__ == '__main__':
    main()
