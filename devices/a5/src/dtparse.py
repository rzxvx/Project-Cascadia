#!/usr/bin/env python3
import sys, struct

def read_u32(data, off):
    if off + 4 > len(data):
        raise ValueError(f"read_u32 out of bounds at off={off:#x} (file len={len(data):#x})")
    return struct.unpack_from('<I', data, off)[0], off + 4

def parse_property(data, off, path):
    if off + 36 > len(data):
        raise ValueError(f"property header out of bounds at off={off:#x} in {path}")
    name = data[off:off+32].split(b'\x00', 1)[0].decode('ascii', errors='replace')
    off += 32
    raw_len, off = read_u32(data, off)
    length = raw_len & 0x7fffffff
    if off + length > len(data):
        raise ValueError(f"property '{name}' in {path} claims length={length:#x} (raw={raw_len:#x}) "
                          f"at off={off:#x}, but file len={len(data):#x} -- OVERRUN")
    value = data[off:off+length]
    padded_len = (length + 3) & ~3
    off += padded_len
    return name, value, off

def parse_node(data, off, path="/"):
    nprops, off = read_u32(data, off)
    nchildren, off = read_u32(data, off)
    if nprops > 1000 or nchildren > 1000:
        raise ValueError(f"implausible nprops={nprops} nchildren={nchildren} at {path} off={off:#x} -- desynced")
    props = {}
    for _ in range(nprops):
        name, value, off = parse_property(data, off, path)
        props[name] = value
    node_name = props.get('name', b'?').split(b'\x00')[0].decode('ascii', errors='replace')
    children = []
    for idx in range(nchildren):
        child, off = parse_node(data, off, path + node_name + "/")
        children.append(child)
    return {'props': props, 'children': children}, off

def decode_value(value):
    stripped = value.rstrip(b'\x00')
    if len(value) == 4 and not stripped:
        return "0"
    if len(value) == 4:
        try:
            return str(struct.unpack('<I', value)[0])
        except Exception:
            pass
    if stripped and all(32 <= b < 127 or b == 0 for b in stripped):
        parts = [p.decode('ascii', errors='replace') for p in stripped.split(b'\x00')]
        return ",".join(parts) if len(parts) > 1 else parts[0]
    return value.hex() if value else ""

def print_node(node, path="/"):
    indent = path.count("/") - 1
    pad = "  " * indent
    print(f"{pad}[{path}]")
    for name, value in node['props'].items():
        print(f"{pad}  {name} = {decode_value(value)}")
    node_name = node['props'].get('name', b'?').split(b'\x00')[0].decode('ascii', errors='replace')
    for child in node['children']:
        print_node(child, path + node_name + "/")

if __name__ == "__main__":
    infile = sys.argv[1]
    dump = "--dump" in sys.argv[2:]
    with open(infile, 'rb') as f:
        data = f.read()
    try:
        root, end_off = parse_node(data, 0)
        print(f"OK: parsed cleanly, end_off={end_off:#x}, file_len={len(data):#x}, "
              f"leftover={len(data)-end_off} bytes")
        if dump:
            print_node(root, "/")
    except ValueError as e:
        print(f"DESYNC: {e}")
        sys.exit(1)
