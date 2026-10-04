#!/usr/bin/env python3
"""fakepack.py -- a template frame for running the driver on a PC.

    python3 mesa/host/fakepack.py OUTDIR

Writes pack.txt, tmpl.bin and the two images the way rpack.py packs the
real one (./cascadia gpu), from tools/sgx/frame.py's GL window, with two
differences: the 3D pass's event program (iOS's GL driver's, from the
IPSW) is zeros, and there is no microkernel data. Nothing in it comes from
Apple, so it can be made anywhere; the GPU could not run it, but through
drm-shim nothing runs: what it is for is SGX_DEBUG=frame, every word a
clear or a draw would hand the GPU (mesa/host/build.sh, mesa/host/run).
The code base is the one the sgx noop shim reports, 0x9a000000.
"""
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'sgx'))
import frame  # noqa: E402
import pds    # noqa: E402
import rpack  # noqa: E402

CODE_BASE = 0x9a000000

if len(sys.argv) != 2:
    sys.exit(__doc__)
out = sys.argv[1]
os.makedirs(out, exist_ok=True)

pds.event_program = lambda path, eor, eot, done: (struct.pack('<42I', *([0] * 42)), 5)
g = frame.gl_window(None)
open(os.path.join(out, 'm_gl.bin'), 'wb').write(g)
open(os.path.join(out, 'm_cmd.bin'), 'wb').write(struct.pack('<I', 0x120) + bytes(0x11c))

t = bytearray()
lines = []
for name, va, size in rpack.TMPL:
    off = (len(t) + 0x7f) & ~0x7f
    t += b'\0' * (off - len(t)) + g[va - frame.GL_VA:va - frame.GL_VA + size]
    lines.append('tmpl_%s %d %d' % (name, off, size))
open(os.path.join(out, 'tmpl.bin'), 'wb').write(t)

v = frame.vdm_stream()
tail = v[v.index(0xc0000000) - 4:v.index(0xc0000000) + 1]
p = ['map 0x98956000 0x6f000', 'map 0x98f00000 0x4000', 'map 0x9a001000 0x1000',
     'map 0x9b400000 0x400000', 'map 0x9d000000 0x4000000', 'map 0x87b00000 0x1000',
     'map 0x87c00000 0x10000', 'map 0x88000000 0x1000',
     'img 0x98956000 m_gl.bin', 'img 0x87b00000 m_cmd.bin',
     'poke 0x35100a18 0x%x' % (CODE_BASE >> 6), 'kick 0x88000000 0x87c00000 0x87b00000',
     'screen 768 1024', 'consts0 0x%x' % (frame.STATE + 0xa0),
     'consts 0x%x' % (frame.STATE + 0x380),
     'idx 0x%x 8192' % frame.IDX, 'vdm 0x98f00000 0x4000', 'ext 0x9b400000 0x400000',
     'texheap 0x9d000000 0x4000000', 'fetch 9 0x07800604',
     'tail ' + ' '.join('0x%08x' % w for w in tail)] + lines
open(os.path.join(out, 'pack.txt'), 'w').write('\n'.join(p) + '\n')
print('fakepack.py: a template frame in %s' % out)
