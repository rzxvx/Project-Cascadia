#!/usr/bin/env python3
"""Read-only inventory of a pinned local Apple kernelcache; no MMIO/device access.
Outputs addresses and selected diagnostic labels, never keys or firmware bytes.
"""
import argparse
import hashlib
import json
import struct
import xml.etree.ElementTree as ET
from pathlib import Path

EXPECTED = 'db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c'
LABELS = ('fmiss_ppn_read_multi', 'fmiss_ppn_write_multi', 'fmiss_ppn_prep_write_multi',
          'h2fmi_ppn_write_bootpage', 'h2fmi_ppn_erase_blocks',
          'h2fmi_ppn_process_error_list', 'h2fmi_iop_write_multiple',
          'h2fmi_write_multi', 'h2fmi_ppn_iop_write_multiple',
          'h2fmi_ppn_iop_erase_multiple', 's_write_multi_internal', 'h2fmi_ppn_prep_erase_blocks',
          'h2fmi_ppn_get_operation_status', 'h2fmi_ppn_get_erase_error_list',
          'h2fmi_ppn_get_program_error_list',
          'IOReturn AppleIOPFMI::_fmiSendCommand(Channel *, AppleIOPFMIDMACommand *)',
          'IOReturn AppleIOPFMI::_fmiWaitForCommand(Channel *, AppleIOPFMIDMACommand *)',
          'IOReturn AppleIOPFMI::_fmiRunCommand(Channel *, AppleIOPFMIDMACommand *)',
          'void AppleIOPFMI::_iopEvent(UInt32, void *)',
          'void AppleIOPFMI::_fmiPerformPpnOperation(AppleIOPFMIDMACommand *)',
          'static void AppleIOPFMI::_fmiPpnCompletion(AppleIOPFMIDMACommand *)',
          'void AppleIOPFMIDMACommand::generatePpnDmaLists(IOFlashStoragePPNCommand *)')


def segments(data, base):
    magic, _, _, _, ncmds, sizeofcmds, _ = struct.unpack_from('<7I', data, base)
    if magic != 0xfeedface or ncmds > 128 or sizeofcmds > 65536:
        raise ValueError('invalid Mach-O header')
    end = base + 28 + sizeofcmds
    if end > len(data):
        raise ValueError('truncated Mach-O commands')
    pos, out = base + 28, []
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', data, pos)
        if size < 8 or pos + size > end:
            raise ValueError('invalid Mach-O command size')
        if cmd == 1:
            if size < 56:
                raise ValueError('short segment command')
            name = data[pos+8:pos+24].split(b'\0')[0].decode('ascii')
            va, vs, fo, fs, _, _, ns, _ = struct.unpack_from('<8I', data, pos+24)
            if 56 + ns * 68 > size:
                raise ValueError('truncated Mach-O sections')
            ss = []
            for k in range(ns):
                at = pos + 56 + k*68
                sn = data[at:at+16].split(b'\0')[0].decode('ascii')
                addr, length, offset = struct.unpack_from('<3I', data, at+32)
                ss.append({'name': sn, 'address': addr, 'size': length, 'offset': offset})
            out.append({'name': name, 'address': va, 'size': vs,
                        'offset': fo, 'file_size': fs, 'sections': ss})
        pos += size
    if pos != end:
        raise ValueError('load command size mismatch')
    return out


def inventory(data):
    sha = hashlib.sha256(data).hexdigest()
    if sha != EXPECTED:
        raise ValueError('kernelcache does not match pinned official 12H321 extraction')
    top = segments(data, 0)
    def vaoff(va):
        matches = [s['offset'] + va-s['address'] for s in top
                   if s['address'] <= va < s['address']+s['file_size']]
        if len(matches) != 1:
            raise ValueError('ambiguous/unmapped address')
        return matches[0]
    def offva(off):
        matches = [s['address'] + off-s['offset'] for s in top
                   if s['offset'] <= off < s['offset']+s['file_size']]
        if len(matches) != 1:
            raise ValueError('ambiguous/unmapped offset')
        return matches[0]
    info = next(s for s in top if s['name'] == '__PRELINK_INFO')
    # Apple's prelink serialization has an unwrapped dict, ID/IDREF nodes and
    # Latin-1 copyright bytes. Resolve object references explicitly, not plistlib.
    xml = data[info['offset']:info['offset']+info['file_size']].rstrip(b'\0')
    tree = ET.fromstring(xml.decode('latin1'))
    ids = {n.attrib['ID']: n for n in tree.iter() if 'ID' in n.attrib}
    def decode(node, seen=frozenset()):
        if 'IDREF' in node.attrib:
            key = node.attrib['IDREF']
            if key in seen:
                raise ValueError('cyclic prelink reference')
            return decode(ids[key], seen | {key})
        if node.tag == 'dict':
            items = list(node)
            if len(items) % 2 or any(items[i].tag != 'key' for i in range(0,len(items),2)):
                raise ValueError('invalid prelink dictionary')
            return {items[i].text: decode(items[i+1], seen) for i in range(0,len(items),2)}
        if node.tag == 'array':
            return [decode(c, seen) for c in node]
        if node.tag == 'integer':
            return int(node.text, 0)
        if node.tag in ('true', 'false'):
            return node.tag == 'true'
        return node.text
    kexts = decode(tree)['_PrelinkInfoDictionary']
    def owner(va):
        found = [k for k in kexts if k.get('_PrelinkExecutableLoadAddr',0) <= va <
                 k.get('_PrelinkExecutableLoadAddr',0)+k.get('_PrelinkExecutableSize',0)]
        if len(found) != 1:
            raise ValueError('unmapped/overlapping kext')
        return found[0]
    labels, used = [], {}
    for label in LABELS:
        start, needle = 0, label.encode()+b'\0'
        while True:
            at = data.find(needle, start)
            if at < 0: break
            start = at+1
            va = offva(at)
            k = owner(va)
            ident = k['CFBundleIdentifier']
            if ident not in used:
                used[ident] = {'name': k['CFBundleName'], 'version': k['CFBundleVersion'],
                              'address': k['_PrelinkExecutableLoadAddr'],
                              'size': k['_PrelinkExecutableSize'],
                              'segments': segments(data, vaoff(k['_PrelinkExecutableLoadAddr']))}
            lo = vaoff(k['_PrelinkExecutableLoadAddr'])
            hi = lo+k['_PrelinkExecutableSize']
            ptr, ptrs, p = struct.pack('<I', va), [], lo
            while True:
                p = data.find(ptr,p,hi)
                if p < 0: break
                if p % 4 == 0: ptrs.append(offva(p))
                p += 1
            entry = {'label': label, 'file_offset': at, 'address': va,
                     'kext': ident, 'aligned_pointer_locations': ptrs}
            firmware = [ss for seg in used[ident]['segments'] for ss in seg['sections']
                        if ss['name'] == 'fwFirmwareImage' and
                        ss['address'] <= va < ss['address']+ss['size']]
            if firmware:
                fw = firmware[0]
                fwlo = vaoff(fw['address'])
                relative = va-fw['address']
                entry['firmware_image_offset'] = relative
                entry['firmware_offset_literal_locations'] = []
                p = fwlo
                needle = struct.pack('<I', relative)
                while True:
                    p = data.find(needle, p, fwlo+fw['size'])
                    if p < 0: break
                    if p % 4 == 0:
                        entry['firmware_offset_literal_locations'].append(p-fwlo)
                    p += 1
                used[ident]['firmware_sha256'] = hashlib.sha256(data[fwlo:fwlo+fw['size']]).hexdigest()
            labels.append(entry)
    return {'source_sha256': sha, 'source_length': len(data), 'ios_build': '12H321',
            'warning': 'Diagnostic names and aligned pointer candidates are not decoded instruction callsites or protocol semantics.',
            'kexts': used, 'labels': labels}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('kernelcache', type=Path)
    a = p.parse_args()
    print(json.dumps(inventory(a.kernelcache.read_bytes()), indent=2))

if __name__ == '__main__':
    main()
