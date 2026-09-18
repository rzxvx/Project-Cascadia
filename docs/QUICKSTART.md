# Quickstart

From a clean machine to a shell on the iPad. Every step here has been walked
end to end twice: on macOS / Apple Silicon, and on a fresh Arch Linux install
on an x86_64 Core i3 — clone, build, flash, ssh, with nothing borrowed from the
author's machine.

Nothing is written to the device's storage. The kernel is uploaded into RAM and
run from there; iOS is untouched, and holding **power + home** brings the iPad
back to it. That is worth knowing before you start rather than after.

## What you need

**The device.** An iPad mini 1, Wi-Fi: **iPad2,5 / A1432 / p105ap**. The
cellular models (iPad2,6 and iPad2,7) are the same SoC on a different board and
are not supported — the boot chain is pinned to p105ap on iOS 8.4.1 (12H321),
because the auto-go hook patches an address inside that exact iBEC.

**A way into pwned DFU**, one of:

- a **Raspberry Pi Pico** running LukeZGD's checkm8-a5 firmware (or an Arduino
  with a USB host shield). checkm8 on A5 needs tighter USB timing than a
  general-purpose host manages, which is why the hardware is required; or
- a **jailbroken iOS 7–9.3.6** on the iPad itself, for `./cascadia flash
  --kdfu`. [EverPwnage](https://github.com/LukeZGD/EverPwnage) is untethered on
  A5, so this stays a one-command step. No extra hardware at all.

**A Lightning cable.** A DCSD cable is optional: it carries the UART, and the
flash script captures it when it is there. The early boot log also goes to the
framebuffer, so its absence is a nuisance rather than a blocker.

**A host.** macOS or Linux, with `git`, `python3`, `rsync` and a running
`docker`. Everything else lives inside the build image. Budget about 12 GB:
a shallow kernel clone, its objects, the build image and the IPSW.

```bash
# Arch
sudo pacman -Syu                       # not optional; see "Legacy iOS Kit" below
sudo pacman -S --needed git python rsync docker
sudo systemctl enable --now docker
sudo usermod -aG docker "$USER" && newgrp docker
```

**An SSH key on the host**, if you want ssh on the device. `./cascadia rootfs`
installs the public key of the machine it runs on and nothing else — there is
no password login, root's password is blank and dropbear refuses it.

```bash
ls ~/.ssh/id_*.pub || ssh-keygen -t ed25519
```

## Build

```bash
git clone https://github.com/rzxvx/Project-Cascadia.git
cd Project-Cascadia
./cascadia doctor        # says what this machine is missing
./cascadia kernel        # shallow clone of Linux, pinned to v6.12
./cascadia rootfs        # Alpine armhf + the boot scripts + dropbear + mount.nfs
./cascadia build         # dtb + kernel + output/staging-bundle.bin
```

`doctor` is worth reading rather than skipping; it checks reachability, not
just presence. `kernel` downloads a few hundred MB. `build` takes about fifteen
minutes on a 2012 Core i3 and a few on an M-series Mac, and ends by checking
that the archive it just embedded really contains stage 1, stage 2, the USB
console and the pinned address — a build that skips any of them fails here
rather than on the device.

Noise you can ignore on a first run: git warns that the annotated `v6.12` tag
"is not a commit", and docker may say the legacy builder is deprecated
(`pacman -S docker-buildx` silences that one).

## The boot chain

No Apple firmware ships with this repository. `./cascadia firmware` derives
iBSS and iBEC from a stock IPSW, which Apple still serves:

```bash
curl -LO 'https://secure-appldnld.apple.com/ios8.4.1/031-31249-20150812-75235CC6-3C8F-11E5-848D-BE1A3A53DB92/iPad2,5_8.4.1_12H321_Restore.ipsw'
./cascadia firmware
```

Downloaded into the repository root under that name, it is found without being
told. Both images are checked against reference hashes; they reproduce byte for
byte on macOS/arm64 and on Linux/x86_64.

## Legacy iOS Kit

Flashing calls [Legacy iOS Kit](https://github.com/LukeZGD/Legacy-iOS-Kit) for
the two things that get a device into pwned DFU — `primepwn` (checkm8) and
kDFU — rather than reimplementing either. It brings its own `irecovery`, so
there is nothing else to install.

```bash
git clone https://github.com/LukeZGD/Legacy-iOS-Kit.git ~/Legacy-iOS-Kit
cd ~/Legacy-iOS-Kit && ./restore.sh     # ONCE, on its own, before the first flash
```

On Linux its first run installs its own dependencies and exits with "run the
script again" — successfully, having never touched the device. Run it once by
hand and leave it when it reaches its menu. On Arch, `pacman -Syu` first: Legacy
iOS Kit's installer pulls `udev`, which is systemd, and a partially upgraded
system refuses the transaction.

## Flash, and the first shell

```bash
cd Project-Cascadia
./cascadia flash            # Pi Pico in DFU, checkm8 via primepwn
./cascadia flash --kdfu     # from a jailbroken iOS, no extra hardware
ssh root@10.55.0.2
```

`sudo` is asked for twice over: raw USB to a device in DFU needs it, and so
does the address this host puts on its end of the link.

The order matters and is not decoration: `primepwn` (or kDFU) leaves a pwned
iBSS running, that iBSS accepts the unsigned iBEC, and the iBEC's auto-go hook
fires when the bundle upload ends and runs the loader — which is why the loader
goes last and why nothing here sends `irecovery -c go`.

`flash` finishes by giving this machine `10.55.0.1` on the gadget's network
interface and waiting for the device to answer. That has to happen on every
boot — the interface is created when the device enumerates and destroyed when
it goes away, twice per boot — and without it `ssh` does not fail, it hangs.
`./cascadia link` is the same step on its own.

There are two consoles besides ssh, and both are useful when ssh is not:

- **the glass**: a shell on the framebuffer, no keyboard attached to it yet;
- **the cable**: CDC ACM, no credentials at all —
  `screen /dev/cu.usbmodem* 115200` on macOS, `sudo picocom -b 115200
  /dev/ttyACM0` on Linux (picocom exits with `C-a C-x`).

## What you get

Linux 6.12 booting to an interactive shell in about five seconds, with
interrupts, a working tick and correct wall-clock time; a framebuffer console;
CDC ACM and CDC ECM over the Lightning cable; ssh; `apk`, the whole Alpine
repository, over that link; and `/bin/peek` for poking at MMIO. Touch, Wi-Fi
and the second CPU are not there — see the README for why, in detail.

The root filesystem is the initramfs, in RAM, so anything installed with `apk`
is gone on the next boot. On macOS, `./cascadia nfs on` moves the root onto the
host's disk over the same cable, which fixes both that and the 512 MB ceiling.
`./cascadia net on` shares the host's internet with the device. Both are macOS
only so far; on Linux they say so.

## When it goes wrong

| What you see | What it is |
|---|---|
| `ssh` asks for a password | The key is not root's in the image, or you are ssh'ing from a different machine than the one that ran `./cascadia rootfs`. Root has no password — use the ACM console, and check `ls -ln /root/.ssh`. |
| `ssh` hangs, no error | This host has no address on the link. `./cascadia link`. |
| kDFU: "Unable to connect to device", the iPad never reacted | Legacy iOS Kit spent the run installing its own dependencies. Run it once on its own; on Arch `pacman -Syu` first. |
| kDFU: the iBEC uploads to 100%, then the device looks switched off | An encrypted iBEC. After iOS has booted the AES GID key is gone, so the KBAG decrypts to nothing. `--kdfu` picks the plaintext image by itself unless `--ibec` or `--known-good` pinned one. |
| `no iBoot32Patcher ... rebuild the image` | The build image predates a change to the Dockerfile. `./cascadia image` (and `./cascadia firmware` now does this itself). |
| The device boots, but from RAM when you expected NFS | The host's address arrived after stage 1 had given up. `flash` now sets it while the device boots; check `./cascadia nfs status`. |
| The UART log stops mid-boot | Normal. It truncates late in boot; the framebuffer and `dmesg` over ssh have the rest. |

`docs/CASCADIA-CHEATSHEET.md` is the full reference: every address, every
driver, and the log of what was tried and what it did.
