# Quickstart -- iPad 7

From a clean Mac to Linux on the iPad 7, and on to its own storage and its
desktop. Walked end to end on macOS (Apple Silicon) with one iPad7,11
(J171, Wi-Fi) on iPadOS 18.7.9, 2026-11-22..23. Linux as the host is not
walked yet: the builds run in Docker and should work there, the iPadOS image
is decrypted with macOS's `aea` (elsewhere `ipsw fw aea`, untried). The
iPad7,12 (J172, cellular) shares the device tree and should boot the same
way; it is not tested here. If something here does not match what you see,
that is a bug.

Three things to know before you start, not after:

- **The boot is tethered.** Every boot goes through DFU and checkm8 from your
  computer. Power the iPad off and it comes back as iPadOS; Linux needs
  `./cascadia a10 boot` again.
- **Nothing is written to the iPad unless you say so.** Linux runs from RAM
  until you give it a partition of its own. That step (`./cascadia a10
  storage linux`) shrinks iPadOS's APFS container -- iPadOS does it itself,
  your data stays -- and asks for a backup and a typed `RESIZE` first.
- **There is no clean power-off.** The iPad leaves Linux only by a hard
  reset (DFU, or holding power + home). Type `sync` before, when you just
  installed or changed things: ext4 has brought files rewritten a moment
  before a reset back empty.

> **Stuck in a program that will not let go of the screen?** Press **Home
> three times**, quickly (each press within 0.8 s of the last): task-killer
> asks, then kills it, or everything but the basic system. No computer
> needed.

## What you need

**The iPad.** An iPad 7: **iPad7,11** (Wi-Fi, J171AP) or **iPad7,12**
(cellular, J172AP). Not the iPad 8 (J171aAP, A12): that one is in
[devices/a12](../../a12/README.md). Any iPadOS: the boot never runs it.

**A Lightning cable.** A DCSD cable is optional: it carries the UART, the
kernel log from the first line, at 115200 8N1.

**A Mac** (Apple Silicon), about 25 GB free: the iPadOS root filesystem
image for the Wi-Fi and Bluetooth firmware is 5.9 GB, the kernel tree in a
Docker volume about 6 GB.

```bash
brew install llvm lld dtc libirecovery libimobiledevice ldid python
rustup target add aarch64-unknown-none-softfloat   # rustup from rustup.rs
# + Docker Desktop, + ipsw 3.1.714 (github.com/blacktop/ipsw releases),
# + Xcode (its iPhoneOS SDK builds the storage helper)
```

`./cascadia a10 doctor` says what is missing.

**For a partition of Linux's own** (optional, later): the iPad booted into a
jailbroken iPadOS with OpenSSH -- palera1n (rootless) works on iPadOS 18 on
the A10. Cascadia does not jailbreak the iPad; it needs root there only to
let iPadOS shrink its own storage.

## The short way

```bash
git clone https://github.com/rzxvx/Project-Cascadia.git
cd Project-Cascadia
./cascadia a10 doctor
./cascadia a10 build            # gaster, hBootPatcher, makez2fw, hcdpack; m1n1;
                                # the kernel (Docker); the initramfs; fbkeyboard;
                                # the Alpine roots
```

Put the iPad into **DFU**: cable in, hold **power + home** about 8 s, let go
of power and keep home held about 5 s more. The screen stays black.

```bash
./cascadia a10 inputs           # once per board: checkm8, iBSS + DeviceTree from
                                # Apple's CDN, decrypted and patched
./cascadia a10 firmware         # Apple's files for the drivers (see below)
./cascadia a10 pack
./cascadia a10 boot             # ~30 s later: Tux, then a shell on the screen
```

Then, from the Mac: `ssh root@172.16.43.1` (no password: the cable is
yours), or the serial shell `/dev/cu.usbmodemiPad7_linux1`. The first boot
runs Alpine from RAM.

### Touch, the first time

The touch controller's firmware carries this iPad's own calibration, read
from its SysCfg (a small NVMe namespace, read-only). With Linux up:

```bash
./cascadia a10 syscfg           # SysCfg -> build/private/syscfg.bin (never share it)
./cascadia a10 firmware         # now also the touch firmware, built with it
./cascadia a10 pack             # and SysCfg for m1n1: Wi-Fi MAC + calibration, BT address
```

The next boot has touch, and the on-screen keyboard (fbkeyboard) under the
text console.

### What `firmware` fetches

Everything from iPadOS 18.7.9 (22H355), checked by SHA-256:

- the SmartIO firmware (the touch controller's SPI DMA), cut out of the
  kernelcache -- a partial download;
- the touch firmware, `J171_Multitouch.im4p` (the J172's is the same file),
  plus this iPad's calibration from SysCfg, through makez2fw;
- the Wi-Fi firmware and NVRAM (BCM4355C1, "rudderb") and the Bluetooth
  Patchram image (cut out of `BlueTool` by hcdpack): these live in the root
  filesystem image, 5.9 GB, AEA-encrypted. It is downloaded once (resumable,
  into `build/fwsrc`), decrypted with its key from `sources.env`, the two
  places read out with 7-Zip in a container, and the decrypted image deleted.
  The encrypted one stays for a re-run; delete it when you like;
- cfg80211's `regulatory.db`, from Alpine.

## A partition of its own

Linux in RAM forgets everything at the next boot. To keep it, give it part
of the internal storage. Three steps, two of them in Linux:

**1. Make the partition** -- in iPadOS. Back up the iPad (Finder), boot it
into your jailbroken iPadOS with OpenSSH, then:

```bash
./cascadia a10 storage inspect    # read-only: the GPT, the APFS container, and how
                                  # far iPadOS says it can shrink it
./cascadia a10 storage linux      # Linux size in GB: Enter = 10, at most 14 recommended
```

`storage linux` shows the plan first (how much iPadOS keeps, its own
minimum), asks whether you have a backup and for a typed `RESIZE`, then
shrinks APFS -- a few minutes, do not unplug -- writes the GPT and reads it
all back. It refuses a size that leaves iPadOS less than 2 GiB over its
minimum. Reboot iPadOS afterwards. The new partition is empty and
unformatted; iPadOS ignores it.

**2. Open it for Linux** -- boot Linux (RAM), then:

```bash
./cascadia a10 nvme setup         # read-only: the "Linux" GPT entry, checked
./cascadia a10 pack               # the kernel may write that partition now -- only it
./cascadia a10 boot               # (DFU)
```

**3. Put Alpine on it:**

```bash
./cascadia a10 nvme format        # ext4 "cascadia-root" + the Alpine root with the
                                  # desktop; asks for a typed FORMAT
./cascadia a10 boot               # (DFU) -- Linux now starts from the NVMe
```

From then on everything you install stays, and the desktop starts at boot.

## Everyday

On the iPad, on the screen or over ssh:

| | |
|---|---|
| `desktop` | the Wayland desktop (labwc); `desktop stop` back to the console; `desktop auto on\|off` at boot or not |
| `wifi scan`, `wifi connect "SSID" [PASS]`, `wifi auto on` | Wi-Fi; the network is remembered |
| `bluetooth`, `bluetooth pair NAME`, `bluetooth auto off` | Bluetooth (on at boot unless turned off) |
| Home × 3 | task-killer |
| `sync` | before every reset |

The panel on the desktop has the apps, the windows, the keyboard, Wi-Fi and
Bluetooth menus, the battery and the clock; a tap on the background opens
labwc's menu.

On the Mac:

| | |
|---|---|
| `./cascadia a10 net on` | the Mac's internet for the iPad over the cable (pf NAT; sudo) |
| `./cascadia a10 rootfs sync` | bring the iPad's root up to this tree: packages and configuration (`HOST=root@<its Wi-Fi address>` to do it without the cable) |
| `./cascadia a10 nvme status` | what the kernel lets write, and where |
| `./cascadia a10 boot proxy` | m1n1's proxy instead of Linux, for m1n1's proxyclient |

With Wi-Fi up the cable is needed only for the boot.

## When it goes wrong

- **The boot stops at `[nvme] root /dev/nvme0n1p2`, or anywhere after.** The
  root on the NVMe is broken. Boot Alpine in RAM instead, and mend it:

  ```bash
  EXTRA_BOOTARGS="cascadia.root=ram" ./cascadia a10 pack && ./cascadia a10 boot
  # on the iPad: mount /dev/nvme0n1p2 /mnt, look, fix, sync
  ```

  `EXTRA_BOOTARGS="cascadia.debug"` makes a10-init trace every step into the
  kernel log (the screen, the DCSD UART). Pack once more without them after.
- **No `usbmodem` and no 172.16.43.1 after `boot`.** Look at the screen: the
  kernel log is there from the first line. With a DCSD cable, the same log
  at 115200 8N1: `screen /dev/cu.usbserial-* 115200`.
- **"No valid payload found" on the screen.** m1n1 refused to hand over:
  the line above it says why (on the J171 it was the NVMe's HMB size, now
  known).
- **`desktop: not found`.** An old root: `./cascadia a10 rootfs sync`.
- **checkm8 does not take.** DFU again; replug the cable; try another USB
  port. gaster is usually quick on the A10.
