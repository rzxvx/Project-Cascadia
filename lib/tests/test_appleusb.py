#!/usr/bin/env python3
"""The USB sequences lib/appleusb.py sends, against a recording fake device.

    python3 lib/tests/test_appleusb.py

No device needed.  The CRC is checked against a table-driven CRC-32 step,
written out the way libirecovery's crc32_step is, so a slip in the zlib
shortcut shows up here and not as a device that ignores its iBEC.
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import appleusb  # noqa: E402

TABLE = []
for n in range(256):
    c = n
    for _ in range(8):
        c = (c >> 1) ^ 0xEDB88320 if c & 1 else c >> 1
    TABLE.append(c)


def crc_steps(data, h=0xFFFFFFFF):
    for b in data:
        h = TABLE[(h & 0xFF) ^ b] ^ (h >> 8)
    return h


class FakeDev:
    def __init__(self, pid, state=2):
        self.idProduct = pid
        self.log = []
        self.state = state

    def ctrl_transfer(self, rtype, req, value, index, data, timeout):
        self.log.append(('ctrl', rtype, req, value, index,
                         bytes(data) if isinstance(data, (bytes, bytearray)) else data))
        if rtype == 0xA1 and req == appleusb.GETSTATE:
            return bytes([self.state])
        if rtype == 0xA1 and req == appleusb.GETSTATUS:
            return bytes([0, 0, 0, 0, appleusb.ST_DNLOAD_IDLE, 0])
        if isinstance(data, (bytes, bytearray)):
            return len(data)
        return 0 if data is None else b''

    def write(self, ep, data, timeout):
        self.log.append(('bulk', ep, bytes(data)))
        return len(data)

    def reset(self):
        self.log.append(('reset',))


def device(pid, serial='CPID:8942 BDID:00 SRTG:[iBoot-1145.3] PWND:[checkm8]', state=2):
    d = appleusb.Device.__new__(appleusb.Device)
    d.dev = FakeDev(pid, state)
    d.pid = pid
    d.mode = appleusb.MODES[pid]
    d.serial = serial
    d.info = appleusb.fields(serial)
    return d


def dnloads(log):
    return [e for e in log if e[0] == 'ctrl' and e[1] == 0x21 and e[2] == appleusb.DNLOAD]


class DFU(unittest.TestCase):
    def check(self, n):
        data = bytes((i * 7 + 3) & 0xFF for i in range(n))
        d = device(appleusb.DFU)
        d.send(data, progress=False)
        log = d.dev.log
        self.assertEqual(log[0][:3], ('ctrl', 0xA1, appleusb.GETSTATE))
        blocks = dnloads(log)
        finish = blocks[-1]
        body = blocks[:-1]
        sent = b''.join(b[5] for b in body)
        # the image, then the 12-byte suffix and the CRC register over both
        self.assertEqual(sent[:n], data)
        self.assertEqual(sent[n:n + 12], appleusb.DFU_SUFFIX)
        h = crc_steps(data + appleusb.DFU_SUFFIX)
        self.assertEqual(int.from_bytes(sent[n + 12:], 'little'), h)
        self.assertEqual(len(sent), n + 16)
        # block numbers count up; a suffix that does not fit reuses the last
        packets = (n + 0x7FF) // 0x800
        self.assertEqual(finish[3], packets)
        self.assertIsNone(finish[5])
        self.assertEqual(body[0][3], 0)
        self.assertEqual(body[-1][3], packets - 1)
        self.assertTrue(all(len(b[5]) <= 0x800 for b in body))
        # finish: zero-length DNLOAD, two GETSTATUS, a reset, nothing after
        tail = log[log.index(finish):]
        self.assertEqual([e[2] if e[0] == 'ctrl' else e[0] for e in tail],
                         [appleusb.DNLOAD, appleusb.GETSTATUS, appleusb.GETSTATUS, 'reset'])
        return body

    def test_short_last_packet(self):
        body = self.check(0x800 * 3 + 100)
        self.assertEqual(len(body), 4)

    def test_suffix_does_not_fit(self):
        body = self.check(0x800 * 3 - 8)
        # data alone, then the suffix in its own packet under the same number
        self.assertEqual([b[3] for b in body], [0, 1, 2, 2])
        self.assertEqual(len(body[-1][5]), 16)

    def test_exact_packets(self):
        body = self.check(0x800 * 2)
        self.assertEqual([b[3] for b in body], [0, 1, 1])

    def test_error_state_is_cleared(self):
        d = device(appleusb.DFU, state=10)
        with self.assertRaises(appleusb.Fail):
            d.send(b'x' * 10, progress=False)
        self.assertEqual(d.dev.log[-1][2], appleusb.CLRSTATUS)


class Recovery(unittest.TestCase):
    def test_bulk(self):
        data = bytes(range(256)) * 300          # 76800 bytes
        d = device(0x1281)
        d.send(data, progress=False)
        log = d.dev.log
        self.assertEqual(log[0][:3], ('ctrl', 0x41, 0))
        bulk = [e for e in log if e[0] == 'bulk']
        self.assertTrue(all(e[1] == 0x04 for e in bulk))
        self.assertEqual(b''.join(e[2] for e in bulk), data)
        self.assertEqual([len(e[2]) for e in bulk], [0x8000, 0x8000, 76800 - 0x10000, 0])

    def test_no_zlp_when_not_needed(self):
        d = device(0x1281)
        d.send(b'a' * 1000, progress=False)
        self.assertEqual([len(e[2]) for e in d.dev.log if e[0] == 'bulk'], [1000])

    def test_command(self):
        d = device(0x1281)
        d.command('go')
        self.assertEqual(d.dev.log, [('ctrl', 0x40, 0, 0, 0, b'go\0')])


class Pwned(unittest.TestCase):
    def test_sequence(self):
        data = bytes(range(256)) * 20           # 5120 bytes
        d = device(appleusb.DFU)
        d.boot_pwned(data)
        log = d.dev.log
        self.assertEqual(log[0], ('ctrl', 0x21, appleusb.DNLOAD, 0, 0, bytes(16)))
        self.assertEqual(log[1], ('ctrl', 0x21, appleusb.DNLOAD, 0, 0, None))
        self.assertEqual([e[2] for e in log[2:4]], [appleusb.GETSTATUS] * 2)
        image = [e for e in log[4:-1]]
        self.assertTrue(all(e[2] == appleusb.DNLOAD and e[3] == 0 for e in image))
        self.assertEqual(b''.join(e[5] for e in image), data)
        self.assertEqual(log[-1], ('ctrl', 0xA1, appleusb.UPLOAD, 0xFFFF, 0, 0))

    def test_refuses_unpwned(self):
        d = device(appleusb.DFU, serial='CPID:8942 BDID:00 SRTG:[iBoot-1145.3]')
        with self.assertRaises(appleusb.Fail):
            d.boot_pwned(b'x')
        self.assertEqual(d.dev.log, [])


class Serial(unittest.TestCase):
    def test_fields(self):
        f = appleusb.fields('CPID:8942 CPRV:20 BDID:00 ECID:000001 IBFL:03 SRTG:[iBoot-1145.3] PWND:[checkm8]')
        self.assertEqual(f['CPID'], '8942')
        self.assertEqual(f['SRTG'], '[iBoot-1145.3]')
        self.assertEqual(f['PWND'], '[checkm8]')


if __name__ == '__main__':
    unittest.main()
