#!/usr/bin/env python3
"""tmpl-test.py -- the template frame as Mesa builds it (sgx_template.c, M17)
against sgx2d's pack, byte for byte, at the pack's addresses.

    python3 mesa/host/tmpl-test.py [PACKDIR]     (default build/sgx2d/pack)

Builds tmpl-test.c with sgx_template.c and sgx_rt.c and runs it: the PDS
block, the state area, the index buffer, the programs, the stream's tail,
the TA command and the 3D register block.  The pack is ./cascadia gpu's.
"""
import os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
SGX = os.path.join(ROOT, 'mesa', 'files', 'src', 'gallium', 'drivers', 'sgx')

pack = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'build', 'sgx2d', 'pack')
exe = os.path.join(tempfile.mkdtemp(prefix='tmpl-test.'), 'tmpl-test')
subprocess.run(['cc', '-O1', '-Wall', '-Werror', '-I', SGX, '-o', exe,
                os.path.join(HERE, 'tmpl-test.c'), os.path.join(SGX, 'sgx_template.c'),
                os.path.join(SGX, 'sgx_rt.c')], check=True)
sys.exit(subprocess.run([exe, pack]).returncode)
