#!/usr/bin/env python3
"""Check exact official P105 ADT port declarations; no device or I/O backend."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct

INPUTS = {
    '12H321': '5c346586116de1bb32606acb27cad3158bcaa8a1318a10ee9631de09884cfd72',
    '13G36': 'e8277fd827ba250de3f46567cb1c910d4e1c80633d666510a7780e00f4ad4b59',
}
EXPECTED = {
    '/device-tree/arm-io/usb-complex/usb-ehci/usb-ehci-port1': 0x30002,
    '/device-tree/arm-io/usb-complex/usb-ohci0/usb-ohci0-port1': 0x30006,
}


def check(data, version, parse_node):
    digest = hashlib.sha256(data).hexdigest()
    if version not in INPUTS or digest != INPUTS[version]:
        raise ValueError('exact official P105 ADT hash required')
    tree, end = parse_node(data, 0)
    if end != len(data):
        raise ValueError('unexpected bytes after ADT')
    selected = {}

    def walk(node, parent=''):
        name = node['props']['name'].split(b'\0', 1)[0].decode('ascii')
        path = parent + '/' + name
        if path in EXPECTED:
            fields = {}
            for key, value in (('port', 1), ('port-type', 2),
                               ('clock-mask', EXPECTED[path]), ('port-current-limit', 50)):
                raw = node['props'].get(key)
                if raw != struct.pack('<I', value):
                    raise ValueError(f'{path}: mismatched {key}')
                fields[key] = value
            selected[path] = fields
        for child in node['children']:
            walk(child, path)

    walk(tree)
    if set(selected) != set(EXPECTED):
        raise ValueError('expected EHCI/OHCI0 port1 declarations absent')
    return {'version': version, 'adt_sha256': digest, 'declared_ports': selected,
            'physical_wiring_verified': False, 'vbus_source_verified': False,
            'linux_host_tested': False, 'hardware_access': False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('adt_12H321', type=Path)
    parser.add_argument('adt_13G36', type=Path)
    parser.add_argument('--source-tree', type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    try:
        path = args.source_tree / 'src/dtparse.py'
        spec = importlib.util.spec_from_file_location('cascadia_dtparse', path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        results = [check(path.read_bytes(), version, module.parse_node)
                   for path, version in ((args.adt_12H321, '12H321'), (args.adt_13G36, '13G36'))]
    except (OSError, ValueError, KeyError, UnicodeError) as error:
        parser.exit(1, f'error: {error}\n')
    print(json.dumps({'inputs': results, 'hardware_access': False}, indent=2))


if __name__ == '__main__':
    main()
