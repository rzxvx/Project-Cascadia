#!/usr/bin/env python3
"""touchtest.py -- screenpad's touches end to end, on the iPad, as root:

    ssh root@10.55.0.2 python3 - < tools/screenpad/touchtest.py

A uinput touchscreen ("faketouch") is handed to screenpad with
SCREENPAD_TOUCH; scripted fingers go into it, and what comes out is read
back -- the keys from "screenpad keys", the touches X gets from "screenpad
touch".  For the supertux preset, with the screen turned left (rotate left,
rotation-lock on: a landscape preset hides otherwise).  The touchscreen's
own events cannot be faked instead: screenpad grabs it, and the kernel
drops what another file writes into a grabbed device.
"""
import fcntl
import os
import re
import struct
import subprocess
import time

EV = struct.Struct('llHHi')          # struct input_event, 32-bit ARM
EV_SYN, EV_KEY, EV_ABS = 0, 1, 3
ABS_X, ABS_Y, MT_SLOT, MT_X, MT_Y, MT_ID = 0, 1, 0x2f, 0x35, 0x36, 0x39
BTN_TOUCH = 0x14a
KEYS = {57: 'SPACE', 29: 'LEFTCTRL', 1: 'ESC', 103: 'UP', 108: 'DOWN', 105: 'LEFT', 106: 'RIGHT'}

# the fake touchscreen: uinput's ioctls, numbered for 32-bit ARM
fd = os.open('/dev/uinput', os.O_WRONLY | os.O_NONBLOCK)
for ev in (EV_KEY, EV_ABS):
    fcntl.ioctl(fd, 0x40045564, ev)                          # UI_SET_EVBIT
fcntl.ioctl(fd, 0x40045565, BTN_TOUCH)                       # UI_SET_KEYBIT
fcntl.ioctl(fd, 0x4004556e, 1)                               # UI_SET_PROPBIT DIRECT
for code, top in ((ABS_X, 4096), (ABS_Y, 4096), (MT_SLOT, 9), (MT_X, 4096), (MT_Y, 4096),
                  (MT_ID, 65535)):
    fcntl.ioctl(fd, 0x40045567, code)                        # UI_SET_ABSBIT
    fcntl.ioctl(fd, 0x401c5504, struct.pack('HH6i', code, 0, 0, 0, top, 0, 0, 0))
fcntl.ioctl(fd, 0x405c5503, struct.pack('4H80sI', 3, 1, 2, 1, b'faketouch', 0))
fcntl.ioctl(fd, 0x5501)                                      # UI_DEV_CREATE
time.sleep(1)


def node(name):
    for block in open('/proc/bus/input/devices').read().split('\n\n'):
        if 'Name="%s"' % name in block:
            return '/dev/input/' + re.search(r'event[0-9]+', block).group(0)


env = dict(os.environ, SCREENPAD_TOUCH=node('faketouch'))
sp = subprocess.Popen(['/usr/local/bin/screenpad', 'supertux'], env=env,
                      stderr=open('/tmp/touchtest.log', 'w'))
time.sleep(2.5)
keys = os.open(node('screenpad keys'), os.O_RDONLY | os.O_NONBLOCK)
clone = os.open(node('screenpad touch'), os.O_RDONLY | os.O_NONBLOCK)
tid = [100]


def put(t, c, v):
    os.write(fd, EV.pack(0, 0, t, c, v))


def raw(lx, ly):
    """a point of the screen turned left (1024 x 768) on the panel: X = 1 - v, Y = u"""
    return int(ly / 768 * 4096), int((1 - lx / 1024) * 4096)


def down(slot, lx, ly):
    x, y = raw(lx, ly)
    tid[0] += 1
    for t, c, v in ((EV_ABS, MT_SLOT, slot), (EV_ABS, MT_ID, tid[0]), (EV_ABS, MT_X, x),
                    (EV_ABS, MT_Y, y), (EV_KEY, BTN_TOUCH, 1), (EV_SYN, 0, 0)):
        put(t, c, v)
    time.sleep(0.1)


def move(slot, lx, ly):
    x, y = raw(lx, ly)
    for t, c, v in ((EV_ABS, MT_SLOT, slot), (EV_ABS, MT_X, x), (EV_ABS, MT_Y, y),
                    (EV_SYN, 0, 0)):
        put(t, c, v)
    time.sleep(0.1)


def up(slot):
    for t, c, v in ((EV_ABS, MT_SLOT, slot), (EV_ABS, MT_ID, -1), (EV_KEY, BTN_TOUCH, 0),
                    (EV_SYN, 0, 0)):
        put(t, c, v)
    time.sleep(0.1)


def drain(f, touches):
    out = []
    try:
        while True:
            b = os.read(f, EV.size * 64)
            for i in range(0, len(b), EV.size):
                _, _, t, c, v = EV.unpack(b[i:i + EV.size])
                if not touches and t == EV_KEY:
                    out.append('%s %s' % (KEYS.get(c, c), 'down' if v else 'up'))
                if touches and t in (EV_KEY, EV_ABS) and c in (MT_ID, MT_X, MT_Y, BTN_TOUCH):
                    out.append({MT_ID: 'id', MT_X: 'x', MT_Y: 'y', BTN_TOUCH: 'touch'}[c] +
                               '=%d' % v)
    except BlockingIOError:
        pass
    return out


def step(title, *actions):
    for a in actions:
        a()
    print('%-32s keys: %-36s X: %s' % (title, ', '.join(drain(keys, False)) or '-',
                                       ' '.join(drain(clone, True)) or '-'))


step('tap Jump', lambda: down(0, 932, 656), lambda: up(0))
step('tap Menu', lambda: down(0, 80, 52), lambda: up(0))
step('d-pad left, slide up, let go', lambda: down(0, 81, 585), lambda: move(0, 181, 480),
     lambda: up(0))
step('Jump, slide onto Run, let go', lambda: down(0, 932, 656), lambda: move(0, 782, 682),
     lambda: up(0))
step('Run and d-pad right at once', lambda: down(0, 782, 682), lambda: down(1, 290, 585),
     lambda: up(0), lambda: up(1))
step('a tap on the game (500, 300)', lambda: down(0, 500, 300), lambda: up(0))
sp.terminate()
sp.wait()
print(open('/tmp/touchtest.log').read().strip())
