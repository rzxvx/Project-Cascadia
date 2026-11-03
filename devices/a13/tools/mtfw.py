#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
mtfw.py - turn the multitouch firmware plist from the IPSW into the "Z2FW"
file the HBPP loaders take (the format of asahi-installer's
asahi_firmware/multitouch.py, plist_to_bin_touchbar, read by Linux's
apple_z2.c).

  mtfw.py N104_Multitouch.raw C1FC2,2 out.bin [default-cal.bin]

N104_Multitouch.raw is the payload of Firmware/N104_Multitouch.im4p
(`ipsw img4 im4p extract`). The plist is IOKit XML (ID/IDREF). Each step of
the personality becomes one load command:

  Binary              SEND_BLOB    0x3001 nwords addr csum16 | payload | sum32
  Property            SEND_CALIBRATION addr   (the panel's MtCl, from the FDT)
  RequestCalibration  SEND_BLOB    0x1f01
  ReadModifyWrite     SEND_BLOB    0x1e33 addr mask value csum16
  Config              skipped (HID-over-SPI settings, used after boot)

Every field is little-endian here; the loader sends blobs as 16-bit SPI
words, so on the wire each half is big-endian (`1e 33`, `30 01`, ...).
The Property step's own Payload (the plist's default calibration) is written
to default-cal.bin when that argument is given.
"""
import base64, struct, sys
import xml.etree.ElementTree as ET

LOAD_COMMAND_INIT_PAYLOAD = 0
LOAD_COMMAND_SEND_BLOB = 1
LOAD_COMMAND_SEND_CALIBRATION = 2


def load_iokit_plist(data):
    root = ET.fromstring(data.rstrip(b"\0").decode())
    ids = {}

    def conv(e):
        if "IDREF" in e.attrib:
            return ids[e.attrib["IDREF"]]
        if e.tag == "dict":
            ch = list(e)
            v = {k.text: conv(x) for k, x in zip(ch[::2], ch[1::2])}
        elif e.tag == "array":
            v = [conv(x) for x in e]
        elif e.tag == "integer":
            v = int(e.text, 0)
        elif e.tag == "string":
            v = e.text or ""
        elif e.tag == "data":
            v = base64.b64decode(e.text or "")
        elif e.tag in ("true", "false"):
            v = e.tag == "true"
        else:
            v = e.text
        if "ID" in e.attrib:
            ids[e.attrib["ID"]] = v
        return v

    return conv(root)


def pad4(b):
    return b + bytes(-len(b) % 4)


def build(steps):
    out = [struct.pack("<4sI", b"Z2FW", 1)]
    for s in steps:
        t = s["Type"]
        if t == "Binary":
            payload = s["Payload"]
            hdr = struct.pack("<HHI", 0x3001, (len(payload) + 3) // 4, s["Address"])
            blob = hdr + struct.pack("<H", sum(hdr[2:]) & 0xffff) + payload + \
                struct.pack("<I", sum(payload) & 0xffffffff)
            out.append(struct.pack("<II", LOAD_COMMAND_SEND_BLOB, len(blob)) + pad4(blob))
        elif t == "Property":
            out.append(struct.pack("<II", LOAD_COMMAND_SEND_CALIBRATION, s["Address"]))
        elif t == "RequestCalibration":
            out.append(struct.pack("<II", LOAD_COMMAND_SEND_BLOB, 2) + pad4(struct.pack("<H", 0x1f01)))
        elif t == "ReadModifyWrite":
            data = struct.pack("<HIII", 0x1e33, s["Address"], s["Mask"], s["Value"])
            blob = data + struct.pack("<H", sum(data[2:]) & 0xffff)
            out.append(struct.pack("<II", LOAD_COMMAND_SEND_BLOB, len(blob)) + pad4(blob))
        elif t == "Config":
            pass
        else:
            raise SystemExit(f"unknown step type {t!r}")
    return b"".join(out)


def main():
    if len(sys.argv) not in (4, 5):
        raise SystemExit(__doc__)
    plist = load_iokit_plist(open(sys.argv[1], "rb").read())
    key = sys.argv[2]
    if key not in plist:
        raise SystemExit(f"no {key} in the plist (has: {', '.join(plist)})")
    steps = plist[key]
    open(sys.argv[3], "wb").write(build(steps))
    for i, s in enumerate(steps):
        desc = s.get("Description", s.get("Property", ""))
        size = len(s["Payload"]) if "Payload" in s else 0
        print(f"  {i:2} {s['Type']:18} {s.get('Address', 0):#010x} {size:6} {desc}")
    if len(sys.argv) == 5:
        cal = [s for s in steps if s["Type"] == "Property"]
        if cal:
            open(sys.argv[4], "wb").write(cal[0]["Payload"])


if __name__ == "__main__":
    main()
