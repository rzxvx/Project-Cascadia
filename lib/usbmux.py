#!/usr/bin/env python3
"""usbmux.py -- reach a TCP port on an iOS device over USB, through usbmuxd.

    usbmux.py list                 the devices usbmuxd sees (id, UDID, model)
    usbmux.py connect PORT [UDID]  pipe stdin/stdout to PORT on the device
                                   (the first one, or UDID): an ssh ProxyCommand

    ssh -o ProxyCommand='python3 lib/usbmux.py connect 22' root@ios

What iproxy does, without a local port or libusbmuxd: usbmuxd itself is
part of macOS, and on Linux the usbmuxd package (it starts on its own when
an iOS device is plugged in).  USBMUXD_SOCKET_ADDRESS (UNIX:/path or
host:port) points elsewhere, as for libusbmuxd.
"""
import os
import plistlib
import select
import socket
import struct
import sys

PLIST = 8               # message type: plist, protocol version 1


class Fail(Exception):
    pass


def mux_socket():
    addr = os.environ.get('USBMUXD_SOCKET_ADDRESS', '')
    if addr and not addr.startswith('UNIX:'):
        host, _, port = addr.rpartition(':')
        return socket.create_connection((host, int(port)))
    path = addr[5:] if addr else next((p for p in ('/var/run/usbmuxd', '/run/usbmuxd')
                                       if os.path.exists(p)), '/var/run/usbmuxd')
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        s.connect(path)
    except OSError as e:
        hint = ("usbmuxd is not running: install the usbmuxd package; it starts when an iOS "
                "device is plugged in (or: sudo systemctl start usbmuxd)"
                if sys.platform.startswith('linux') else "usbmuxd is not reachable")
        raise Fail("%s (%s: %s)" % (hint, path, e.strerror))
    return s


def recv_exact(s, n):
    b = b''
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c:
            raise Fail("usbmuxd closed the connection")
        b += c
    return b


def request(s, msg, tag=1):
    msg = dict(msg, ClientVersionString='cascadia', ProgName='cascadia')
    body = plistlib.dumps(msg)
    s.sendall(struct.pack('<IIII', 16 + len(body), 1, PLIST, tag) + body)
    length, _version, _mtype, _tag = struct.unpack('<IIII', recv_exact(s, 16))
    return plistlib.loads(recv_exact(s, length - 16))


def devices():
    s = mux_socket()
    try:
        r = request(s, {'MessageType': 'ListDevices'})
    finally:
        s.close()
    out = []
    for d in r.get('DeviceList', []):
        p = d.get('Properties', {})
        if p.get('ConnectionType', 'USB') != 'USB':
            continue
        out.append((d.get('DeviceID'), p.get('SerialNumber', ''), p.get('ProductID', 0)))
    return out


def connect(port, udid=None):
    devs = devices()
    if udid:
        devs = [d for d in devs if d[1] == udid]
    if not devs:
        raise Fail("no iOS device on USB%s -- booted into iOS, unlocked, the cable in?"
                   % (" with UDID " + udid if udid else ""))
    s = mux_socket()
    r = request(s, {'MessageType': 'Connect', 'DeviceID': devs[0][0],
                    'PortNumber': socket.htons(port)})
    if r.get('Number', 1) != 0:
        s.close()
        raise Fail("the device refused port %d (usbmuxd result %s) -- is its sshd running?"
                   % (port, r.get('Number')))
    return s


def pipe(s):
    fin, fout = sys.stdin.fileno(), sys.stdout.fileno()
    open_in = True
    while True:
        r, _, _ = select.select([s] + ([fin] if open_in else []), [], [])
        if s in r:
            b = s.recv(65536)
            if not b:
                return
            os.write(fout, b)
        if open_in and fin in r:
            b = os.read(fin, 65536)
            if not b:
                open_in = False
                try:
                    s.shutdown(socket.SHUT_WR)
                except OSError:
                    pass
            else:
                s.sendall(b)


def main(argv):
    if argv[:1] == ['list']:
        for did, udid, pid in devices():
            print("%s\t%s\t0x%04x" % (did, udid, pid))
        return 0
    if argv[:1] == ['connect'] and len(argv) >= 2:
        s = connect(int(argv[1]), argv[2] if len(argv) > 2 else None)
        try:
            pipe(s)
        finally:
            s.close()
        return 0
    sys.stderr.write(__doc__)
    return 2


if __name__ == '__main__':
    try:
        sys.exit(main(sys.argv[1:]))
    except Fail as e:
        sys.stderr.write("error: %s\n" % e)
        sys.exit(1)
