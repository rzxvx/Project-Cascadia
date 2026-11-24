# J171: the screen, the desktop, task-killer

## The screen

No display driver: **simpledrm** on the framebuffer iBoot leaves --
0x8bc2dc000, 1620x2160, 32 bpp (XRGB8888), stride 1632 pixels (6528 bytes),
13.4 MiB. fbcon on it at ~1.0 s:

```
[drm] Initialized simpledrm 1.0.0 for 8bc2dc000.framebuffer on minor 0
simple-framebuffer 8bc2dc000.framebuffer: [drm] fb0: simpledrmdrmfb frame buffer device
Console: switching to colour frame buffer device 202x135
```

No boot logo at first: `FRAMEBUFFER_CONSOLE_DEFERRED_TAKEOVER` (on in
config_16k) skips it. Off now; the Tuxes are two (two CPUs), in the corner
(`fbcon=logo-pos:center` pushed the whole log under them).

## Wayland: labwc on pixman

labwc 0.20.0 (Alpine's) on wlroots 0.20, `WLR_RENDERER=pixman`, a DRM dumb
allocator, a software cursor (simpledrm has no cursor plane, which also
turns direct scan-out off). The iPad mini 1 needed labwc 0.20.3 (Alpine's
0.20.0 left a black screen without a hardware cursor); here 0.20.0's first
modeset went through: `Modesetting with 1620x2160 @ 60.000 Hz`.

What it took:

- **/dev/shm.** The first start: `Failed to allocate shm file for keymap`.
  The initramfs's switch to the root does not bring one; a10-init mounts a
  tmpfs there.
- **seatd** (`-g video`) for the screen and the input devices,
  `LIBSEAT_BACKEND=seatd`; dbus's system bus for the programs that want it.
- **scale 2** (`wlr-randr --scale 2` in labwc's autostart): 810x1080 in
  logical pixels. An integer scale: a fractional one would have pixman
  scale every window's buffer on the CPU.
- **fbkeyboard off** while it runs: it draws on fb0 directly. busybox `pkill
  -x` did not stop it (it compares the whole `argv[0]`, here
  `/usr/bin/fbkeyboard`); `killall` does. It comes back when labwc ends --
  once fbkeyboard was in the root itself, not only in the initramfs.

The desktop, as on the iPad mini 1: the mini's theme, waybar's panel (apps
with fuzzel, the windows, wvkbd, Wi-Fi and Bluetooth with their menus, the
battery, the clock; no rotation button), labwc's menu on a tap of the
background, Tux on the wallpaper. `desktop` starts and stops it;
`desktop auto` at boot. waybar's network and Bluetooth modules want
`/dev/rfkill` (`RFKILL` built in since).

## What the CPU draws

SuperTux 0.6.3 (SDL needs Wayland's client libraries, libxkbcommon and
libdecor; without them "wayland not available"):

| | |
|---|---|
| SDL renderer, 1024x768 window | about 10 FPS, one core at 81 % |
| OpenGL on llvmpipe (`LIBGL_ALWAYS_SOFTWARE=1`), the same window | about the same, both cores at 85 %, labwc 1 % |
| either, full screen 1620x2160 | slower still |

The game's rasterising is the limit, not the compositor or the copy to the
screen. Windows, terminals, menus are fine; games wait for the GPU.

## task-killer

The iPad mini 1's (`devices/a5/tools/task-killer`), ported
(`devices/a10/tools/task-killer`, static aarch64). Three things changed:

- **No overlay plane.** On the mini the window goes into an SGX buffer the
  kernel blends over KMS. simpledrm has no such plane, so when the active
  VT is in `KD_GRAPHICS` (a session holds the screen) task-killer opens a
  free VT (`VT_OPENQRY`) and switches to it -- seatd sets labwc aside --
  sets it to graphics mode so fbcon keeps off, draws on fb0, and switches
  back on Cancel. The switch is waited for 2 s at most: a session too hung
  to let go is drawn over (nothing draws then).
- **Home × 3 read by itself**: gpio-keys, `KEY_HOMEPAGE`, three presses each
  within 0.8 s (the mini has a key daemon of its own for it).
- **The basic processes** are the A10's: a10-init, the initramfs's USB
  helpers in `/run/cascadia` (usbwatch, udhcpd), dropbear, getty, seatd,
  udevd, fbkeyboard, the system bus, bluetoothd, wpa_supplicant, udhcpc.
  After Continue, the text console with fbkeyboard (its touchscreen found
  by name).

## The console

fbkeyboard (Alpine's) on the console, started by `/init` on the touchscreen
it finds by name; the shell on `tty1` is `getty -n -l /bin/sh`, with BusyBox
init's PATH -- without `/usr/local/bin`, so the commands are linked into
`/usr/bin`.
