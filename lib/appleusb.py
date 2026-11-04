#!/usr/bin/env python3
"""appleusb.py -- talk to an Apple device in DFU or Recovery mode over USB.

What the boot chain needs from irecovery and primepwn, in one file on pyusb
(libusb), so flashing needs neither Legacy iOS Kit nor libirecovery:

    appleusb.py query                 the device's serial-string fields and its
                                      mode, as irecovery -q prints them
    appleusb.py mode                  DFU, Recovery, WTF, or none
    appleusb.py wait MODE [SECONDS]   until the device is in MODE (default 20 s)
    appleusb.py send FILE             upload FILE the way `irecovery -f` does:
                                      DFU -- control transfers with the DFU
                                      suffix and CRC, then the finish and a USB
                                      reset; Recovery -- bulk to endpoint 4
    appleusb.py boot-pwned FILE       run FILE (an iBSS) on a device that is
                                      in pwned DFU already (checkm8 from a Pi
                                      Pico, ipwndfu -l / primepwn <image>)
    appleusb.py cmd COMMAND           an iBoot command (Recovery), e.g. "go"

Exit status: 0 done, 1 failed, 3 no device, 4 no permission for the device.

The sequences follow libirecovery's irecv_send_buffer (DFU and Recovery) and
the image-boot request ipwndfu's checkm8 payload answers; the code is ours.
"""
import os
import re
import sys
import time
import zlib

# pyusb: the system's, or the copy `setup` puts in build/pylib (no pip)
sys.path.append(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'build', 'pylib'))
try:
    import usb.core
    import usb.util
    import usb.backend.libusb1
except ImportError:                                    # pragma: no cover
    sys.stderr.write("error: no pyusb -- ./cascadia a5 setup puts it in build/pylib\n")
    sys.exit(1)

# libusb itself.  ctypes' search does not look in Homebrew's lib directory,
# so a python that is not Homebrew's own finds no backend at all on a Mac
# whose libusb is installed; try the places it lives as well.
LIBUSB_PATHS = ('/opt/homebrew/lib/libusb-1.0.dylib', '/usr/local/lib/libusb-1.0.dylib')
BACKEND = None


def backend():
    global BACKEND
    if BACKEND is None:
        BACKEND = usb.backend.libusb1.get_backend()
        for p in LIBUSB_PATHS:
            if BACKEND is not None:
                break
            if os.path.exists(p):
                BACKEND = usb.backend.libusb1.get_backend(find_library=lambda _n, p=p: p)
        if BACKEND is None:
            raise Fail("no libusb -- ./cascadia a5 setup installs it (macOS: brew install libusb)")
    return BACKEND

APPLE = 0x05AC
DFU, WTF = 0x1227, 0x1222
RECOVERY = (0x1280, 0x1281, 0x1282, 0x1283)
MODES = {DFU: 'DFU', WTF: 'WTF'}
MODES.update({p: 'Recovery' for p in RECOVERY})

TIMEOUT = 10000          # ms, as libirecovery's USB_TIMEOUT
DFU_PACKET = 0x800
RECOVERY_PACKET = 0x8000
# The DFU suffix libirecovery appends to the last packet: bcdDevice,
# idProduct, idVendor 0xffff, bcdDFU 0x0100, "UFD", length 16 -- then the
# CRC register over the image and these 12 bytes, little-endian.
DFU_SUFFIX = bytes([0xff, 0xff, 0xff, 0xff, 0xac, 0x05, 0x00, 0x01,
                    0x55, 0x46, 0x44, 0x10])

# DFU class requests (bmRequestType 0x21 out / 0xA1 in)
DNLOAD, UPLOAD, GETSTATUS, CLRSTATUS, GETSTATE, ABORT = 1, 2, 3, 4, 5, 6
ST_DNLOAD_IDLE, ST_IDLE, ST_ERROR = 5, 2, 10


class Fail(Exception):
    def __init__(self, msg, code=1):
        super().__init__(msg)
        self.code = code


def say(msg):
    sys.stderr.write(msg + '\n')


def crc_register(data):
    """libirecovery's running CRC: the reflected CRC-32 register started at
    0xffffffff and never inverted at the end, i.e. zlib's value inverted."""
    return zlib.crc32(data) ^ 0xFFFFFFFF


def permission_hint():
    if sys.platform.startswith('linux'):
        return ("no permission for the device.  Give your user Apple's USB devices once:\n"
                "  echo 'SUBSYSTEM==\"usb\", ATTR{idVendor}==\"05ac\", MODE=\"0666\"' | "
                "sudo tee /etc/udev/rules.d/39-cascadia-apple.rules\n"
                "  sudo udevadm control --reload && sudo udevadm trigger\n"
                "then unplug and replug the device (or run this once with sudo)")
    return "no permission for the device (another program may hold it: close Finder/iTunes, irecovery)"


def find():
    for d in usb.core.find(find_all=True, idVendor=APPLE, backend=backend()):
        if d.idProduct in MODES:
            return d
    return None


def serial_of(dev):
    try:
        return usb.util.get_string(dev, dev.iSerialNumber) or ''
    except (usb.core.USBError, ValueError) as e:
        if getattr(e, 'errno', None) in (13, 1) or 'Access denied' in str(e):
            raise Fail(permission_hint(), 4)
        return ''


def fields(serial):
    return dict(re.findall(r'([A-Z]{4}):(\[[^\]]*\]|\S+)', serial))


class Device:
    def __init__(self, dev):
        self.dev = dev
        self.pid = dev.idProduct
        self.mode = MODES[self.pid]
        self.serial = serial_of(dev)
        self.info = fields(self.serial)

    @classmethod
    def open(cls, attempts=5):
        for i in range(attempts):
            d = find()
            if d is not None:
                try:
                    return cls(d).claim()
                except usb.core.USBError as e:
                    if e.errno in (13, 1) or 'Access denied' in str(e):
                        raise Fail(permission_hint(), 4)
            time.sleep(1)
        raise Fail("no Apple device in DFU or Recovery mode on USB", 3)

    def claim(self):
        d = self.dev
        try:
            cfg = d.get_active_configuration()
            if cfg.bConfigurationValue != 1:
                d.set_configuration(1)
        except usb.core.USBError:
            d.set_configuration(1)
        intfs = [0]
        if self.mode == 'Recovery' and self.pid > 0x1281:
            intfs.append(1)
        for i in intfs:
            try:
                if d.is_kernel_driver_active(i):
                    d.detach_kernel_driver(i)
            except (NotImplementedError, usb.core.USBError):
                pass
            usb.util.claim_interface(d, i)
        if 1 in intfs:
            d.set_interface_altsetting(interface=1, alternate_setting=1)
        return self

    def close(self):
        try:
            usb.util.dispose_resources(self.dev)
        except usb.core.USBError:
            pass

    # -- plumbing ------------------------------------------------------------
    def ctrl(self, rtype, req, value=0, index=0, data=None, timeout=TIMEOUT):
        return self.dev.ctrl_transfer(rtype, req, value, index, data, timeout)

    def status(self, timeout=TIMEOUT):
        b = self.ctrl(0xA1, GETSTATUS, 0, 0, 6, timeout)
        if len(b) != 6:
            raise Fail("DFU GETSTATUS returned %d bytes" % len(b))
        return b[4]

    def reset(self):
        try:
            self.dev.reset()
        except usb.core.USBError:
            pass                    # it re-enumerates: gone is the point

    # -- uploads -------------------------------------------------------------
    def send(self, data, progress=True):
        if self.mode == 'Recovery':
            return self._send_recovery(data, progress)
        return self._send_dfu(data, progress)

    def _send_dfu(self, data, progress):
        state = self.ctrl(0xA1, GETSTATE, 0, 0, 1)
        state = state[0] if len(state) == 1 else -1
        if state == ST_ERROR:
            self.ctrl(0x21, CLRSTATUS)
            raise Fail("the device was in DFU error state (cleared); try again")
        if state != ST_IDLE:
            self.ctrl(0x21, ABORT)
            raise Fail("unexpected DFU state %d (aborted); try again" % state)

        n = len(data)
        packets = (n + DFU_PACKET - 1) // DFU_PACKET or 1
        crc_input = data + DFU_SUFFIX
        for i in range(packets):
            chunk = data[i * DFU_PACKET:(i + 1) * DFU_PACKET]
            if i + 1 == packets:
                if len(chunk) + 16 > DFU_PACKET:
                    # no room for the suffix: the data alone, then the suffix
                    # in a packet of its own under the same block number
                    if self.ctrl(0x21, DNLOAD, i, 0, chunk) != len(chunk):
                        raise Fail("upload failed in block %d" % i)
                    chunk = b''
                h = crc_register(crc_input)
                chunk = chunk + DFU_SUFFIX + h.to_bytes(4, 'little')
            if self.ctrl(0x21, DNLOAD, i, 0, chunk) != len(chunk):
                raise Fail("upload failed in block %d" % i)
            st = self.status()
            tries = 0
            while st != ST_DNLOAD_IDLE and tries < 20:
                time.sleep(1)
                st = self.status()
                tries += 1
            if st != ST_DNLOAD_IDLE:
                raise Fail("the device did not take block %d (DFU status %d)" % (i, st))
            if progress:
                sys.stderr.write("\r    %3d%%" % (100 * min(n, (i + 1) * DFU_PACKET) // max(n, 1)))
        if progress:
            sys.stderr.write("\n")
        # the finish: a zero-length block, two status reads, a USB reset
        self.ctrl(0x21, DNLOAD, packets, 0, None)
        for _ in range(2):
            self.status()
        self.reset()

    def _send_recovery(self, data, progress):
        self.ctrl(0x41, 0, 0, 0, None)
        n = len(data)
        for off in range(0, n, RECOVERY_PACKET):
            chunk = data[off:off + RECOVERY_PACKET]
            if self.dev.write(0x04, chunk, TIMEOUT) != len(chunk):
                raise Fail("upload failed at byte %d" % off)
            if progress:
                sys.stderr.write("\r    %3d%%" % (100 * (off + len(chunk)) // max(n, 1)))
        if progress:
            sys.stderr.write("\n")
        if n % 512 == 0:
            self.dev.write(0x04, b'', TIMEOUT)          # a zero-length packet ends it

    def boot_pwned(self, data):
        """Run an image on a device a checkm8 payload has pwned: an empty
        block, the image, then the payload's boot request (UPLOAD with
        wValue 0xffff), which jumps to it."""
        if self.mode != 'DFU':
            raise Fail("the device is in %s mode, not DFU" % self.mode)
        if 'PWND' not in self.info:
            raise Fail("the device is in DFU but not pwned (no PWND in '%s').\n"
                       "Run checkm8 first: plug it into the Pi Pico, then back into this computer." % self.serial)
        self.ctrl(0x21, DNLOAD, 0, 0, bytes(16))
        self.ctrl(0x21, DNLOAD, 0, 0, None, 100)
        for _ in range(2):
            try:
                self.ctrl(0xA1, GETSTATUS, 0, 0, 6, 100)
            except usb.core.USBError:
                pass
        n = len(data)
        for off in range(0, n, DFU_PACKET):
            chunk = data[off:off + DFU_PACKET]
            if self.ctrl(0x21, DNLOAD, 0, 0, chunk, 5000) != len(chunk):
                raise Fail("upload failed at byte %d" % off)
            sys.stderr.write("\r    %3d%%" % (100 * (off + len(chunk)) // max(n, 1)))
        sys.stderr.write("\n")
        try:
            self.ctrl(0xA1, UPLOAD, 0xFFFF, 0, 0, 5000)
        except usb.core.USBError:
            pass                    # the device is off running the image

    def command(self, cmd):
        if self.mode != 'Recovery':
            raise Fail("iBoot commands need Recovery mode (the device is in %s)" % self.mode)
        b = cmd.encode() + b'\0'
        if len(b) > 0x100:
            raise Fail("command too long")
        try:
            self.ctrl(0x40, 0, 0, 0, b)
        except usb.core.USBError:
            pass                    # "go", "bootx" and "reboot" do not answer


def current_mode():
    d = find()
    return MODES[d.idProduct] if d is not None else 'none'


def main(argv):
    if not argv:
        sys.stderr.write(__doc__)
        return 2
    op, args = argv[0], argv[1:]
    if op == 'mode':
        print(current_mode())
        return 0
    if op == 'wait':
        if not args:
            raise Fail("usage: appleusb.py wait DFU|Recovery [SECONDS]")
        want, secs = args[0], float(args[1]) if len(args) > 1 else 20
        end = time.time() + secs
        while time.time() < end:
            if current_mode().lower() == want.lower():
                return 0
            time.sleep(0.5)
        raise Fail("no device in %s mode within %g s (now: %s)" % (want, secs, current_mode()), 3)
    dev = Device.open()
    try:
        if op == 'query':
            for k, v in dev.info.items():
                v = v.strip('[]')
                if k in ('CPID', 'CPRV', 'CPFM', 'SCEP', 'BDID', 'IBFL', 'ECID') and not v.startswith('0x'):
                    v = '0x' + v
                print("%s: %s" % (k, v))
            print("MODE: %s" % dev.mode)
        elif op in ('send', 'boot-pwned'):
            if not args:
                raise Fail("usage: appleusb.py %s FILE" % op)
            with open(args[0], 'rb') as f:
                data = f.read()
            say("==> %s %s (%d bytes, device in %s)" % (op, os.path.basename(args[0]), len(data), dev.mode))
            if op == 'send':
                dev.send(data)
            else:
                dev.boot_pwned(data)
        elif op == 'cmd':
            if not args:
                raise Fail("usage: appleusb.py cmd COMMAND")
            dev.command(' '.join(args))
        else:
            sys.stderr.write(__doc__)
            return 2
    finally:
        dev.close()
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main(sys.argv[1:]))
    except Fail as e:
        say("error: %s" % e)
        sys.exit(e.code)
    except usb.core.USBError as e:
        if e.errno in (13, 1) or 'Access denied' in str(e):
            say("error: " + permission_hint())
            sys.exit(4)
        say("error: USB: %s" % e)
        sys.exit(1)
