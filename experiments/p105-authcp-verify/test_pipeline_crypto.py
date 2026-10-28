#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Real signatures through pinned whole actual-C 32/32 THS/AuthCPAID pipeline."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
from test_authcp_verify import sign, spki
from cryptography.hazmat.primitives.asymmetric import ec, rsa
from authcp_verify import verify

PINS = {
    'experiments/p105-ths-idbus/cascadia-ths-idbus.c': 'bd6da8ec07730a79c5d3bfc9b936085bd67016c02bfc70337a0b69978f5af61c',
    'experiments/p105-authcp-aid/cascadia-authcp-aid.c': '382cc9dd37bd18d37e9466830fe109c0b29bbe1bdab6885bf65acdff2584b529',
    'experiments/p105-ths-idbus/authcp-ths-window13-oracle.json': '78b0091f7f704676495e3121d378eba9d6abfe8234318f686af8aacb37e48e75',
    'experiments/p105-ths-idbus/cascadia-ths-idbus.h': '7a29ca7b1eca0a85d1d5d212c429948179c7f86daf475619041b6c20b6a69f3d',
    'experiments/p105-authcp-aid/cascadia-authcp-aid.h': '87f49ff6b1ad50869125612e9563d5d512aa800e3deb3a0a1fa8ce8f4d15fd33',
}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', required=True, type=Path)
    args = parser.parse_args()
    root = args.source_root
    here = Path(__file__).resolve().parent
    for name, expected in PINS.items():
        if hashlib.sha256((root / name).read_bytes()).hexdigest() != expected:
            raise ValueError('pinned whole-C source/oracle changed: ' + name)
    oracle = json.loads((root / 'experiments/p105-ths-idbus/authcp-ths-window13-oracle.json').read_text())
    def array(data):
        return '{' + ','.join(str(v) for v in data) + '}'
    lines = []
    for case in oracle['cases']:
        lines.append('static const struct wire_frame v' + str(case['version']) + '[]={')
        for frame in case['transfers']:
            tx, rx = bytes.fromhex(frame['tx']), bytes.fromhex(frame['rx'])
            lines.append('{' + array(tx) + ',' + array(rx) + ',' + str(len(tx)) + ',' + str(len(rx)) + '},')
        lines.append('};')
    rows = []
    with tempfile.TemporaryDirectory(prefix='cascadia-authcp-real-crypto-') as td:
        path = Path(td)
        (path / 'frames.inc').write_text('\n'.join(lines) + '\n')
        executable = path / 'pipeline'
        subprocess.run(['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie', '-pthread',
                        '-I' + str(here / 'include'), '-I' + str(root / 'experiments/p105-ths-idbus'),
                        '-I' + str(root / 'experiments/p105-authcp-aid'), '-I' + str(path),
                        str(root / 'experiments/p105-ths-idbus/cascadia-ths-idbus.c'),
                        str(root / 'experiments/p105-authcp-aid/cascadia-authcp-aid.c'),
                        str(here / 'pipeline-crypto-fixture.c'), '-o', str(executable)], check=True)
        keys = {2: rsa.generate_private_key(public_exponent=65537, key_size=1024),
                3: ec.generate_private_key(ec.SECP256R1())}
        for version, key in keys.items():
            nonce = bytes(range(20 if version == 2 else 32))
            signature = sign(key, version, nonce)
            for late in (False, True):
                result = subprocess.run([str(executable), str(version), '1' if late else '0'],
                                        input=signature, capture_output=True, check=True,
                                        env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1:halt_on_error=1'})
                if result.stderr:
                    raise RuntimeError(result.stderr.decode())
                reconstructed = result.stdout
                assert len(reconstructed) == len(signature)
                expected = signature[:31] + signature[:31] + signature[62:] if late else signature
                assert reconstructed == expected
                outcome = verify(version, nonce, reconstructed, spki(key))
                assert outcome.signature_valid is (not late)
                assert outcome.authorization_granted is False
                assert outcome.certificate_chain_trusted is None
                rows.append({'version': version, 'late_chunk_substitution': late,
                             'parser_completed_unverified': True,
                             'real_signature_valid': outcome.signature_valid,
                             'authorization_granted': outcome.authorization_granted})
        for version in (2, 3):
            for invalid in (b'', b'x' * 129):
                bad = subprocess.run([str(executable), str(version), '0'], input=invalid, capture_output=True)
                assert bad.returncode == 2 and not bad.stdout and not bad.stderr
    print(json.dumps({'scope': 'synthetic signatures/leases/wire; actual unchanged whole Linux-C; real host crypto',
                      'source_pins': PINS, 'cases': rows}, indent=2))

if __name__ == '__main__':
    main()
