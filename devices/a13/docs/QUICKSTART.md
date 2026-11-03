# Quickstart -- iPhone 11

From a clean machine to Linux on the iPhone 11, and then to a phone that
runs on its own: Wi-Fi, its own disk, no cable.

The macOS side (Apple silicon) has been walked end to end. The Linux side
(Debian / Ubuntu, x86_64 or arm64) is written and its container steps are
checked; a walk on a fresh Linux machine is next -- if something here does
not match what you see, that is a bug.

Two things to know before you start, not after:

- **The boot is tethered.** Every boot goes through DFU and the usbliter8
  board, from your computer. Power the phone off and it stays off until the
  next `./cascadia a13 boot`.
- **Nothing is written to the phone unless you say so.** Linux runs from RAM
  (or from your computer over NFS) and leaves iOS alone -- until you give it
  the internal storage, which **erases iOS** (asked twice, you type `ERASE`).

## What you need

**The phone.** An iPhone 11: **iPhone12,1 / n104ap**. Not the 11 Pro / Pro Max
(different board), not other A13 devices.

**usbliter8.** The RP2350 board that takes the phone from DFU into a state
where it runs our boot chain (Paradigm Shift). `usbliter8ctl` is in
`devices/a13/`.

**An IM4M.** iBoot still reads the manifest, so it has to be a real one from
a saved SHSH blob of *any* iPhone12,1 (`ipsw img4 im4m extract`), with the
entries `rdtr`, `rtsc`, `rkrn` (and `rdsk` for Alpine in RAM). It cannot be
made up. `./cascadia a13 inputs` checks it.

**A Lightning cable.** For the boot; after it, Wi-Fi can take over. A DCSD
cable is optional: it carries the whole boot log (115200 8N1).

**A computer.** macOS (Apple silicon) or Linux, ~25 GB free (the iOS root
filesystem image is 4.9 GB, the kernel tree in a Docker volume ~6 GB), and:

```bash
# macOS
brew install llvm lld dtc libirecovery python
rustup target add aarch64-unknown-none-softfloat   # rustup from rustup.rs
pip3 install pyusb
# + Docker Desktop, + ipsw 3.1.714 (github.com/blacktop/ipsw releases)
```

```bash
# Debian 12 / Ubuntu 24.04
sudo apt install git python3 python3-usb curl openssh-client \
    device-tree-compiler irecovery docker.io
sudo usermod -aG docker "$USER"          # then log in again
# x86_64 only: let Docker run the arm64 containers
docker run --privileged --rm tonistiigi/binfmt --install arm64
# + ipsw 3.1.714 (github.com/blacktop/ipsw releases, the linux tar.gz, on PATH)
```

On Linux, m1n1 builds in a container (no clang / Rust on the host), and the
kernel builds in a container of the host's own architecture.

```bash
git clone https://github.com/rzxvx/Project-Cascadia.git && cd Project-Cascadia
./cascadia a13 doctor          # says what is missing
```

## The short way

```bash
IM4M=/path/to/IM4M ./cascadia a13 inputs   # iBSS, DeviceTree, trust cache from Apple's CDN
./cascadia a13 periphery                   # touch, keyboard (Enter), Wi-Fi/BT;
                                         # the storage question: Enter = keep iOS
./cascadia a13 build                       # m1n1, kernel, initramfs, Alpine -- ~30 min the first time
./cascadia a13 pack
./cascadia a13 boot                        # iPhone in DFU, then connect usbliter8
```

About 30 s later the screen shows the console and the on-screen keyboard,
and the phone is on USB:

```bash
ssh root@172.16.42.1                     # your ~/.ssh/*.pub keys are in the image
```

Then, once (with the phone running): its own Wi-Fi / Bluetooth calibration
and MAC addresses come from its syscfg.

```bash
./cascadia a13 periphery
./cascadia a13 pack && ./cascadia a13 boot
```

On the phone:

```sh
wifi scan
wifi connect "My network" password       # remembered
wifi auto on                             # join it at every boot
```

Pull the cable: `ssh root@<its address>` over Wi-Fi (`wifi` shows it).

## Where the system lives

The initramfs picks the first that is there:

1. **The internal storage** -- if you gave it to Linux (below). Persistent,
   no cable, the whole 119 GB (on a 128 GB phone).
2. **NFS** -- a directory on your computer, over the cable
   (`./cascadia a13 nfs on` before `pack`). Persistent, but the phone stops when
   the cable goes.
3. **RAM** -- the same Alpine, unpacked into memory at every boot. Starts
   fresh each time; no cable needed after the boot.

### Giving the internal storage to Linux

This **erases iOS and everything on the phone**. iOS comes back only by a
restore in Finder / iTunes (which takes the Linux disk in turn). The small
areas with the phone's factory data (calibration, serial numbers) are never
written -- the NVMe driver refuses it.

```bash
./cascadia a13 nfs off           # if it was on: the install runs from Alpine in RAM
./cascadia a13 periphery         # "Give the internal storage to Linux?" -> ERASE
./cascadia a13 pack              # opens the disk to Linux (only that part)
./cascadia a13 boot              # boots into RAM; keep the cable
./cascadia a13 periphery         # backs up the factory data to boot/inputs/nvme-backup/,
                               # asks ERASE again, partitions, formats, installs Alpine
./cascadia a13 boot              # "[nvme] root /dev/nvme0n1p1"
```

From then on the phone boots into its own disk. `apk add` survives reboots.
A normal power-on (not DFU) shows the recovery screen: there is no iOS.

## Everyday

| | |
|---|---|
| Shell | ssh (USB `172.16.42.1`, or Wi-Fi), the screen with the on-screen keyboard, USB serial `/dev/cu.usbmodem*` (macOS) / `/dev/ttyACM*` (Linux) |
| Internet | Wi-Fi when it is up; else through your computer: `./cascadia a13 net on` |
| Wi-Fi | `wifi`, `wifi scan`, `wifi connect`, `wifi up` / `down`, `wifi forget`, `wifi auto on|off`, `wifi reset` |
| Touch off by a few mm | `./cascadia a13 recalibrate` (tap four corners), then `pack` |
| Desktop | XFCE runs on the framebuffer, X finds the touchscreen through libinput (eudev is in the root); Alpine's `setup-desktop xfce` should do -- not walked here yet |
| Off / reboot | `poweroff` / `reboot` (through the SMC) |
| A new build | `./cascadia a13 build` + `pack` + `boot`; the system on the internal storage stays as it is |

## Good to know

- **The cable can come and go.** Without a host for ~10 s the phone re-plugs
  its USB side by itself and keeps its address; your computer gets its
  address back by DHCP. (A flaky Lightning port was the reason.)
- **At 100 % on the cable** the SMC logs "Battery OverCharged" once a second
  (folded in dmesg) and stops charging; better not left on the cable for
  days.
- **Wi-Fi first, the cable second**: with both, the internet goes over Wi-Fi.
- **`periphery` is the installer** for everything around the core: run it
  again any time, what is there is kept. It asks two things: the on-screen
  keyboard (Enter = yes) and the internal storage (Enter = no).
- Nothing from Apple is in this repository: the firmware comes from Apple's
  CDN (one pinned iOS, 16.4 / 20E247) to your machine.

## Not needed to know (done for you)

The boot chain (sig-patched iBSS from 16.4 as iBEC, a DeviceTree, an empty
trust cache, m1n1 as the restore kernel), the touch controller's firmware
loaded through its bootloader, the PCIe controller brought up from the
DeviceTree's tunables, the Wi-Fi module powered through the SMC, the
firmware kept reachable after the switch to the root, the Alpine root sent as
an iBoot ramdisk (iBoot refuses a large kernel image), the write gate in the
NVMe driver. Details: `docs/research/` -- `touch.md`, `wifi.md`,
`nvme-plan.md`, `bringup-status.md`, `m1n1-patches.md`.

## Subtleties

- **ipsw 3.1.714.** `pack` repairs the compression record that version adds
  to the IMG4; other versions may not need it or break differently.
- **irecovery on Linux** (1.0.0 in Debian / Ubuntu) prints no PRODUCT /
  MODEL: the boot checks CPID / BDID instead. USB access: the package's udev
  rule gives it to the logged-in user; for `usbliter8ctl` (pyusb) run
  `sudo -E ./cascadia a13 boot`, or add a udev rule for the board.
- **x86_64 Linux**: the initramfs, the Alpine root and `periphery` run arm64
  containers (busybox, Alpine's packages) -- through qemu (binfmt), a bit
  slower; the kernel and m1n1 build natively (clang cross-compiles).
- **Docker on Linux** writes as root; the build scripts give the files back
  to you.
- **m1n1 off macOS** builds in a Debian 13 container (clang 19; Debian 12's
  clang 14 is too old for m1n1's C). On macOS it builds with Homebrew's llvm
  -- the binary tested on the phone; `M1N1_DOCKER=1` uses the container
  there as well.
- **iOS installed on the phone** does not matter: everything that runs comes
  from the pinned 16.4 files. (The internal storage of a phone whose iOS was
  much newer than 16.4 read fine; an older iOS is not tried.)
- **cpufreq is off** (`cpufreq.off=1`): the cores run at the clock iBoot left
  them. The CPU clusters' DVFS hangs now and then; parked.
- **L1 ASPM is off** (`pcie_aspm.policy=performance`): with it, the Wi-Fi
  firmware stops answering after ~15 s.
- **Touch is polled** (~5 % of a core when idle): its interrupt line does not
  move yet.
- **NFS is a hard mount**: pull the cable and the phone waits for it to come
  back -- by design (no lost writes).
- **The touchscreen panel**: the firmware is the one for the panel
  personality `C1FC2,2`; other iPhone 11 panels may need another one.
- **Two `ERASE`s** because the first one only changes the next image: the
  disk is written by the second `periphery` run, on a phone that booted an
  image with the disk open.

## When it goes wrong

| What you see | Why / what to do |
|---|---|
| "Device Tree image not valid" | the IM4M is not a real iPhone12,1 manifest -- see *What you need* |
| iBoot panic right after `bootx` | the kernel image is too large for iBoot (keep big things in the ramdisk) |
| `boot` waits for Recovery | usbliter8 did not land: DFU again, reconnect the board |
| no `172.16.42.1` | wait ~30 s; Linux: is the new USB network interface up (DHCP)? |
| no `wlan0` | `wifi reset`; `dmesg | grep -i "brcmf\|pcie"` |
| the phone is in RAM though the disk was given | `dmesg | grep "write gate"` should say namespace 1 is writable -- else `pack` again |
| anything else | the DCSD cable's log, or `dmesg`; `docs/research/` |
