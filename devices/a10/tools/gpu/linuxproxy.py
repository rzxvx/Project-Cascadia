# SPDX-License-Identifier: MIT
"""The GPU tools' Linux backend (devices/a10/docs/gpu.md, "GPU work under
Linux"): the same p / u / iface objects the tools use with m1n1's proxy, but
the accesses run on the iPad's Linux through tools/gpu/mmiod (over ssh,
/dev/mem). Under m1n1 the Rogue core's registers hang the bus; under Linux
they answer.

Environment:
  GPU_HOST  ssh target (default root@172.16.43.1)
  GPU_RAM   BASE:SIZE of the RAM Linux does not use (boot with mem=..., see
            docs/gpu.md); default 0x8b8000000:0x2000000 (mem=2944M)
  GPU_ADT   the iPad's ADT (default build/adt/j171.bin)

Memory from GPU_RAM is mapped Device / uncached by mmiod: no cache
maintenance is needed, dc_* are no-ops.
"""
import os
import struct
import subprocess
import time

HERE = os.path.dirname(os.path.abspath(__file__))
A10 = os.path.join(HERE, "../..")

from m1n1.adt import load_adt  # noqa: E402


class _Daemon:
    def __init__(self, host):
        subprocess.run(["scp", "-q", "-o", "BatchMode=yes", os.path.join(A10, "build/bin/mmiod"),
                        f"{host}:/run/mmiod"], check=True)
        self.proc = subprocess.Popen(["ssh", "-o", "BatchMode=yes", host, "/run/mmiod"],
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, bufsize=0)

    def req(self, op, addr=0, arg=0, data=b"", reply=0):
        self.proc.stdin.write(struct.pack("<BQQ", op, addr, arg) + data)
        out = b""
        while len(out) < reply:
            chunk = self.proc.stdout.read(reply - len(out))
            if not chunk:
                raise IOError("mmiod went away (the iPad hung or the link dropped)")
            out += chunk
        return out


class GUARD:
    OFF = 0
    SKIP = 1
    SILENT = 0x100


class Proxy:
    def __init__(self, d, u):
        self.d = d
        self.u = u

    def read32(self, a):
        return struct.unpack("<I", self.d.req(1, a, reply=4))[0]

    def write32(self, a, v):
        self.d.req(2, a, v & 0xffffffff, reply=1)

    def read64(self, a):
        return struct.unpack("<Q", self.d.req(3, a, reply=8))[0]

    def write64(self, a, v):
        self.d.req(4, a, v & 0xffffffffffffffff, reply=1)

    # m1n1 API the tools call; Linux has no exception guard, caches are off
    def set_exc_guard(self, mode):
        pass

    def get_exc_count(self):
        return 0

    def dc_cvac(self, a, size):
        pass

    def dc_civac(self, a, size):
        pass

    def pmgr_adt_power_enable(self, path):
        self.u.pmgr_enable(path)

    def memalign(self, align, size):
        return self.u.memalign(align, size)


class Iface:
    def __init__(self, d):
        self.d = d

    def readmem(self, a, size):
        lo, hi = a & ~3, (a + size + 3) & ~3
        data = self.d.req(5, lo, hi - lo, reply=hi - lo)
        return data[a - lo:a - lo + size]

    def writemem(self, a, data, progress=False):
        lo, hi = a & ~3, (a + len(data) + 3) & ~3
        if lo != a or hi != a + len(data):
            buf = bytearray(self.readmem(lo, hi - lo))
            buf[a - lo:a - lo + len(data)] = data
            data = bytes(buf)
        self.d.req(6, lo, len(data), data, reply=1)


class Heap:
    def __init__(self, base, size):
        self.base, self.end, self.cur = base, base + size, base

    def memalign(self, align, size):
        a = (self.cur + align - 1) & ~(align - 1)
        if a + size > self.end:
            raise MemoryError(f"GPU_RAM exhausted ({size:#x} more at {a:#x})")
        self.cur = a + size
        return a

    def malloc(self, size):
        return self.memalign(0x40, size)


class Utils:
    def __init__(self, d, adt_path, ram):
        self.d = d
        self.adt = load_adt(open(adt_path, "rb").read())
        self.iface = Iface(d)
        self.proxy = self.p = Proxy(d, self)
        self.heap = Heap(*ram)

    def memalign(self, align, size):
        return self.heap.memalign(align, size)

    def mrs(self, reg, **kw):
        if reg in ("CNTPCT_EL0", "CNTVCT_EL0"):
            return struct.unpack("<Q", self.d.req(7, reply=8))[0]
        raise NotImplementedError(reg)

    # RegMap backend
    def read(self, addr, width):
        return {32: self.p.read32, 64: self.p.read64}[width](addr)

    def write(self, addr, data, width):
        {32: self.p.write32, 64: self.p.write64}[width](addr, data)

    # m1n1's pmgr_adt_power_enable, in Python: the node's clock-gates (or
    # power-gates), parents first, virtual (no_ps) devices only recursed
    def pmgr_enable(self, path):
        node = self.adt[path]
        ids = getattr(node, "clock_gates", None) or getattr(node, "power_gates", [])
        for i in ids:
            self._enable(i)

    def _enable(self, dev_id):
        pm = self.adt["/arm-io/pmgr"]
        dev = next((d for d in pm.devices if self.adt.pmgr_dev_get_id(d) == dev_id), None)
        if dev is None:
            return
        for par in self.adt.pmgr_dev_get_parents(dev):
            if par:
                self._enable(par)
        if dev.flags.no_ps:
            return
        a = self.adt.pmgr_dev_get_addr(dev)
        v = self.p.read32(a)
        if v & 0xf != 0xf:
            self.p.write32(a, (v & ~0xf) | 0xf)
        t = time.time()
        while (self.p.read32(a) >> 4) & 0xf != 0xf:
            if time.time() - t > 0.5:
                raise TimeoutError(f"PMGR {dev.name} did not reach state 0xf")


def setup():
    host = os.environ.get("GPU_HOST", "root@172.16.43.1")
    base, size = (int(x, 0) for x in os.environ.get("GPU_RAM", "0x8b8000000:0x2000000").split(":"))
    adt_path = os.environ.get("GPU_ADT", os.path.join(A10, "build/adt/j171.bin"))
    d = _Daemon(host)
    d.req(8, reply=1)
    u = Utils(d, adt_path, (base, size))
    return u.p, u, u.iface
