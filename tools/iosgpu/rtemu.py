# Emulate the IMGSGX543 kext's render-target init (0x80bf7aac) with unicorn.
# Usage: KC841=path/to/kc841.macho python3 rtemu.py W H [samples [ncores [payload.txt]]]
# Stage 1 runs the RT init on a fake device and dumps the RT object and every buffer it allocates
# (rt_<W>x<H>_b<i>.bin, GPU VAs faked from 0x87900000). Stage 2 (with a payload of words w0..,
# hex) runs the per-submit 3D block builder 0x80bf5eec and prints the words the kernel fills in.
# Needs the user's own decrypted 8.4.1 kernelcache; it executes Apple code but writes only results.
import struct, sys, json
from unicorn import *
from unicorn.arm_const import *
import os
exec(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'kctool.py')).read().split("if __name__")[0])

W = int(sys.argv[1], 0); H = int(sys.argv[2], 0)
SAMPLES = int(sys.argv[3], 0) if len(sys.argv) > 3 else 1
NCORES = int(sys.argv[4], 0) if len(sys.argv) > 4 else 2

KLO, KHI = 0x80bf1000, 0x80c20000
HEAP, HEAPSZ = 0x10000000, 0x01000000      # fake kernel objects
BUF, BUFSZ = 0x40000000, 0x04000000        # CPU maps of GPU buffers
STK, STKSZ = 0x20000000, 0x00100000
TRAP, TRAPSZ = 0x30000000, 0x1000          # fake virtual functions
GPU_BASE = 0x87900000                      # fake GPU VAs, inside the TA heap

mu = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
for b, s in ((KLO, KHI - KLO), (HEAP, HEAPSZ), (BUF, BUFSZ), (STK, STKSZ), (TRAP, TRAPSZ)):
    mu.mem_map(b, s)
for va in range(KLO, KHI, 4):
    o = va2off(va)
    if o is not None: mu.mem_write(va, D[o:o + 4])
mu.reg_write(UC_ARM_REG_C1_C0_2, mu.reg_read(UC_ARM_REG_C1_C0_2) | (0xf << 20))
mu.reg_write(UC_ARM_REG_FPEXC, 0x40000000)

hp = [HEAP]
def halloc(n):
    a = hp[0]; hp[0] += (n + 0xff) & ~0xff; return a
def w32(a, v): mu.mem_write(a, struct.pack('<I', v & 0xffffffff))
def r32(a): return struct.unpack('<I', mu.mem_read(a, 4))[0]

# trap table: index -> handler(name)
traps = {}
def trap(name, fn):
    a = TRAP + 4 * len(traps); traps[a] = (name, fn); return a | 1
def vtable(entries):
    vt = halloc(0x200)
    for off, a in entries.items(): w32(vt + off, a)
    return vt

bufs = []          # (gpu, cpu, size, opts)
gp = [GPU_BASE]; cp = [BUF]
def ret1(*a): return 1
def ret0(*a): return 0

def new_memobj(size, opts):
    size = (size + 0xfff) & ~0xfff
    gpu = gp[0]; gp[0] += size
    cpu = cp[0]; cp[0] += size
    bufs.append(dict(gpu=gpu, cpu=cpu, size=size, opts=opts))
    mp = halloc(0x40); w32(mp, vtable({0x38: trap('map.cpu', lambda *a: cpu)}))
    inner = halloc(0x40)
    w32(inner, vtable({0x70: trap('size', lambda *a: size), 0x98: trap('mkmap', lambda *a: mp)}))
    d = halloc(0x100)
    w32(d + 0x18, gpu); w32(d + 0x1c, 0); w32(d + 0x2c, size); w32(d + 0x30, mp); w32(d + 0x3c, inner)
    m = halloc(0x100)
    idx = len(bufs) - 1
    def fill(r0, r1, r2, r3):
        bufs[idx]['fill'] = (r1, r2); return 1
    w32(m, vtable({0x4c: trap('memobj.alloc', fill), 0x14: trap('release', ret0)}))
    w32(m + 0x3c, d)
    return m

def stub_alloc(r0, r1, r2, r3):
    sp = mu.reg_read(UC_ARM_REG_SP)
    return new_memobj(r3, r32(sp))
def stub_bzero(r0, r1, *a):
    mu.mem_write(r0, b'\0' * r1); return 0
def stub_memcpy(r0, r1, r2, *a):
    mu.mem_write(r0, bytes(mu.mem_read(r1, r2))); return r0

STUBS = {0x80bfe4f4: ('alloc', stub_alloc), 0x80bfe8b4: ('bzero', stub_bzero),
         0x80bfe924: ('memcpy', stub_memcpy), 0x80bfe3f4: ('tail', ret0)}
log = []
def hook(uc, addr, size, ud):
    h = None
    if addr in STUBS: h = STUBS[addr]
    elif addr in traps: h = traps[addr]
    if h is None: return
    a = [uc.reg_read(r) for r in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)]
    rv = h[1](*a)
    log.append('%s(%s) -> %#x' % (h[0], ', '.join('%#x' % x for x in a), rv or 0))
    uc.reg_write(UC_ARM_REG_R0, rv or 0)
    uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
mu.hook_add(UC_HOOK_CODE, hook, begin=TRAP, end=TRAP + TRAPSZ)
for a in STUBS: mu.hook_add(UC_HOOK_CODE, hook, begin=a, end=a + 1)

def unmapped(uc, access, addr, size, value, ud):
    # calls into the kernel proper (superclass init): return 1
    if access == UC_MEM_FETCH_UNMAPPED:
        log.append('kernel call %#x -> 1' % addr)
        uc.mem_map(addr & ~0xfff, 0x1000)
        uc.mem_write(addr & ~1, b'\x01\x20\x70\x47')  # movs r0,#1; bx lr
        return True
    if access == UC_MEM_READ_UNMAPPED and va2off(addr) is not None:
        pg = addr & ~0xfff; uc.mem_map(pg, 0x1000)
        for va in range(pg, pg + 0x1000, 4):
            o = va2off(va)
            if o is not None: uc.mem_write(va, D[o:o + 4])
        return True
    print('unmapped %s at %#x (pc %#x)' % (access, addr, uc.reg_read(UC_ARM_REG_PC))); return False
mu.hook_add(UC_HOOK_MEM_UNMAPPED, unmapped)
def blk(uc, addr, size, ud):
    if KLO <= addr < KHI or TRAP <= addr < TRAP + TRAPSZ: return
    log.append('kernel call %#x -> 1' % addr)
    uc.reg_write(UC_ARM_REG_R0, 1)
    uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
mu.hook_add(UC_HOOK_BLOCK, blk)

rt = halloc(0x200)
dev = halloc(0x2000); w32(dev + 0x5c8, NCORES)
owner = halloc(0x100)
params = halloc(0x70)
w32(params + 0, 0); w32(params + 4, W); w32(params + 8, H); w32(params + 12, SAMPLES)

END = TRAP + 0xffc
mu.reg_write(UC_ARM_REG_SP, STK + STKSZ - 0x100)
mu.reg_write(UC_ARM_REG_LR, END | 1)
for r, v in ((UC_ARM_REG_R0, rt), (UC_ARM_REG_R1, dev), (UC_ARM_REG_R2, owner), (UC_ARM_REG_R3, params)):
    mu.reg_write(r, v)
mu.emu_start(0x80bf7aac | 1, END, count=5_000_000)
rv = mu.reg_read(UC_ARM_REG_R0)
for l in log: print('  ', l)
print('init ->', rv)
f = [r32(rt + i) for i in range(0, 0x140, 4)]
for i in range(0, len(f), 8):
    print('rt+%03x: %s' % (i * 4, ' '.join('%08x' % x for x in f[i:i + 8])))
out = dict(W=W, H=H, samples=SAMPLES, ncores=NCORES, rt=f, bufs=[])
for i, b in enumerate(bufs):
    data = bytes(mu.mem_read(b['cpu'], b['size']))
    fn = 'rt_%dx%d_b%d.bin' % (W, H, i)
    open(fn, 'wb').write(data)
    nz = sum(1 for k in range(0, len(data), 4) if data[k:k + 4] != b'\0\0\0\0')
    print('buf%d gpu %#x size %#x opts %#x fill %s nonzero words %d' %
          (i, b['gpu'], b['size'], b['opts'], b.get('fill'), nz))
    out['bufs'].append(dict(gpu=b['gpu'], size=b['size'], opts=b['opts'], file=fn))
json.dump(out, open('rt_%dx%d.json' % (W, H), 'w'))

# ---- stage 2: the per-submit 3D block builder (0x80bf5eec) on a captured payload ----
if len(sys.argv) > 5:
    pw = [int(x, 16) for x in open(sys.argv[5]).read().split()]   # payload words w0..
    pay = halloc(0x200)
    for i, v in enumerate(pw[2:]): w32(pay + 4 * i, v)
    ctx = halloc(0x1400)
    w32(ctx + 0x94, dev)
    w32(ctx + 0x1268, 0)
    blk_m = new_memobj(0x1000, 0)
    w32(ctx + 0xc08, r32(blk_m + 0x3c))
    o1e8 = halloc(0x40); w32(o1e8, vtable({0x90: trap('dev.v90', ret0), 0xa8: trap('dev.va8', ret0)}))
    w32(dev + 0x1e8, o1e8)
    log.clear()
    mu.reg_write(UC_ARM_REG_SP, STK + STKSZ - 0x100)
    mu.reg_write(UC_ARM_REG_LR, END | 1)
    for r, v in ((UC_ARM_REG_R0, ctx), (UC_ARM_REG_R1, pay), (UC_ARM_REG_R2, rt), (UC_ARM_REG_R3, 0)):
        mu.reg_write(r, v)
    mu.emu_start(0x80bf5eec | 1, END, count=5_000_000)
    for l in log: print('  ', l)
    np_ = [r32(pay + 4 * i) for i in range(len(pw) - 2)]
    print('payload words changed by the kernel:')
    for i, (a, b) in enumerate(zip(pw[2:], np_)):
        if a != b: print('  w%d: %08x -> %08x' % (i + 2, a, b))
    b = bufs[-1]
    blk = [r32(b['cpu'] + 4 * i) for i in range(0x160 // 4)]
    print('3D block (gpu %#x):' % b['gpu'])
    for i in range(0, len(blk), 8):
        print('  +%03x: %s' % (i * 4, ' '.join('%08x' % x for x in blk[i:i + 8])))
    open('rt_%dx%d_payload.bin' % (W, H), 'wb').write(struct.pack('<%dI' % len(np_), *np_))
    open('rt_%dx%d_blk3d.bin' % (W, H), 'wb').write(struct.pack('<%dI' % len(blk), *blk))
