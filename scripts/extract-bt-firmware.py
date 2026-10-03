#!/usr/bin/env python3
"""Cut the Bluetooth chip's patchram out of iOS's BlueTool.

    extract-bt-firmware.py BLUETOOL MODULE OUT

iOS 8 ships no .hcd files.  /usr/sbin/BlueTool carries the patchram for every
Broadcom part Apple used -- 87 images in 8.4.1 -- each an ordinary HCD image:
Write_RAM commands (opcode 0xfc4c, a length byte, an address and data) closed
by one Launch_RAM (0xfc4e).  Each names its target in its own data, e.g.
"BCM4334B1 37.4 MHz Borg TDK": the chip, its reference clock, the board's
module ("Borg", the same name as the Wi-Fi firmware's wifi/4334b1/borg.trx)
and the module's maker.  MODULE is the end of that name ("Borg TDK"); the
maker has to match the Wi-Fi NVRAM's (borg-t-st.txt: TDK).

An image can appear more than once; the copies of one module must be the same
bytes, and this refuses to choose between copies that are not.  btbcm loads
the result as brcm/BCM4334B0.hcd, the name it gives this chip's subversion.
"""
import hashlib
import re
import sys

WRITE_RAM = b"\x4c\xfc"
LAUNCH_RAM = b"\x4e\xfc"


def images(data):
    """(offset, bytes) of every HCD image: 8 or more Write_RAMs, a Launch_RAM."""
    i = 0
    while True:
        i = data.find(WRITE_RAM, i)
        if i < 0:
            return
        j, n = i, 0
        while j + 3 <= len(data) and data[j:j + 2] == WRITE_RAM:
            j += 3 + data[j + 2]
            n += 1
        if n >= 8 and data[j:j + 2] == LAUNCH_RAM:
            end = j + 3 + data[j + 2]
            yield i, data[i:end]
            i = end
        else:
            i += 1


def name(image):
    m = re.search(rb"BCM[0-9A-Z]+ [0-9.]+ MHz [ -~]+", image)
    return m.group().decode() if m else None


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    bluetool, module, out = sys.argv[1:]
    data = open(bluetool, "rb").read()
    found = [(off, img) for off, img in images(data)
             if (name(img) or "").endswith(" " + module)]
    if not found:
        names = sorted({name(img) for _, img in images(data)} - {None})
        sys.exit("no patchram for %r in %s; it has:\n  %s"
                 % (module, bluetool, "\n  ".join(names)))
    if len({img for _, img in found}) != 1:
        sys.exit("%d different images for %r -- not choosing one" % (len(found), module))
    img = found[0][1]
    with open(out, "wb") as f:
        f.write(img)
    print("    %s: %d bytes, %d copies in BlueTool, sha256 %s -> %s"
          % (name(img), len(img), len(found), hashlib.sha256(img).hexdigest()[:16], out))


if __name__ == "__main__":
    main()
