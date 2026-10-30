# Cascadia on Apple A13 — iPhone 11 (experimental)

Linux on the iPhone 11 (iPhone12,1 / N104AP, T8030), booting to a shell on the
screen and over the Lightning cable: USB serial, USB networking and SSH.

> **Status: experimental, Phase 2 reached.** The kernel boots, the framebuffer
> console works, the USB gadget gives a shell and a network, the clock and the
> timer are right, all six cores run. No frequency scaling, no touch yet.
> The boot is tethered and needs the usbliter8 RP2350 board.

This is the A13 track of Project Cascadia. The iPad mini 1 (A5) lives in the
repository root; this directory is self-contained.

## What works

| | |
|---|---|
| Boot | usbliter8 pwned DFU → patched iBSS → m1n1 as the restore kernelcache → Linux 7.2 (16K pages) |
| Console | simpledrm + fbcon on the framebuffer iBoot left; `earlycon=applefb` draws early boot text on it |
| USB | DWC3 in device mode: ACM serial shell + CDC-ECM network, DHCP for the host, dropbear SSH |
| Clock, timer | arch timer at 24 MHz (FIQ through the AIC), PMIC RTC sets the wall clock |
| SMC | RTKit, GPIO, RTC, reboot / poweroff, power-button events |
| Storage | the internal NVMe (ANS2) is detected — **it holds iOS; never mount or write it** |
| CPU | all six cores (4x Thunder, 2x Lightning), started by m1n1 through the PMGR |
| Not yet | cpufreq (parked, `cpufreq.off=1`: the clusters' DVFS hangs now and then), touch, Wi-Fi, Bluetooth, audio, native display, GPU |

The DCSD cable carries the s5l UART0: m1n1 and then the whole kernel log
(`earlycon=applefb keep_bootcon`, 115200 8N1).

## Boot it

You need: an iPhone 11 (iPhone12,1), the usbliter8 board, a Mac or Linux host
with Docker, `llvm` + `lld`, Rust, `ipsw` 3.1.714, `irecovery`, Python 3.

```bash
a13/cascadia doctor                         # check everything
```

1. **Inputs** — the four Apple-derived files, downloaded and prepared from
   Apple's CDN (nothing from Apple is in this repository):
   ```bash
   a13/cascadia inputs                      # downloads, decrypts, patches
   ```
2. **Build** (or take the payload from a release and put it in `boot/payload/`):
   ```bash
   a13/cascadia build                       # m1n1 + kernel + initramfs
   ```
3. **Pack** — adds your `~/.ssh/*.pub` as root's authorized keys and wraps
   everything with your IM4M:
   ```bash
   a13/cascadia pack
   ```
4. **Boot** — plug in the iPhone in DFU, connect usbliter8, then:
   ```bash
   a13/cascadia boot
   ```
5. **Connect** — about 30 s later:
   ```bash
   ssh root@172.16.42.1                      # the Mac gets 172.16.42.2 by DHCP
   screen /dev/cu.usbmodemN104AP_linux1      # serial shell, no password
   ```

The kernel command line is `boot/bootargs.txt`.

## A real system: Alpine over NFS

The initramfs is a small RAM system. For a persistent one (`apk add` that
survives a reboot), the phone can run Alpine Linux from a directory on the Mac:

```bash
a13/cascadia build rootfs                  # Alpine 3.24 aarch64 -> a13/build/alpine-a13.tar.gz
a13/cascadia nfs on                        # sudo: unpack, export over the USB network (macOS or Linux)
a13/cascadia pack                          # picks the export up; boot as above
a13/cascadia net on                        # sudo, after boot: NAT for apk
```

The initramfs brings up USB, waits for the Mac's DHCP address, mounts the
export and switches to it; if anything fails it stays in RAM. Pack with
`NFSROOT=none` for a RAM-only image.

## Layout

```
boot/        package.sh, boot-from-recovery.sh, bootargs.txt, the LZSS IMG4 fix,
             overlay_cpio.py (your SSH keys), make-release.sh;
             inputs/README.md: what you provide
m1n1/        our patch to HoolockLinux/m1n1 (idevice) + build-m1n1.sh
kernel/      build-kernel.sh (yhavry/Hoolock-linux t8030, pinned), n104.fragment
             on top of Hoolock's config_16k, extras/ (framebuffer earlycon)
initramfs/   make-initramfs.sh: busybox, USB gadget (configfs), udhcpd, dropbear,
             the switch to an NFS root
rootfs/      build-alpine-rootfs.sh + overlay/ (inittab, a13-init): the NFS root
host/        mac-nfs-export.sh, linux-nfs-export.sh (nfsd),
             mac-share-internet.sh (pf NAT), linux-share-internet.sh (iptables/nft)
dts/         our first, ADT-derived minimal DT (historical; the kernel's
             t8030-n104.dts is the one used)
tools/       fb-stubs/: bare-metal payloads used to prove the hand-off works;
             peek/: peek (physical read/write), dvfs-time, spin (on the phone)
docs/        ROADMAP.md, research/ (one file per investigation, the first dmesg),
             ipad9/ (iPad 9, same T8030: recon notes)
```

Build output goes to `a13/build/` (not tracked).

## Decisions

- **No home-made bootloader.** Apple's iBSS stays in the chain; our code starts
  at m1n1. Replacing it means redoing DRAM training, PMU voltages and panel
  bring-up for nothing a user would notice; the boot is tethered either way.
- **Nothing from Apple is shipped.** You make the inputs from your own IPSW.
- **The entry is an external tool.** usbliter8 is used as it is.
- **Fix drivers, not programs**, as on the A5.

## Credits

- **yhavry** — [Hoolock-linux `t8030`](https://github.com/yhavry/Hoolock-linux/tree/t8030)
  (the A13 kernel and device trees) and [Hoolock-Docs-A13](https://github.com/yhavry/Hoolock-Docs-A13)
  (`config_16k`, the boot method)
- **Hoolock Linux** — [m1n1 `idevice`](https://github.com/HoolockLinux/m1n1/tree/idevice)
- **Asahi Linux** — m1n1, and the Apple silicon drivers Hoolock builds on
- **Paradigm Shift** — usbliter8
- **pwnerblu** — [surrealra1n](https://github.com/pwnerblu/surrealra1n) (iBSS key and patcher)
- **blacktop** — [ipsw](https://github.com/blacktop/ipsw)

## License

Our files: GPL-2.0, as the rest of Cascadia. The m1n1 patch follows m1n1's MIT
license. Release payloads contain GPL-2.0 Linux (source: the pinned commit in
`kernel/build-kernel.sh` + `kernel/`), MIT m1n1 (source: the pinned commit +
`m1n1/`), BusyBox (GPL-2.0) and Dropbear (MIT) from Debian bookworm.
