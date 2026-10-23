#!/usr/bin/env python3
"""xshot.py [--screen] OUT.png [DISPLAY] -- a screenshot of the iPad's X
server (M36: XFCE on the GPU): xwd over ssh, written as a PNG here (no PIL
needed).  xwd is on the iPad by tools/device-apk.sh xwd.
DEVICE=root@10.55.0.2.

--screen: what is on the glass instead, /dev/fb0 (the scanout's memory),
turned as X's screen is (`rotate`).  xwd reads X's own front buffer, which
a page flip leaves behind: a fullscreen GL program presenting with vsync
is flipped (M42), and xwd shows the desktop under it.

    python3 tools/desktop/xshot.py /tmp/desktop.png
    python3 tools/desktop/xshot.py --screen /tmp/game.png
"""
import os, struct, subprocess, sys, zlib


def png(path, w, h, rows):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + \
            struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)
    raw = b''.join(b'\0' + r for r in rows)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def screen(out, display):
    """/dev/fb0, BGRX rows of the panel (portrait), turned as X is"""
    dev = os.environ.get('DEVICE', 'root@10.55.0.2')
    info = subprocess.run(['ssh', dev, 'cat /sys/class/graphics/fb0/virtual_size '
                           '/sys/class/graphics/fb0/stride; DISPLAY=%s /usr/local/bin/rotate 2>/dev/null '
                           '|| echo normal' % display],
                          capture_output=True, text=True, check=True).stdout.split()
    w, h = (int(v) for v in info[0].split(','))
    stride, turn = int(info[1]), info[2]
    d = subprocess.run(['ssh', dev, 'head -c %d /dev/fb0' % (stride * h)],
                       capture_output=True, check=True).stdout

    def px(x, y):
        o = y * stride + 4 * x
        return d[o + 2], d[o + 1], d[o]
    # RandR's turns are anticlockwise: the panel's picture turned back
    if turn == 'left':
        W, H, at = h, w, lambda x, y: px(y, h - 1 - x)
    elif turn == 'right':
        W, H, at = h, w, lambda x, y: px(w - 1 - y, x)
    elif turn == 'inverted':
        W, H, at = w, h, lambda x, y: px(w - 1 - x, h - 1 - y)
    else:
        W, H, at = w, h, px
    rows = [bytes(c for x in range(W) for c in at(x, y)) for y in range(H)]
    png(out, W, H, rows)
    print('%s: %d x %d (the screen, %s)' % (out, W, H, turn))


def main():
    args = [a for a in sys.argv[1:] if a != '--screen']
    if not args:
        sys.exit(__doc__)
    display = args[1] if len(args) > 1 else ':0'
    if '--screen' in sys.argv:
        return screen(args[0], display)
    d = subprocess.run(['ssh', os.environ.get('DEVICE', 'root@10.55.0.2'),
                        'DISPLAY=%s xwd -root -silent' % display],
                       capture_output=True, check=True).stdout
    # XWD's header: 25 big-endian words, then the window's name, the colours
    f = struct.unpack('>25I', d[:100])
    hs, w, h, bpp, bpl, ncolors = f[0], f[4], f[5], f[11], f[12], f[19]
    off = hs + 12 * ncolors
    step = bpp // 8             # (24: three bytes a pixel, rows of bpl bytes)
    rows = []
    for y in range(h):
        line = d[off + y * bpl:off + y * bpl + w * step]
        rgb = bytearray(3 * w)
        rgb[0::3] = line[2::step]
        rgb[1::3] = line[1::step]
        rgb[2::3] = line[0::step]
        rows.append(bytes(rgb))
    png(args[0], w, h, rows)
    print('%s: %d x %d' % (args[0], w, h))


if __name__ == '__main__':
    main()
