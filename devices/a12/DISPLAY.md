# iPad 8 native display and brightness

J171a/iPad11,6 only. This extends the shared BadApple H11P scanout driver with
board-specific guards; it does not run the XS Max's Summit/DSI panel setup.
XR stays on simpledrm; the XS Max retains its existing display compatible.

## Normal build and boot

The standard `ipad8` DTS enables the guarded H11P takeover and the PMIC backlight
child. `build kernel` stages the built-in backlight driver; `build dtb` selects
the iPad nodes; `pack` supplies `cma=64M` for contiguous display buffers. No manual
`insmod`, write-enable command, prototype boot directory or GPU test is needed.
Use the normal commands in [QUICKSTART.md](QUICKSTART.md). Existing payloads must
be rebuilt/repacked to obtain these source changes; they do not update in place.
Neither the Apple firmware inputs nor the initramfs require changes for this feature.

The driver inherits the working iBoot pixel path and checks dimensions1620x2160,
format0x055c8000, stride, DMA window and a contiguous allocation before evicting
simpledrm. It does not perform cold panel initialization, retrain the link or
provide validated hardware vblank/suspend/resume. Failed preflight leaves the
framebuffer driver in place where possible; failures after eviction are not a
promise of automatic recovery.

## Brightness

`/sys/class/backlight/j171-backlight` uses SPMI USID0x0f, LE11 DAC register0x1e00,
and SMC `rLDl` for the separate on/off operation. The provider is the iPad's
`pmu,d2652` PMIC, identified through Apple's IOKit personality and device tree.
The low-level control is not the XS panel's DCS brightness command.

Brightness writes are **enabled by default**. Probe reads and preserves the
inherited nonzero DAC, without setting a new brightness. The current experimental
maximum is that boot-time DAC:100% means the inherited setting, **not** proof of
the panel's full calibrated range. The tested unit inherited1575; another unit or
boot may differ. The full slider/current calibration and cold-start/off-at-probe
case remain unvalidated. Transfer and readback errors are returned to userspace.

For deliberate read-only diagnosis only, boot with
`apple_j171_backlight.allow_writes=0`; normally no parameter is needed.

The iPad Debian root builder automatically installs a narrowly matched udev rule
that grants the existing `video` group write access to this backlight attribute.
It does not grant world write access or sudo. The builder already adds the desktop
user to `video`. Other desktop distributions must supply the same permissions.
For an existing root, install them without recreating it:

```sh
python3 devices/a12/rootfs/install-ipad-display.py --device ipad8 --root /path/to/rootfs
```

This updates files for the next boot/login; it does not restart a live session or
reload the running kernel. Existing users must belong to the target's `video`
group. Omarchy's existing brightness helper can use the Linux class device
without any Omarchy-specific kernel API.

## Accelerated Omarchy desktop

The installer also recognizes the existing `arch-ipad-login-session` launcher
and routes it through `cascadia-ipad-labwc`. It backs up the launcher once and is
idempotent. The wrapper enables GLES2 only on J171 with native H11P and AGX DRM
nodes and a separately installed shared G11 Mesa runtime (`cascadia-gpu`). Nodes
are resolved by platform paths rather than assuming card numbers. Without these
prerequisites it retains the software session. There is no RAM opt-in marker.

The Mesa build/install and ABI checks in [GPU.md](GPU.md) still apply. Installing
this display setup does not download Mesa, replace Debian's Mesa, turn Xfce/Xorg
into Wayland, or assert that every GUI application is accelerated. The existing
Omarchy greeter/Qt-shell software policy is preserved. Debian/Xfce gets native
scanout and brightness, not an untested claim of accelerated Xorg.

## Evidence and remaining validation

Local hardware tests passed native takeover, user-confirmed touch, full-screen
1620x2160 AGX triangle render into an H11P-owned DMA-BUF scanned out without a CPU
presentation copy, and an interactive GLES2 labwc desktop at scale2 with no GPU
error. Backlight tests passed1575->1260->1575, physical off/on and Omarchy's slider
as the unprivileged user. Detailed research dumps remain local, not in this repo.

The normal-profile integration subsequently booted with native H11P, brightness
writes enabled by default, automatic root:video permissions and both radios loaded.
Automatic AGX desktop selection was verified after login (GLES2/private Mesa,
active AGX jobs); the user confirmed the brightness slider works without a manual
write-enable command. Firefox performance remains limited/under investigation. No XR/XS hardware regression result is
claimed for this integration.
