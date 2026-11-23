# iPad 7 (J171): the bring-up

How Linux came up on the iPad 7, part by part, with what each part took and
the traps on the way. One iPad7,11 (J171, Wi-Fi, board ID 0x1c), iPadOS
18.7.9 on it, a Mac, 2026-11-22..23. The GPU has a page of its own:
[gpu.md](gpu.md). To use it rather than read about it:
[QUICKSTART.md](QUICKSTART.md).

## Where it stands (2026-11-23)

| | |
|---|---|
| the boot chain | works: gaster's checkm8, iPadOS 16.4's iBSS patched, m1n1 as the restore kernel -- open source end to end, no checkra1n |
| m1n1 | works, its proxy too: framebuffer 1620x2160, USB, the ADT read from the Mac |
| Linux 7.2 (Pauli1Go's HoolockLinux) | works, both logical CPUs, 3 GB, 16K pages |
| cpufreq | works: ten states, Zephyr 300 MHz to Hurricane 2.24 GHz; 60 s of load on both cores held the top at 32 °C |
| the screen | simpledrm + fbcon on iBoot's framebuffer, the boot logo; no native display driver |
| USB gadget (ACM + ECM, dwc2) | works: shell, network 172.16.43.0/24, ssh |
| NVMe (ANS on PCIe) | works, read-write -- behind a write gate: only Linux's own partition |
| touch (Z2 on SPI, SmartIO DMA) | works, multi-touch, this iPad's calibration |
| on-screen keyboards | fbkeyboard on the console, wvkbd on the desktop |
| buttons (gpio-keys) | the keys arrive; Home × 3 opens task-killer |
| battery, charging status | work (HDQ fuel gauge, charger) |
| Wi-Fi (BCM4355C1 on PCIe) | works: 2.4 and 5 GHz, the MAC and the calibration from SysCfg; 162/195 Mbit/s on 5 GHz |
| Bluetooth (BCM4355C1 on UART) | works: BlueZ, scanning, the address from SysCfg; RF calibration not used |
| Alpine 3.24 | in RAM (a ramdisk), or on its own 10 GB NVMe partition |
| desktop | labwc drawn by the CPU (pixman), scale 2, panel, menus, Tux |
| task-killer | works (the iPad mini 1's, ported) |
| sound | not yet: no driver for the speakers |
| rotation sensors | not yet: the accelerometer and gyro sit behind the AOP (an RTKit coprocessor) |
| the USB power source | not yet: `power_supply usb` gets an I2C NACK on the J171 |
| GPU | in progress: [gpu.md](gpu.md) |

## The boot chain

Tethered, open source end to end -- the sequence of HoolockLinux's
`remote_boot`, written as `./cascadia a10 inputs | pack | boot`:

1. **gaster** (HoolockLinux's fork, Apache-2.0) runs checkm8: pwned DFU. The
   same gaster decrypts iBSS with the SoC's AES engine (`decrypt_kbag`
   first, a throwaway that leaves the engine usable).
2. **iBSS** of iPadOS 16.4 (`iBSS.ipad7c.RELEASE.im4p`, one for J171 and
   J172), patched by **hBootPatcher** `-airs` (AES, IORVBAR, reconfig,
   signature checks). The A10 boots in one stage: no iBEC.
3. iBSS takes the **DeviceTree** (`rdtr`, iPadOS 16.4's for the board), an
   **empty trust cache** (`rtsc`: version 1, a UUID, no entries), the
   **Alpine ramdisk** (`rdsk`, optional) and **m1n1** as the restore kernel
   (`rkrn`), then `bootx`. Every image is wrapped with a T8010 IM4M from
   HoolockLinux's `remote_boot`; the patched iBSS does not check whose it is.
4. ipsw 3.1.714 writes a bad ASN.1 record into LZSS-compressed IMG4s;
   `devices/a13/boot/repair_lzss_img4.py` drops it.

iBSS and the DeviceTree come from 16.4 whatever iPadOS the iPad runs: the
boot never touches the installed one. iBoot fills the board's own data
(calibrations, serials) into the ADT at boot all the same.

## m1n1

HoolockLinux's `idevice` branch at the A13 track's commit, plus:

- **0001-0009, Paul Praschl's** (Pauli1Go/m1n1, authorship kept): the
  kernel's T8010 drivers expect them from the loader. The NVMe host memory
  buffer (`nvme-hmb`, from the ADT's `nvme-mmu0` `sart-region`), the NVMe
  link brought up through PCIe (the SecureROM sequence, a cold restart of
  the S3E controller with its boot straps prepared), SysCfg published as
  NVMEM, touch and ambient-light calibration.
- **0010**: the J171's HMB size. After the cold restart Pauli's code checks
  the controller against the exact signatures it knows, and refuses the
  kernel handoff otherwise ("No valid payload found"). The J171 matched the
  J172 in everything but `HMB=0x00b00000` (J172 0x00c00000, D111
  0x00e00000).
- **0011**: the A13 track's 0006, rebased -- the iBoot ramdisk appended to
  the initramfs (iBoot refuses a large `rkrn`, so Alpine travels apart).

`pack` appends this iPad's SysCfg to the payload (`m1n1_syscfg`, a length,
128 KiB); m1n1 reserves it, publishes it to the kernel, and the drivers take
the Wi-Fi MAC and calibration and the Bluetooth address from it.

## The kernel

Pauli1Go/HoolockLinux (Linux 7.2.0-rc1), the J172 drivers: H9P PCIe, ANS
NVMe, SmartIO + SPI + Z2 touch, HDQ battery, SN2400/D2365 charging,
BCM4355C1 Wi-Fi and Bluetooth, ambient light. J171 and J172 share
`t8010-ipad7.dtsi`. Config: HoolockLinux's `config_16k` +
`kernel/a10.fragment`, which needed, beyond Pauli's own defconfig list:

- `DMADEVICES`, `APPLE_SIO`, `INPUT_TOUCHSCREEN` -- without them touch
  stayed off silently; `REGULATOR`, `WLAN`, `APPLE_T8010_WLAN` -- the same
  for Wi-Fi;
- no `USB_G_MULTI`: built in, it took the UDC first and failed with -22,
  leaving nothing for the initramfs's own configfs gadget;
- no `FW_LOADER_USER_HELPER`: a missing firmware waited 60 s for a sysfs
  helper (the boot paused from 0.22 to 62.4 s);
- no `FRAMEBUFFER_CONSOLE_DEFERRED_TAKEOVER`: with it, no boot logo;
- Wi-Fi and Bluetooth built in (cfg80211, brcmfmac, hci_bcm on serdev): the
  root carries no modules; `RFKILL` for the panel.

Our patches:

- **0001**: the H9P PCIe driver dereferenced NULL in its probe error path --
  with one power domain `attach_genpd` returned before allocating, leaving
  `pd_count` at 1 for `detach_genpd`. Seen when the probe failed for want of
  the HMB (before 0010); the boot hung in deferred probe.
- **0002**: the NVMe write gate (below).

## The first boots

1. m1n1's proxy: up the first time (1620x2160, the ADT 196 KiB).
2. Linux from the proxy (`linux.py`): to a kernel console, then a data
   abort 62 s in -- the SmartIO firmware missing (the 60 s wait), the H9P
   probe failing, its error path oopsing (0001).
3. With the SmartIO firmware and 0001: "No valid payload found" -- m1n1's
   handoff refused (0010).
4. With 0010: Linux to the initramfs, the NVMe found, USB up -- 4 DFU boots
   from the first m1n1 to an ssh session.

## CPU

The A10 Fusion has two Hurricane and two Zephyr cores, paired: each logical
CPU is one or the other at a time, switched by performance state. Linux,
like iPadOS, sees two (`ARM64_ERRATUM_APPLE_FUSION`). `cpufreq.off=1`,
carried over from the A13 (whose DVFS hangs now and then), kept the A10 at
the lowest Zephyr state iBoot left; without it apple-cpufreq and schedutil
use all ten states. A shell loop: 3.38 s before, 0.70 s after. 60 s of load
on both cores: the top state held, 30 -> 32 °C, no stall. No cpuidle states
yet (WFI only: idle costs more battery than in iPadOS); KPTI is on.

## Firmware

All from iPadOS 18.7.9 (22H355), all checked against the SHA-256 sums of
Pauli1Go's `ipad7.md` (`tools/firmware.sh`):

- **SmartIO**: `AppleT8010SmartIO`'s `__DATA.__firmware`, cut out of the
  kernelcache (a partial download of the IPSW).
- **Touch**: `J171_Multitouch.im4p` is byte-identical to the J172's, and the
  ADT's `hid-merge-personality` is the same `K1F19,6`. makez2fw picks
  `Kona-J171-master.bin` out of it by itself and adds this iPad's
  calibration from SysCfg (MtCl, FfCl, MtDO, FPCL, MtPc, ETCL). The kernel
  names the device `iPad7,12 Touchscreen` on both boards (the shared DT).
- **Wi-Fi and Bluetooth** live in the root filesystem image,
  `094-32804-023.dmg.aea`: 5.9 GB, AEA-encrypted. Read from the iPad itself
  it was no good: apfs-fuse fails on the iOS keybag ("Initialization of
  KeyManager failed"), there is no FUSE in the kernel, and 7-Zip will not
  open a block device. With its AEA key the image decrypts to plain APFS
  (`NXSB` at 32), from which 7-Zip reads the BCM4355C1 directory and
  `BlueTool`. The Wi-Fi files are named the six ways Pauli1Go's guide stages
  them; the Bluetooth module reports "RudderB MUR" (Murata), so the
  `M-YSBU_V-m__m-2.5` Patchram image is the J171's too.

## Storage

The NVMe: 4096-byte sectors, 32 GB; namespace 1 holds a GPT with one APFS
container over all of it, namespaces 2-7 Apple's own (n3 is SysCfg,
128 KiB).

**Making room** (`./cascadia a10 storage`): Linux cannot write APFS, so
iPadOS shrinks its own container, live, with its keys -- as OrchardLinux's
resize-apfs does, but from a terminal. `cascadia-storage` (C, signed by
ldid with the entitlements resize-apfs uses) runs as root in a jailbroken
iPadOS over SSH: `APFSContainerGetMinimalSize` and `APFSContainerResize`
from the private APFS.framework, then the GPT -- the backup copy first, then
the primary, both read back. On this iPad: the container 32.0 GB, its
minimum 15.57 GB; Linux 10 GB, iPadOS 22.0 GB (6.4 GB over its minimum), in
about four minutes. While it runs, `df` in iPadOS shows the cut part as gone
already.

**The write gate** (kernel patch 0002, after the A13's): in nvme-pci, for
Apple's ANS (106b:2002) only. Writes, write-zeroes and the kernel's own
discards must lie in a range of `apple,write-ranges` on the PCIe host
bridge; none: no writes anywhere. Format, firmware, namespace, sanitize,
security-send and vendor admin commands: never. `nvme setup` notes the
Linux partition (checked against the kernel's view) and `pack` writes its
range into the DT. Tested: with no range, a write to the empty Linux
partition came back "write gate: refused I/O opcode 0x01 nsid 1 lba
5370880". `/init` keeps the APFS partition and the other namespaces
read-only on top.

## The root

- **Alpine in RAM**: the base root (`rootfs/packages`) as an iBoot ramdisk,
  unpacked into a tmpfs by `/init`. About 19 MB now (fbkeyboard, fonts,
  task-killer, the Wi-Fi tools); iBoot took it.
- **Alpine on the NVMe**: ext4 `cascadia-root`, the root with the desktop
  (`rootfs/packages` + `packages-desktop`, 118 MB packed -- Mesa's llvmpipe
  most of it). `/init` boots from it first.
- **Rewritten files came back empty.** `rootfs sync` rewrote the overlay's
  files in place; minutes later the iPad was reset through DFU, and ext4's
  journal recovery left 17 of them -- a10-init, inittab, labwc's
  configuration -- at length 0: BusyBox init had nothing to run, the boot
  stopped after `[nvme] root`. Since then `rootfs sync` ends with `sync`,
  and a10-init writes dirty pages out within 5 s (30 s by default). The way
  in to mend such a root: `cascadia.root=ram`, and `cascadia.debug` to see
  where a10-init stops.

## The desktop

labwc on simpledrm, `WLR_RENDERER=pixman`, at scale 2 (an integer scale: a
fractional one would make pixman scale every window on the CPU), touch
mapped to the screen by its name, the iPad mini 1's theme, panel (waybar),
apps' list (fuzzel), Wi-Fi and Bluetooth menus, wvkbd. Wayland needs
`/dev/shm` (wlroots' keymaps), which a10-init mounts. SDL apps need
Wayland's client libraries, libxkbcommon and libdecor.

What the CPU draws: SuperTux 0.6.3 in a 1024x768 window, about 10 FPS with
its SDL renderer (one core busy) and about the same through OpenGL on
llvmpipe (both cores at 85 %, labwc 1 %) -- the rasterising itself is the
limit. A desktop of windows is fine; games wait for the GPU.

**task-killer**: the iPad mini 1's, ported. Its window was drawn over KMS
through the SGX's overlay plane, which simpledrm does not have: here, when a
graphics session holds the screen, it switches to a free VT -- seatd sets
the session aside -- draws onto fb0, and switches back on Cancel; a session
too hung to let go is drawn over (nothing draws then). Home × 3 is read from
gpio-keys by task-killer itself.

## Traps on the way

- BusyBox `blockdev` takes one device per call: `--setrw dev1 dev2` changes
  nothing.
- BusyBox `pkill -x` compares the whole `argv[0]`: fbkeyboard, started as
  `/usr/bin/fbkeyboard`, did not match. `killall`, `pidof`.
- macOS's bash 3.2 cannot source a process substitution (`. <(...)`).
- macOS's tar writes your UID and the group name "wheel" (GID 10 on
  Alpine): `--uid 0 --gid 0 --uname root --gname root`.
- The initramfs's `/init` lives in a single-quoted `docker ... bash -c`:
  one apostrophe in a comment breaks the build.
- The shell on the screen has BusyBox init's PATH, without `/usr/local/bin`;
  ssh has dropbear's: the commands are linked into `/usr/bin`.
- The kernel logs Bluetooth's placeholder address at probe; the real one
  arrives later through NVMEM.

## Not yet

- Sound: the speakers have no driver.
- Rotation: the accelerometer and gyro are behind the AOP (RTKit).
- The USB power source on the J171 (`power_supply usb`: I2C NACK).
- Deep idle (cpuidle), Bluetooth RF calibration (BTRx, BTTx, BCAL).
- Booting without the computer: checkm8 is a tethered exploit.
- The GPU: [gpu.md](gpu.md).
