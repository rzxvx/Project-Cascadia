# Quickstart

From a clean machine to a shell on the iPad, and on to its desktop.
`./cascadia a5 install` has been walked end to end on macOS (Apple Silicon)
and on Arch Linux (x86_64) -- from the prebuilt release and from source:
the boot through kDFU, the NFS root, ssh, XFCE on the GPU, SuperTux.
`setup` is checked on Debian, Ubuntu, Arch and Fedora. If something here
does not match what you see, that is a bug.

Nothing is written to the device's storage. The kernel is uploaded into RAM and
run from there; iOS is untouched (Linux can read its files, and never writes
them), and holding **power + home** brings the iPad back to it. That is worth
knowing before you start rather than after.

## What you need

**The device.** An iPad mini 1, Wi-Fi: **iPad2,5 / A1432 / p105ap**. The
cellular models (iPad2,6 and iPad2,7) are the same SoC on a different board and
are not supported — the boot chain is pinned to p105ap on iOS 8.4.1 (12H321),
because the auto-go hook patches an address inside that exact iBEC.  The iPad 2
Wi-Fi (iPad2,1 / K93AP) is a board of the same tree with a path of its own
(`./cascadia k93 ...`, from a jailbroken iOS): [k93-bringup.md](k93-bringup.md).

**A way into pwned DFU**, one of:

- a **Raspberry Pi Pico** (RP2040; not the Pico 2) running the checkm8-a5
  firmware (below), or an Arduino with a USB host shield. checkm8 on A5 needs
  tighter USB timing than a general-purpose host manages, which is why the
  hardware is required; or
- a **jailbroken iOS 7–9.3.6** on the iPad itself, for `./cascadia a5 flash
  --kdfu`. [EverPwnage](https://github.com/LukeZGD/EverPwnage) is untethered on
  A5, so this stays a one-command step. No extra hardware at all.

**A Lightning cable.** A DCSD cable is optional: it carries the UART, and the
flash script captures it when it is there. The early boot log also goes to the
framebuffer, so its absence is a nuisance rather than a blocker.

**A computer.** macOS, or Linux (Debian / Ubuntu, Arch or Fedora), with
about 5 GB free (12 GB to build everything from source). `./cascadia a5
setup` installs what it needs (it asks first): Docker -- still needed, for
the files made from your IPSW --, libusb and pyusb, an NFS server and
usbmuxd on Linux, a udev rule for the iPad's USB, and an ssh key without a
passphrase, `~/.ssh/cascadia`, which goes into the image. On macOS, the
Command Line Tools and [Homebrew](https://brew.sh) first.

## The short way

```bash
git clone https://github.com/rzxvx/Project-Cascadia.git
cd Project-Cascadia
./cascadia a5 install
```

It goes through every step below in order, says what each does and how
long it takes, and skips what is done already: stopped anywhere (a reboot,
a failed download, Docker not running yet), run it again and it carries on.
The kernel and the desktop root come prebuilt (the release in
`prebuilt/stable/`, ~460 MB) instead of being built -- minutes instead of
hours --, and what is yours is added on this computer: the firmware and the
GPU's pack from your IPSW, your ssh key, the iPad's host key.
`INSTALL_FROM=source ./cascadia a5 install` builds everything instead (the
newer release, when there is one: `CHANNEL=experimental`).
It asks three things: what to install on this computer, whether you want
the desktop (it then lives on this computer as the iPad's disk, over the
cable), and how the iPad gets into pwned DFU (a Pi Pico, or its jailbroken
iOS). At the end it boots the iPad; then

```bash
./cascadia a5 ssh            # a shell on the iPad
```

and `desktop` on the iPad for XFCE. Every later boot is `./cascadia a5
flash` (`--kdfu` from iOS). The rest of this page is the same road one
step at a time, and what to do on the iPad once it is there.

## Step by step: the image

The boot image is two halves: a kernel with its base system, the same for
everyone, and a small layer of this computer's own -- the firmware from your
IPSW, your ssh key, the iPad's host key, the NFS root's address --, which
`build` puts next to it (`build/overlay`, `scripts/assemble.py`).

```bash
./cascadia a5 setup         # this computer: what the rest needs
./cascadia a5 get           # the prebuilt kernel and desktop root (~460 MB)
./cascadia a5 firmware      # Apple's files, from the IPSW (next section)
./cascadia a5 build         # the bundle: the kernel + your layer, in seconds
```

**From source** instead of `get`: the kernel tree is the switch -- with one
here, `build` builds the kernel too.

```bash
./cascadia a5 kernel        # shallow clone of Linux, pinned to v6.12
./cascadia a5 rootfs        # Alpine armhf + the boot scripts + dropbear + mount.nfs
./cascadia a5 build         # dtb + kernel + the bundle
```

`kernel` downloads a few hundred MB. That `build` takes about fifteen minutes
on a 2012 Core i3 and a few on an M-series Mac, and ends by checking that the
base it just embedded really contains stage 1, stage 2, the USB console and
the pinned address — a build that skips any of them fails here rather than on
the device. `./cascadia a5 doctor` says what this machine is missing, at any
point.

Noise you can ignore on a first run: git warns that the annotated `v6.12` tag
"is not a commit", and docker may say the legacy builder is deprecated
(`pacman -S docker-buildx` silences that one).

## The boot chain

No Apple firmware ships with this repository. `./cascadia a5 firmware` derives
iBSS and iBEC from a stock IPSW, which Apple still serves:

```bash
./cascadia a5 firmware         # no IPSW yet: "Download it now? [Y/n]" -- Enter downloads it
```

It goes into the repository's top (about 1.4 GB; an interrupted download
resumes when you run it again). One you already have is found there or in
`devices/a5/` under its own name, or `IPSW=/path/to/it`. Both images are checked against reference hashes; they reproduce byte for
byte on macOS/arm64 and on Linux/x86_64.

## Into pwned DFU

Flashing needs nothing beyond pyusb and libusb, which `setup` puts here
(pyusb in `build/pylib`, no pip): `lib/appleusb.py` does the USB side (what
irecovery and primepwn did), and `--kdfu` is our own (`tools/kdfu.sh`). On
Linux, `setup` also adds the udev rule that gives your user the iPad's USB
device.

**With a Pi Pico.** Once: the firmware is LukeZGD's build of checkm8-a5,
[checkm8-pico.zip](https://github.com/LukeZGD/Legacy-iOS-Kit-Keys/releases/download/a/checkm8-pico.zip)
(from the [Legacy iOS Kit wiki](https://github.com/LukeZGD/Legacy-iOS-Kit/wiki/checkm8-a5)).
Hold the Pico's BOOTSEL button while plugging it into the computer, and copy
the **8942** UF2 (the iPad mini 1's chip) onto the `RPI-RP2` drive that
appears; its LED then blinks once a second, waiting for a device. Every boot:
the iPad in DFU, into the Pico (wired as the wiki shows), wait for the LED to say it is
done, then the iPad into this computer and `./cascadia a5 flash`.

**From a jailbroken iOS** (`--kdfu`): OpenSSH from Cydia on the iPad, the
iPad booted into iOS and unlocked, on the cable. `flash --kdfu` logs in as
root (the password is asked once: Enter is "alpine", iOS's default; a key is
used if one gets in), runs kloader with
our iBSS, and waits for the iPad to come back in DFU: as soon as the screen
goes dark, press Power or Home once. kloader has no licence, so it
is not in this repository: it is fetched once, from a pinned commit, and
checked by hash. Over Wi-Fi instead of the cable: `IOS_HOST=<the iPad's IP>`.

## Touch

Works out of the box. The digitizer's firmware is Apple's, so it does not ship
here: `./cascadia a5 firmware` takes it out of the same IPSW, with the boot chain
(`build/firmware/P105.mtprops`, checked against the copy iOS itself has).

The other thing touch needs is the panel's calibration, and that one is
different on every iPad — iBoot copies it out of syscfg at boot. The image
carries a default, `initramfs/lib/firmware/mtcal.bin`, from the iPad this port
was brought up on. On any other iPad it has not been tried yet: expect touch to
work, possibly a little less precisely than it could.

**Optional:** your own panel's calibration. With the iPad booted into its
jailbroken iOS, OpenSSH installed, on the cable:

```bash
./cascadia a5 mtcal            # iOS's root password once (Enter = "alpine")
./cascadia a5 build            # lays it over the default
```

That is the same iOS `--kdfu` starts from, so it fits right before a flash. It
lands in `build/keep/lib/firmware/`, which no rebuild deletes. The helper that
runs on the iPad, `tools/mtdump/prebuilt/mtcal`, is kept built in the tree
because only a Mac can build it (`tools/mtdump/build.sh`, Xcode); its source is
`tools/mtdump/mtcal.c`.

## Flash, and the first shell

```bash
cd Project-Cascadia
./cascadia a5 flash            # pwned by the Pi Pico, now on this computer
./cascadia a5 flash --kdfu     # from a jailbroken iOS, no extra hardware
./cascadia a5 ssh              # = ssh root@10.55.0.2, with ~/.ssh/cascadia
```

`sudo` is asked for once: for the address this host puts on its end of the
link. The USB side runs as you.

The order matters and is not decoration: our patched iBSS is started first
(on the Pico-pwned iPad, or by kloader), that iBSS accepts the unsigned iBEC,
and the iBEC's auto-go hook fires when the bundle upload ends and runs the
loader — which is why the loader goes last and why nothing here sends `go`.

`flash` finishes by giving this machine `10.55.0.1` on the gadget's network
interface and waiting for the device to answer. That has to happen on every
boot — the interface is created when the device enumerates and destroyed when
it goes away, twice per boot — and without it `ssh` does not fail, it hangs.
`./cascadia a5 link` is the same step on its own.

There are two consoles besides ssh, and both are useful when ssh is not:

- **the glass**: a shell on the framebuffer, with an on-screen keyboard
  (`fbkeyboard`) under it from boot (no arrow keys);
- **the cable**: CDC ACM, no credentials at all —
  `screen /dev/cu.usbmodem* 115200` on macOS, `sudo picocom -b 115200
  /dev/ttyACM0` on Linux (picocom exits with `C-a C-x`).

## What you get

Linux 6.12 booting to an interactive shell in about five seconds, with
interrupts, a working tick and correct wall-clock time; a framebuffer console;
CDC ACM and CDC ECM over the Lightning cable; ssh; `apk`, the whole Alpine
repository, over that link; `/bin/peek` for poking at MMIO; and multitouch,
brought up the way iOS brings it up, as an evdev device; both CPU cores and the
L2 cache; Wi-Fi (below); and iOS's own files, read-only (below).

The root filesystem is the initramfs, in RAM, so anything installed with `apk`
is gone on the next boot. `./cascadia a5 nfs on` moves the root onto the host's
disk over the same cable, which fixes both that and the 512 MB ceiling (then
`./cascadia a5 build` and a flash: the root's address goes in the image).
`./cascadia a5 net on` shares the host's internet with the device. On macOS they
drive nfsd and pf; on Linux, nfs-utils and iptables (nftables without it), from
`tools/linux-*.sh`. The export is `~/cascadia-root` on the Mac and
`/srv/cascadia-root` on Linux; `DST=` moves it.

That is also where it stays: the NAND is iOS's as long as iOS is there, and
kDFU needs iOS. With a Pi Pico, giving the NAND to Linux (and iOS up) is the
planned alternative — not written yet; the README's *Where Linux keeps its
files* says more.

## Wi-Fi

The Wi-Fi chip's firmware comes out of your IPSW with the boot chain
(`./cascadia a5 firmware`); `./cascadia a5 build` says `ok: Wi-Fi firmware from the
IPSW` when it found it. On the iPad:

```sh
wifi                          # where things stand, and what to type next
wifi scan
wifi connect "Home" secret    # leave the password out for an open network
wifi auto on                  # join the remembered networks at every boot
wifi forget "Home"
```

`iw` and `wpa_supplicant` are in the image, so Wi-Fi works from a RAM boot,
with no cable network at all. Networks are remembered in `/etc/wifi/networks/`, a WPA password only
as its PSK -- on the NFS root for good, in a RAM boot until the next reboot. Once Wi-Fi is up, ssh works over it too, at the
address `wifi` prints.

- WPA2 and open networks work on 2.4 and 5 GHz, DHCP and all.
- The MAC address is not the iPad's own (Linux cannot read it from iOS's
  syscfg): it is `02:10:5a:05:00:03` unless you build the firmware with
  another one — `WIFI_MAC=xx:xx:xx:xx:xx:xx ./cascadia a5 firmware`, then
  `./cascadia a5 build`.

## Bluetooth

The chip's Bluetooth firmware comes out of your IPSW too (`./cascadia
firmware`, `ok: Bluetooth firmware from the IPSW`). On the iPad:

```sh
bluetooth                     # where things stand, and what to type next
bluetooth scan
bluetooth pair "TH40"         # any part of the name; the device in pairing mode
bluetooth auto on             # start at boot, so paired devices reconnect
bluetooth forget "TH40"
```

A keyboard or a mouse pairs without a code, connects, and types into the
console (and into X). If a device insists on a code, `bluetooth pair NAME
code` prints one to type on it. After `bluetooth off` or a reboot, a paired
keyboard comes back when a key is pressed. Pairings are kept on the NFS root;
in a RAM boot they last until the next reboot. The address is
`02:10:5A:05:00:04`; `local-bd-address` in the dts sets another.

## iOS's files

```sh
ios mount     # iOS's System partition, read-only, on /mnt/ios
ios umount
```

`iosnand` reads the NAND itself and follows iOS's FTL to its partitions,
without writing a byte. The very first `ios mount` reads every page once —
about 12 minutes — and keeps a map in `/var/lib/iosnand` (on the NFS root);
after that it takes a second, or twenty after iOS has been running. Only
System mounts: iOS's Data partition (apps, photos) is encrypted file by file.

## A desktop

XFCE runs off the NFS root with touch as the pointer, drawn by the GPU (X
runs modesetting with glamor on the GPU's Mesa, and GL programs get GLX; the
two CPU cores draw instead with `DESKTOP_FB=1 desktop`). It comes as a whole
root -- XFCE, the patched X/xfwm4/pixman, Mesa, SuperTux, screenpad,
autorotate --, the one `get` downloads; a new NFS export starts from it, and
gets the GPU's pack from your IPSW:

```bash
./cascadia a5 nfs on           # a NEW root, from the desktop root
./cascadia a5 build && ./cascadia a5 flash --kdfu      # (or flash, with the Pico)
```

Then on the iPad, from the glass or over ssh:

```sh
desktop
```

`desktop` says "X on the GPU" when it takes it. An existing `~/cascadia-root`
is never overwritten: it is the iPad's live disk (`FORCE_SYNC=1 ./cascadia a5
nfs on` reseeds it, deleting what is there).

To build the desktop root yourself instead, `./cascadia a5 desktop-root`
(into `build/desktop-root.tar`, which `nfs on` then prefers). It is long the
first time -- XFCE is ~280 packages under ARM emulation, and Mesa and
SuperTux build for the better part of an hour each on a Mac, hours on an
older PC -- and later runs reuse what is built (`FRESH=1` starts the root
again from the current image). Each piece can still go
onto a running iPad on its own, as before: `ssh root@10.55.0.2 sh -s <
tools/desktop/xfce-setup.sh`, `./cascadia a5 dev gpu`, `./cascadia a5 dev mesa`.

- **Touch.** Tap = click. A quick swipe scrolls (a finger that rests before it
  moves selects instead — that is GTK's rule, not a fault). A two-finger tap =
  right click.
- **Keyboard.** The button next to the clock shows and hides an on-screen
  keyboard (svkbd). Ctrl is the `^` key.
- **Turning.** The screen turns with the iPad, the touch with it
  (`autorotate`, started with the session). The button next to the
  keyboard's locks the turn, and so does Home held (0.8 s). The desktop
  root's X server is the patched one, so a turned screen is as fast as an
  upright one.
- **Timing.** About 40 s to a drawn desktop on the first login, then ~25 s to
  the session and ~40 s until the desktop has settled. The background is a
  solid colour on purpose: XFCE's default wallpaper is an SVG, and rasterising
  it on one core kept the screen black for over a minute.

`desktop` checks that the system D-Bus actually answers before starting the
session. If a stopped daemon left a socket and pid file behind, it removes
only the pid file of a process that no longer exists and starts a new bus.
An unresponsive live process, malformed pid file or socket without a known
owner stops startup for inspection; the script does not kill an existing bus.

The script's header says what each package is for. Three things older recipes
had that do not work here:

- `setup-xorg-base` is not installed (it lives in `alpine-conf`, which also
  pulls in openrc);
- `xf86-video-vesa` does not exist for armv7, and one missing name fails the
  whole `apk add`;
- a touchscreen section forcing `evdev` removes multitouch, and with it the
  two-finger right click. evdev's long-press right click does not help either:
  for a device with multitouch axes X emulates the pointer itself, and evdev's
  emulation never runs.

## Touch controls for games

`screenpad` puts a game controller on the glass, over whatever runs (a
fullscreen game included): buttons, d-pads and sticks, semi-transparent,
from a preset you make on the iPad. It is in the desktop root (its GPU
driver blends the controls into each frame). On the iPad, held landscape: **SuperTux (on-screen controls)** in the
menu's Games, or from a terminal:

```sh
screenpad supertux -- supertux2
```

The d-pad moves, **Jump** is Space, **Run** is Ctrl and **Menu** is Esc. A
finger that lands anywhere else is an ordinary touch. `screenpad gamepad`
is an Xbox-style pad (a stick, a d-pad, ABXY, shoulders, Back and Start),
which games see as a real one. `screenpad -l` lists the presets.

**Your own presets:** **screenpad editor** in the menu (`screenpad-editor`).
Add buttons, d-pads and sticks, drag them into place (the blue handle
resizes), and choose each one's shape, label, colour, opacity and what it
does: a key or a gamepad button. **▶ Try it** runs the preset over the
screen with a Stop button above it; **Save** keeps it as `screenpad NAME`.
A landscape preset shows only while the iPad is held landscape.

**Playing with your preset.** The editor makes and saves presets; it does
not start games. A game gets a preset in one of two ways:

- **From the menu, SuperTux**: the menu entry runs the preset named
  `supertux`. Save yours under the name **SuperTux**: your copy
  (`~/.config/screenpad/supertux.json`) comes before the one that came with
  screenpad, and the menu entry uses it from then on. Delete that file to
  have the original back.
- **Any preset, any game, from a terminal**: the preset's name (what Save
  said: the name in lower case, spaces as `-`), then `--` and the game's
  command, as in its menu entry:

  ```sh
  screenpad my-preset -- supertux2
  ```

  The controls are there while the game runs and go with it.

## The GPU demo

Tux going round on the screen, drawn by the GPU through OpenGL ES -- his
light baked in Blender, the logo over his head glowing -- at about 58 frames
a second, fullscreen. The model is teutekeune's. It is in the desktop root:
on the iPad's console (not under X),

```sh
tux-gpudemo
```

and the **EXIT** button in the top right corner ends it. The same Mesa is
every program's OpenGL there (XFCE, SuperTux). [GPU.md](GPU.md) has the
details, how it works, and how to rebuild Mesa or the GPU's pack onto a
running iPad while working on the driver (`./cascadia a5 dev mesa`, `dev
gpu`).

## When it goes wrong

| What you see | What it is |
|---|---|
| `ssh` says `Permission denied (publickey)` | The image does not carry the key ssh offered — `ssh -v` shows which one that was, and `dmesg` on the ACM console shows dropbear's side. `./cascadia a5 build` adds the building machine's key every time; from any other machine, `PUBKEY=that.pub ./cascadia a5 build`, or append the key to `/root/.ssh/authorized_keys` on the ACM console for this boot. |
| `ssh` asks for a **passphrase** and then gives up | That is your own key on this host, not the device: ssh could not decrypt it, so it never offered it. `ssh-add ~/.ssh/id_ed25519` once, or keep a passphrase-less `~/.ssh/cascadia` (`./cascadia a5 setup` makes one). Nothing is wrong with the image — rebuilding will not help. |
| `ssh` hangs, no error | This host has no address on the link. `./cascadia a5 link`. |
| `ssh` hangs, but `ping 10.55.0.2` answers and the glass says `nfs: server ... not responding, still trying` | The NFS root lost its server, and a hard-mounted root blocks everything that touches it until the server returns — the kernel is fine, the cursor still blinks, and the console over the cable still works (it runs from RAM). This is the host's side, not the iPad's: seen with macOS's nfsd and on an Ubuntu host, never on the Arch one. On a NetworkManager host it was NM taking 10.55.0.1 off the link; `./cascadia a5 link` (and so `flash`) now gives NM a profile that keeps it. Otherwise only a reboot gets out of it today; `./cascadia a5 nfs off` and a rebuild boot from RAM instead. |
| `ssh` hangs, `ping` does not answer, the console over the cable says nothing, but the glass's cursor blinks and it shows `task ... blocked for more than ... seconds` in `nfs_...` | The gadget stopped sending to the host; the kernel is fine and the NFS root waits for it. Seen after the link was down for minutes (the Mac asleep, or `ip link set usb0 down` and the serial console reopened meanwhile) on any kernel, and on its own after boot with the fast DMA bursts of 2026-10-24 (reverted). Only a reboot gets out of it today; before rebooting, a photo of the glass is what tells it apart. |
| `REMOTE HOST IDENTIFICATION HAS CHANGED` | The device's host key changed (a new `build/keep`, another computer). `./cascadia a5 build` makes it once and prints its fingerprint; `./cascadia a5 ssh` keeps its own known_hosts in `build/` and accepts a new one. For plain ssh: `ssh-keygen -R 10.55.0.2`, then compare the fingerprint ssh shows with the build's. |
| `flash`: "the device is in DFU but not pwned" | The Pico's checkm8 did not land (or the iPad went into DFU again afterwards). DFU again, through the Pico, back to this computer. |
| `flash`: "no permission for the device" | Linux without a udev rule for Apple's devices; the message has the rule to add. |
| kDFU: "no ssh to iOS" | The iPad is not in iOS, is locked, or has no OpenSSH (Cydia). Over Wi-Fi: `IOS_HOST=<its IP> ./cascadia a5 flash --kdfu`. |
| kDFU: the iPad goes dark but never comes back | Unplug and replug the cable, or press Home; it waits 90 s. Then `./cascadia a5 flash --skip-pwn --ibec build/firmware/iBEC.patched.autogo.plain.dfu`. |
| kDFU: the iBEC uploads to 100%, then the device looks switched off | An encrypted iBEC. After iOS has booted the AES GID key is gone, so the KBAG decrypts to nothing. `--kdfu` picks the plaintext image by itself unless `--ibec` or `--known-good` pinned one. |
| `no iBoot32Patcher ... rebuild the image` | The build image predates a change to the Dockerfile. `./cascadia a5 image` (and `./cascadia a5 firmware` now does this itself). |
| `mount.nfs` reports `Failed to find 'tcp' protocol` (or `Protocol not supported`) and `/etc/netconfig` is missing | Rebuild with the current base-package list, which includes `libtirpc-conf`. To repair a running image with package access, use `apk add libtirpc-conf`. The binary and its shared libraries alone are insufficient. |
| The device boots, but from RAM when you expected NFS (`mount ... failed: Host is unreachable`) | This computer's address on the link arrived after stage 1 had given up (~30 s). `flash` asks for sudo before the boot now, so the address is set in time; check `./cascadia a5 nfs status`, then flash again. |
| The UART log stops mid-boot | Normal. It truncates late in boot; the framebuffer and `dmesg` over ssh have the rest. |

`docs/CASCADIA-CHEATSHEET.md` is the full reference: every address, every
driver, and the log of what was tried and what it did.
