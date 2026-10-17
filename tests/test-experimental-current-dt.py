#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compile the real P105 tree plus inactive wiring; never install or boot it."""
import argparse
from pathlib import Path
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
TARGET = '/soc@30000000/i2c@3200000/pmu@3c/current-controller'

def tree(data):
    header = struct.unpack_from('>10I', data)
    assert header[0] == 0xd00dfeed and header[1] == len(data)
    pos, strings = header[2], header[3]
    nodes, stack = {}, []
    while True:
        token, = struct.unpack_from('>I', data, pos); pos += 4
        if token == 1:
            end = data.index(0, pos); name = data[pos:end].decode()
            stack.append(name); pos = (end + 4) & ~3
            path = '/' + '/'.join(stack[1:]); assert path not in nodes
            nodes[path] = {}
        elif token == 2:
            stack.pop()
        elif token == 3:
            size, off = struct.unpack_from('>II', data, pos); pos += 8
            name = data[strings+off:data.index(0, strings+off)].decode()
            path = '/' + '/'.join(stack[1:]); assert name not in nodes[path]
            nodes[path][name] = data[pos:pos+size]; pos = (pos+size+3) & ~3
        elif token == 4:
            continue
        elif token == 9:
            assert not stack
            return nodes
        else:
            raise AssertionError(f'unsupported FDT token {token}')

def validate(nodes):
    n = nodes[TARGET]
    assert n['status'] == b'disabled\0'
    assert n['compatible'] == b'apple,d1946-current-only-experimental\0'
    assert n['interrupts'] == struct.pack('>II', 19, 8)
    assert set(n) == {'status','compatible','interrupt-parent','interrupts',
                      'apple,battery-supply','apple,usb-source-controller'}
    handles = {}
    for path, props in nodes.items():
        if 'phandle' in props:
            h = props['phandle']; assert len(h) == 4 and h not in handles
            handles[h] = path
    for prop, path in [('interrupt-parent','/soc@30000000/gpio@fa00000'),
                       ('apple,battery-supply','/soc@30000000/hdq@2a00000'),
                       ('apple,usb-source-controller','/soc@30000000/usb@6100000')]:
        assert handles[n[prop]] == path
    assert nodes['/soc@30000000/gpio@fa00000']['#interrupt-cells'] == struct.pack('>I',2)
    assert nodes['/soc@30000000/i2c@3200000/pmu@3c']['compatible'] == b'apple,pmu-d1946\0'

def main():
    a = argparse.ArgumentParser(description=__doc__)
    a.add_argument('--dtc', required=True, type=Path, help='existing dtc executable')
    args = a.parse_args(); dtc = args.dtc.resolve()
    base = ROOT/'dts/p105ap.dts'
    fragment = ROOT/'experiments/p105-current/p105-current-disabled.dtsi'
    with tempfile.TemporaryDirectory() as tmp:
        p = Path(tmp); outputs = []
        for label, content in [('base',base.read_text()),
                               ('candidate',base.read_text()+'\n'+fragment.read_text())]:
            src=p/(label+'.dts');src.write_text(content);dst=p/(label+'.dtb')
            result=subprocess.run([str(dtc),'-I','dts','-O','dtb','-o',str(dst),str(src)],capture_output=True,text=True)
            assert result.returncode == 0,result.stderr
            warnings=[x.replace(str(src),'<source>') for x in result.stderr.splitlines()]
            outputs.append((tree(dst.read_bytes()),warnings))
        old, old_warnings=outputs[0];new,new_warnings=outputs[1]
        validate(new)
        assert set(new)-set(old)=={TARGET} and set(old)<=set(new)
        for path, props in old.items():
            # New references may assign phandles to previously unreferenced
            # labels. All existing non-phandle properties must stay identical.
            assert {k:v for k,v in props.items() if k not in ('phandle','linux,phandle')} == {k:v for k,v in new[path].items() if k not in ('phandle','linux,phandle')},path
        assert old_warnings == new_warnings,(old_warnings,new_warnings)
        print(f'PASS real compiled disabled wiring, {len(old)} existing nodes unchanged; {len(old_warnings)} baseline diagnostic lines, no new diagnostics')
        for prop, value in [('status',b'okay\0'),('interrupts',struct.pack('>II',23,8)),
                            ('interrupts',struct.pack('>II',19,1)),
                            ('apple,usb-source-controller',new[TARGET]['apple,battery-supply']),
                            ('apple,p105-current-only-experiment',b'')]:
            bad={k:dict(v) for k,v in new.items()};bad[TARGET][prop]=value
            try: validate(bad)
            except (AssertionError,KeyError): pass
            else: raise AssertionError('invalid candidate accepted: '+prop)
        print('PASS 5 negative controls: activation, wrong pin/type/provider, permission assertion')

if __name__ == '__main__':
    main()
