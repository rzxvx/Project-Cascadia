#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""G2 (devices/a10/docs/gpu.md): boot Apple's GFX firmware on the gfx-kf
coprocessor from the Mac through m1n1's proxy, up to the RTKit handshake.

    bash tools/gpu/gfx-fw.sh           # build/firmware/apple/gfx-t8010-22H355.bin
    ./cascadia a10 boot proxy
    M1N1DEVICE=/dev/cu.usbmodemXXXX1 build/proxyvenv/bin/python tools/gpu/gfx-boot.py [SECONDS]

Run from devices/a10. Every step is what the 22H355 kernelcache does
(docs/gpu.md, "G2"):

- AGXFirmwareKextG9PRTBuddy (core config type 3): the image is 0xa4000 bytes
  (+ a 0x60-byte trailer we leave out), the runtime region 0xd8000 bytes
  (= the image's RTSZ), boot-config offset 0: the image goes in unpatched.
- AppleA7IOP-AKFWrap-v2 (AppleA7IOPV2 / V2GFX):
  * mailbox init: IOP +0x4000 = 0x1111, IOP +0xc00 = 0x1111;
  * timebase (gfx-kf has force-timebase-sync): IOP +0x80c &= ~2, IOP
    +0xc030 / +0xc038 = the AP's counter lo / hi, IOP +0x80c |= 2;
  * window, GFX copy: Fender +0x2014 / +0x2018 = IOP address (0), +0x201c =
    length & ~0xfff, +0x2020 = 0, +0x200c / +0x2010 = physical address;
    V2 copy: IOP +0x18 / +0x20 = IOP address, +0x28 = length, +0x30 = 0,
    +0x08 / +0x10 = physical address, +0x38 = 1;
  * run: Fender +0x2048 bit 0 (V2GFX's override of the V2 run bit);
  * messages: send IOP +0x4010, receive IOP +0x4038 (64-bit), inbox status
    +0x4008 (bit 17 empty, 16 full, 18-19 error), outbox status +0x4020.
- The RTKit management protocol (HELLO, EPMAP, IOP / AP power) is m1n1's
  fw/akf (StandardAKF); endpoints 0x20 / 0x21 (the AGX init / doorbell
  endpoints) are started as silent placeholders: nothing is sent to them.

Writes crash.bin (if the firmware crashes) into build/gpu/.
"""
import os
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
A10 = os.path.join(HERE, "../..")
PROXY = os.path.join(A10, "build/m1n1/proxyclient")
sys.path.insert(0, os.environ.get("M1N1_PROXYCLIENT", PROXY))

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gpusetup import p, u, iface, GUARD  # noqa: E402  (m1n1 proxy, or GPU_BACKEND=linux)
from m1n1.fw.akf import StandardAKF, AKFBaseEndpoint  # noqa: E402

FW = os.path.join(A10, "build/firmware/apple/gfx-t8010-22H355.bin")
FW_SIZE = 0xa4000
RUNTIME = 0xd8000
os.makedirs(os.path.join(A10, "build/gpu"), exist_ok=True)
os.chdir(os.path.join(A10, "build/gpu"))

adt = u.adt
FENDER = adt["/arm-io/sgx"].get_reg(1)[0]
IOP = adt["/arm-io/gfx-kf"].get_reg(0)[0]
pm = adt["/arm-io/pmgr"]


def out(s):
    print(f"[gfx-boot] {s}", flush=True)


class AGXEndpoint(AKFBaseEndpoint):
    """Placeholder for the AGX endpoints: logs whatever arrives."""
    SHORT = "agx"

    def handle_msg(self, msg):
        out(f"ep {self.epnum:#x} message {msg:#018x}")
        return True


class GfxKF(StandardAKF):
    ENDPOINTS = {0x20: AGXEndpoint, 0x21: AGXEndpoint}

    def __init__(self, u, buf):
        self.buf = buf
        super().__init__(u, "/arm-io/gfx-kf")
        self.verbose = 3

    def adt_configure(self):
        # mailbox
        p.write32(IOP + 0x4000, 0x1111)
        p.write32(IOP + 0xc00, 0x1111)
        # then unmask, as the kext's mask setter (AKFWrap 0x...c15e7c) does with
        # its "clear" registers +0x4004 / +0xc04: without it the firmware
        # answers the boot handshake (polled) and then never sees a message.
        for v in (0x1, 0x1000):
            p.write32(IOP + 0x4004, v)
            p.write32(IOP + 0xc04, v)
        # timebase
        v = p.read32(IOP + 0x80c)
        p.write32(IOP + 0x80c, v & ~2)
        t = u.mrs("CNTPCT_EL0")
        p.write32(IOP + 0xc030, t & 0xffffffff)
        p.write32(IOP + 0xc038, t >> 32)
        p.write32(IOP + 0x80c, v | 2)
        # window: GFX copy in Fender, then the V2 copy in the IOP block
        p.write32(FENDER + 0x2014, 0)
        p.write32(FENDER + 0x2018, 0)
        p.write32(FENDER + 0x201c, RUNTIME & ~0xfff)
        p.write32(FENDER + 0x2020, 0)
        p.write32(FENDER + 0x200c, self.buf & 0xffffffff)
        p.write32(FENDER + 0x2010, self.buf >> 32)
        p.write32(IOP + 0x18, 0)
        p.write32(IOP + 0x20, 0)
        p.write32(IOP + 0x28, RUNTIME & ~0xfff)
        p.write32(IOP + 0x30, 0)
        p.write32(IOP + 0x08, self.buf & 0xffffffff)
        p.write32(IOP + 0x10, self.buf >> 32)
        p.write32(IOP + 0x38, 1)
        self.configured = True

    def boot(self):
        if not self.configured:
            self.adt_configure()
        p.write32(FENDER + 0x2048, (p.read32(FENDER + 0x2048) & ~1) | 1)

    def shutdown(self):
        p.write32(FENDER + 0x2048, p.read32(FENDER + 0x2048) & ~1)

    def is_running(self):
        return p.read32(FENDER + 0x2048) & 1


def regs(tag):
    f = {o: p.read32(FENDER + o) for o in (0x10, 0x14, 0x1000, 0x200c, 0x2010, 0x201c, 0x2048, 0x2058)}
    i = {o: p.read32(IOP + o) for o in (0x8, 0x10, 0x28, 0x38, 0x48, 0x80c, 0x4008, 0x4020)}
    out(f"{tag}: Fender " + " ".join(f"+{o:#x}={v:#x}" for o, v in f.items()))
    out(f"{tag}: IOP " + " ".join(f"+{o:#x}={v:#x}" for o, v in i.items()))


def gfx_power_on():
    gfx = next(d for d in pm.devices if adt.pmgr_dev_get_id(d) == 124)
    ps = adt.pmgr_dev_get_addr(gfx)
    if "--cold" in sys.argv and (p.read32(ps) >> 4) & 0xf:
        # a cold GPU: the Rogue core keeps sticky state (MMU fault status,
        # context-store status) across firmware reloads while GFX stays on
        p.write32(FENDER + 0x2048, p.read32(FENDER + 0x2048) & ~1)
        time.sleep(0.01)
        p.write32(ps, p.read32(ps) & ~0xf)
        t = time.time()
        while (p.read32(ps) >> 4) & 0xf:
            if time.time() - t > 0.5:
                sys.exit(f"[gfx-boot] GFX did not power off: {p.read32(ps):#010x}")
        out(f"--cold: GFX off ({p.read32(ps):#010x})")
    p.pmgr_adt_power_enable("/arm-io/sgx")
    p.pmgr_adt_power_enable("/arm-io/gfx-kf")
    time.sleep(0.01)
    v = p.read32(ps)
    out(f"PMGR GFX = {v:#010x}")
    if (v >> 4) & 0xf != 0xf:
        sys.exit("[gfx-boot] GFX not on; stop")


def boot_firmware(timeout=5.0, buf=None, endpoints=None):
    """Load the image into a fresh (or the given) runtime region and boot
    RTKit. Returns (kf, buf). A firmware already running is stopped first."""
    img = open(FW, "rb").read()
    assert len(img) == FW_SIZE, f"{FW}: {len(img):#x} bytes, not {FW_SIZE:#x} (run tools/gpu/gfx-fw.sh)"
    if p.read32(FENDER + 0x2048) & 1:
        out("firmware running: clearing the run bit first")
        p.write32(FENDER + 0x2048, p.read32(FENDER + 0x2048) & ~1)
        time.sleep(0.01)
    if buf is None:
        buf = u.heap.memalign(0x10000, RUNTIME)
    iface.writemem(buf, img + bytes(RUNTIME - FW_SIZE))
    p.dc_cvac(buf, RUNTIME)
    out(f"runtime region {buf:#x} +{RUNTIME:#x}, image {FW_SIZE:#x} bytes, first word {p.read32(buf):#010x}")
    regs("before")
    if endpoints:
        GfxKF.ENDPOINTS = {**GfxKF.ENDPOINTS, **endpoints}
    kf = GfxKF(u, buf)
    kf.adt_configure()
    regs("configured")
    # Not StandardAKF.start(): it calls the base AKF.boot(), which writes its
    # "CPU_CONTROL" at IOP +0x28 -- the V2 window's length register per the
    # kext. Our boot() sets Fender +0x2048 instead; then m1n1's mgmt sequence.
    kf.boot()
    out("run bit set; IOP power 0x220, waiting for HELLO / EPMAP / power acks ...")
    kf.mgmt.start()
    t0 = time.time()
    kf.mgmt.wait_boot(timeout)
    out(f"RTKit up after {time.time() - t0:.2f} s; endpoints {[hex(e) for e in kf.eps]}")
    regs("after")
    out(f"ping: {'pong' if ping(kf) else 'NO REPLY'}")
    return kf, buf


def ping(kf, timeout=1.0):
    """RTKit management ping; True on pong (other messages are handled)."""
    kf.mgmt.pongs = getattr(kf.mgmt, "pongs", 0)
    orig = kf.mgmt.msghandler.get(4)

    def pong(msg):
        kf.mgmt.pongs += 1
        return True
    kf.mgmt.msghandler[4] = pong
    n = kf.mgmt.pongs
    kf.send(3 << 52)
    t = time.time()
    while time.time() - t < timeout and kf.mgmt.pongs == n:
        kf.work()
    kf.mgmt.msghandler[4] = orig
    return kf.mgmt.pongs != n


if __name__ == "__main__":
    SECONDS = float(sys.argv[1]) if len(sys.argv) > 1 else 5.0
    gfx_power_on()
    try:
        kf, buf = boot_firmware(SECONDS)
    except Exception as e:
        sys.exit(f"[gfx-boot] boot did not complete: {e!r}")
    out(f"draining messages for {SECONDS} s")
    try:
        kf.work_for(SECONDS)
    except Exception as e:
        out(f"stopped on {e!r}")
    regs("end")
    out("end")
