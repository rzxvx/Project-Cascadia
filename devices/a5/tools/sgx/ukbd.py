#!/usr/bin/env python3
"""ukbd.py SECONDS: a virtual keyboard (/dev/uinput) that skips SuperTux's
level intro (space, return, space) and then holds Right, jumping every
1.5 s, for SECONDS -- SuperTux's input on KMS, where xdotool has no X
(docs/research/p105-mesa.md, M38).  It starts 9 s after it is started, in
time for supertux-mesa.sh started beside it:

    python3 ukbd.py 20 & TIME=34 SGX_DEBUG=fps supertux-mesa.sh LEVEL"""
import fcntl, os, struct, sys, time

UI_SET_EVBIT, UI_SET_KEYBIT, UI_DEV_CREATE, UI_DEV_DESTROY = 0x40045564, 0x40045565, 0x5501, 0x5502
EV_SYN, EV_KEY = 0, 1
KEY_ENTER, KEY_SPACE, KEY_RIGHT = 28, 57, 106

fd = os.open('/dev/uinput', os.O_WRONLY | os.O_NONBLOCK)
fcntl.ioctl(fd, UI_SET_EVBIT, EV_KEY)
for k in (KEY_ENTER, KEY_SPACE, KEY_RIGHT):
    fcntl.ioctl(fd, UI_SET_KEYBIT, k)
# struct uinput_user_dev: name[80], id (4 u16), ff_effects_max, abs arrays
dev = struct.pack('80sHHHHi', b'cascadia-ukbd', 3, 1, 1, 1, 0) + bytes(4 * 64 * 4)
os.write(fd, dev)
fcntl.ioctl(fd, UI_DEV_CREATE)

def ev(t, code, val):
    s = time.time()
    os.write(fd, struct.pack('llHHi', int(s), int((s % 1) * 1e6), t, code, val))

def key(code, down):
    ev(EV_KEY, code, 1 if down else 0)
    ev(EV_SYN, 0, 0)

def press(code):
    key(code, True); time.sleep(0.3); key(code, False)

T = float(sys.argv[1]) if len(sys.argv) > 1 else 20
time.sleep(9)
press(KEY_SPACE); time.sleep(1); press(KEY_ENTER); time.sleep(1); press(KEY_SPACE); time.sleep(2)
key(KEY_RIGHT, True)
end = time.time() + T
while time.time() < end:
    time.sleep(1.2); press(KEY_SPACE)
key(KEY_RIGHT, False)
time.sleep(0.5)
fcntl.ioctl(fd, UI_DEV_DESTROY)
