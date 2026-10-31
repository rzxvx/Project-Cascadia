# t8030 (iPhone 11, N104AP) Linux bring-up — status and the early-boot wall

2026-10-30. Device: iPhone12,1 / N104AP / A13 / CPID 0x8030 / BDID 0x04,
SecureROM iBoot-4479. Boot kit:
IPSW 16.4 / 20E247 for N104. Host: Apple-silicon Mac; DCSD serial cable
(`/dev/cu.usbserial-*`), usbliter8 pwned DFU.

## Where we are

The boot chain **works up to and including the jump into Linux**, and the
Linux payload **executes**. What does not work yet is Linux getting far enough
to show anything. The short version:

1. `usbliter8ctl boot iBSS.n104.20E247.sigpatched.raw` → iBSS Recovery.
2. `irecovery` sends `DeviceTree` (rdtr), empty `RestoreTrustCache` (rtsc),
   then the `rkrn` payload, then `bootx`.
3. The `rkrn` payload is **m1n1 (idevice) + the Linux kernel + DTB + initramfs
   concatenated**. m1n1 runs fully on A13 (MMU, pmgr=177 devices, AIC, display,
   framebuffer), scans the appended payloads, prepares the FDT, and jumps:
   `Vectoring to next stage...`.
4. Linux is entered at **EL1, MMU off, x0 = DTB phys** (standard arm64 boot
   protocol) — see [m1n1-patches.md](./m1n1-patches.md) for the handoff detail.
5. **Linux executes** (proven with bare-metal stubs that paint the framebuffer —
   see [console-investigation.md](./console-investigation.md)) but **hangs very
   early, before `parse_early_param()`**, i.e. before any earlycon can be set
   up. On every Linux boot the screen stays on m1n1's logo and nothing comes
   out of any console we can observe.

## The most likely root cause (found 2026-10-30): wrong kernel config

We built the kernel with `make defconfig` plus a handful of `scripts/config -e`
tweaks. That was always a shortcut, and it is
almost certainly why early boot dies. The Hoolock A13 port ships a **known-good
full config, `config_16k`** (in [Hoolock-Docs-A13](https://github.com/yhavry/Hoolock-Docs-A13),
a complete 4986-line `.config` for Linux/arm64 7.1.0). The deltas that matter:

| Option | Hoolock `config_16k` | Our `defconfig` build | Why it matters |
|---|---|---|---|
| Page size | `ARM64_16K_PAGES=y` | `ARM64_4K_PAGES=y` ❌ | Apple Silicon is **16K-native**. Asahi/Hoolock require 16K. m1n1, the DTB memory map and reserved regions are 16K-granular. A 4K kernel is untested and is the prime suspect for a hang in early MMU/memory setup. |
| `APPLE_DART` | `=y` | `=m` | IOMMU; must be built-in for early platform bring-up. |
| `APPLE_MAILBOX` | `=y` | `=m` | RTKit mailbox (SMC/NVMe/DCP). |
| `APPLE_RTKIT` | `=y` | `=m` | RTKit coprocessor core. |
| `APPLE_SART` | `=y` | `=m` | SART (NVMe/DMA scatter). |
| `PINCTRL_APPLE_GPIO` | `=y` | `=m` | Pinctrl/GPIO. |
| `APPLE_WATCHDOG` | `=y` | `=m` | WDT. |
| `APPLE_USBCOMPLEX` | `=y` | not set | The USB-gadget console complex (see "console" below). |
| `APPLE_AIC` | `=y` | `=y` ✓ | irqchip — we already had this. |
| `APPLE_PMGR_PWRSTATE` | `=y` | `=y` ✓ | power domains — already had this. |
| `DRM_SIMPLEDRM` | `=y` | `=y` (added) | on-screen fbcon. |

Everything Apple being a **module** (`=m`) means it is not present during early
boot at all, and the **4K page size** is the standout wrong-for-the-platform
setting. The Hoolock A13 feature page (`docs-a13/features/A13.md`, t8030 branch
commit `38e339f66253b40b046ef84bd910426eba12f822`) lists SMC, CPU-freq,
simpledrm, backlight, MIPI-DSI as *validated* on A13 — so the port boots; our
config just isn't the port's config.

### Next action (highest confidence, not yet tried)

Rebuild the kernel using `config_16k` as `.config` instead of `make defconfig`,
then add only what we still need on top (initramfs console marker, the fb
earlycon is optional once simpledrm works). Concretely, change
the kernel build to copy `config_16k` → `.config` and run
`make olddefconfig` (16K pages, Apple drivers built-in), rather than
`make defconfig` + manual `scripts/config`. Keep `CONFIG_ARM64_16K_PAGES=y`.
Then repackage (cache-fixed m1n1 — see m1n1-patches.md — plus this kernel) and
boot; with the real config, simpledrm/fbcon should bring the screen up on its
own and we should finally see the kernel log, or a real panic to debug.

## Session 3 (2026-10-30): the "hangs before parse_early_param" conclusion was unsafe

The fb earlycon v1 drew its text into the **top 59 pixel rows** of the panel
(`early_ioremap(0x8fa298000, 0x30000)`). On the iPhone 11 those rows sit under
the **sensor notch** (middle ~420 px) and the **rounded corners** (~80 px radius),
so a working earlycon would have been almost invisible. "Screen showed nothing"
therefore does **not** prove Linux hung before `parse_early_param()`.

Fix: `kernel/extras/apple_fb_earlycon.c` v2 draws a 24-line x 51-column text
area at pixel rows 640..1023 (middle of the screen), 8x8 font drawn 2x, mapped
as six 64-row `early_ioremap` bands (one 256K fixmap slot each, one slot left
free). Use with `earlycon=applefb keep_bootcon` (keep_bootcon matters: the VT
console registers with dummycon long before simpledrm, which would otherwise
switch the earlycon off while nothing is visible yet).

Kernel config switched to Hoolock `config_16k`
(`kernel/build-kernel.sh` fetches it at a pinned commit). Besides 16K pages and built-in Apple drivers, it
has **`ARM64_PTR_AUTH` and `ARM64_BTI` off** — our defconfig build had
`PTR_AUTH_KERNEL=y`, `BTI_KERNEL=y` and `VA_BITS=52`. Apple A12/A13 pointer
authentication is not the architected one, so a PAC-enabled kernel is a strong
early-hang suspect too. `config_16k` lacks the USB controller itself, so the
build adds `USB_DWC3` + `USB_DWC3_APPLE` (glue, `apple,t8103-dwc3`, honours
`dr_mode = "peripheral"` from t8030-n104.dts) + `PHY_APPLE_AUSB`
(`apple,t8030-ausb-phy`) and drops the legacy `USB_G_MULTI` in favour of a
configfs gadget set up by `/init` (ACM serial + CDC-ECM, 172.16.42.1).

## Console, in one line

The DCSD cable carries the **dockchannel UART**, which m1n1 prints to but which
**cannot be driven from the booted payload** (confirmed exhaustively). The s5l
UART (`0x235200000`) is **not on the cable**. The **framebuffer works** from the
payload and is the only usable early channel; a fb earlycon was built but shows
nothing because Linux hangs *before* earlycon setup. Full story and evidence in
[console-investigation.md](./console-investigation.md). Hoolock's normal
"interaction" is actually a **USB gadget** (CDC-ACM `/dev/ttyACM0`, plus USB-net
`172.16.42.1` and a FAT16 log ramdisk) that comes up *after* Linux boots — not
an early channel, and it needs `APPLE_USBCOMPLEX=y`.

## Reference material

- [yhavry/Hoolock-Docs-A13](https://github.com/yhavry/Hoolock-Docs-A13) —
  `features/A13.md`, `tutorials/SETUP*.md`, `config_16k`.
- [yhavry/Hoolock-linux `t8030`](https://github.com/yhavry/Hoolock-linux/tree/t8030) —
  the kernel source (`kernel/build-kernel.sh` pins the commit).
- [HoolockLinux/m1n1 `idevice`](https://github.com/HoolockLinux/m1n1/tree/idevice) —
  m1n1 (`m1n1/build-m1n1.sh` pins the commit and applies our patch).

## Boot / glue recipe (reproduce)

m1n1's own SETUP_iBoot documents the same concatenation we use:

```
cat m1n1-idevice.macho <(echo 'chosen.bootargs=<cmdline>') \
    <dtb> <Image.gz> <initramfs.gz> > m1n1-linux.macho
```

Our `package.sh` does the equivalent (bootargs.txt holds the `chosen.bootargs=`
line), then wraps the result as an `rkrn` IMG4 with `ipsw img4 create
--type rkrn --compress lzss --im4m IM4M_0x8030` and fixes the LZSS container with
`repair_lzss_img4.py` (ipsw 3.1.714 appends a bad ASN.1 compression record).
m1n1 finds the appended pieces by magic (gzip `1f 8b`, FDT `d0 0d fe ed`, cpio
`07 07 0`, and `chosen.*=...\n` variables).

## 2026-10-30 evening: LINUX BOOTS TO USERSPACE on N104

With the `config_16k` kernel + cache-fixed m1n1 the device boots to the
busybox shell on the screen (simpledrm/fbcon): `Booting Linux on physical CPU
0x0 [0x611f0130]`, userspace marker, `[init] shell on console`. So the early
hang was the kernel config (4K pages / PAC+BTI / VA 52 — not isolated which).

Open issues seen on that boot:

- **cpufreq**: endless `cpufreq: __target_index: Failed to change cpu
  frequency: -5`. apple-soc-cpufreq times out on `APPLE_DVFS_CMD_BUSY`; m1n1 has
  no T8030 entry in `src/cpufreq.c` ("cpufreq: Chip 0x8030 is unsupported"), so
  the cluster DVFS is never set up. Temporarily `cpufreq.off=1` in bootargs.
  Real fix = T8030 support in m1n1 cpufreq.c.
- **SMP**: m1n1 "CPU start offset is unknown for this SoC" → CPUs 1-5 disabled
  in the FDT, Linux runs on one core.
- **USB**: no UDC. m1n1 `dt_disable_missing_devs("usb-drd", "usb@", ...)` only
  matches numbered ADT nodes (`usb-drd0`...), but the N104 ADT has a single
  unnumbered `usb-drd` (reg 0x239010000), so m1n1 disabled `/soc/usb@239010000`
  and its phy + both DARTs. Fixed in m1n1 kboot.c (also accept the unnumbered
  name). Awaiting device test.

## 2026-10-30 night: USB gadget WORKS (serial + network)

After the m1n1 `usb-drd` fix, `dwc3-apple 239010000.usb` binds in peripheral
mode, the configfs gadget enumerates on the Mac as "iPhone 11 Linux"
(1d6b:0104): ACM → `/dev/cu.usbmodemN104AP_linux1` (shell), CDC-ECM → a new
`enX` "iPhone 11 Linux" interface on the Mac (en11 here). Ping/telnet worked
over IPv6 link-local `fe80::ff:fe00:104%en11` with no Mac-side config.
Full first dmesg (2026-10-30): `first-boot-dmesg.txt`.

Working on that boot: simpledrm fb0, DARTs (16K pages, bypass support), SMC
(RTKit v12, GPIO, RTC, reboot/poweroff, power/lid input; hwmon finds no
sensors), PMIC RTC sets the clock, NVMe (ANS2 RTKit, `nvme0n1: p1`), USB DWC3
gadget. Not working: cpufreq (`cpufreq.off=1`), SMP (1 CPU), dockchannel/UART
(by design). Only `request ... was not queued to ep1in` once from dwc3 (ACM
write while the host port was closed — harmless).

**NVMe is the iPhone's internal iOS storage.** The ANS firmware logs
"uncleanReboot yes" and runs recovery/GC at every Linux boot (iOS was not shut
down cleanly). Never mount or write `nvme0n1` from Linux.

Initramfs now also has dropbear (key-only root, persistent host key in
`a13/build/ssh/`), udhcpd on usb0 (hands the Mac 172.16.42.2, no router
option), devpts for telnetd. The image carries no keys: `boot/package.sh` adds
the user's `~/.ssh/*.pub` in a second cpio archive.

Host note: Homebrew Python sockets to the link-local/USB network get
`EHOSTUNREACH` (macOS Local Network privacy); system tools (`nc`, `ssh`) work.

The boot kit is `a13/boot/` (see `a13/README.md`).

SSH verified 2026-10-30: `ssh root@172.16.42.1` → `root@n104`; the Mac got
172.16.42.2 from udhcpd. Timekeeping is healthy: clocksource
`arch_sys_counter` (24 MHz, virtual timer), `arch_timer` interrupt delivered as
AIC FIQ, uptime advances normally.

## Battery: the SMC says "OverCharged" every second

With the phone on the cable at 100 %, the SMC firmware logs
`battery.cpp:823: battery_periodic_action: Battery OverCharged: 1` once a
second (forwarded by the RTKit syslog as `dev_info`, so `loglevel=8` puts it
on the screen), and once at boot `charger.cpp:2278: Not charging:200001`.
`macsmc-power` reads: Mains online, battery Full, capacity 100, 4.472 V,
+229 mA, 23.8 °C, health Good. So the SMC has stopped charging on its own;
its protection works without Linux. What iOS normally tells the SMC about
charging (limits, inhibit keys) is not known yet — until it is, do not leave
the phone on the cable under Linux for hours, and watch the temperature.

## 2026-10-31: all six cores

`smp: Brought up 1 node, 6 CPUs` (CPU1-3 Thunder 0x611f0130, CPU4-5
Lightning 0x611f0120). Two m1n1 changes:

1. **CPU start** (`m1n1/0002-*`): m1n1 had no PMGR CPU-start offset for
   T8030. It is 0xd4000 as on A11 (+0x4 system start bits, +0x8 + 4*cluster
   core start): the ADT's `function-enable_core` on every cpu points at the
   PMGR with mask `1 << (4*cluster + core)`, and the t8030 QEMU model that
   boots iOS releases CPUs on a write to PMGR 0xd4004. IORVBAR (`cpu-impl-reg`,
   0x210050000 + 0x100000 per E-core, 0x211050000 per P-core) is locked by
   iBoot (bit 0) but to m1n1's own `_vectors_start` (iBoot points it at the
   start of the "kernelcache"), so m1n1's reset path catches the cores.
2. **CYC_OVRD** (`m1n1/0003-*`): with the cores started, Linux brought up
   CPU1-3 and then died with an Oops on an idle E-core: IABT at pc 0, lr 0,
   every GPR zero, the idle stack intact — WFI without state retention, then
   `ret` to the zeroed x30. A13 was "CPU: Unknown" in m1n1, so `init_cpu()`
   skipped the CYC_OVRD set-up (WFI mode "up", retention on, IRQ/FIQ unmasked)
   on the cores m1n1 starts; the boot CPU had iBoot's setting. A13
   Lightning/Thunder now have a `midr_parts[]` entry with
   `apple_sysregs_unlocked` and `fast_ipi` only — no chicken bits, no deep
   sleep, none of A11/M1's cache and power tweaks until they are checked.

## 2026-10-31: cpufreq — parked (`cpufreq.off=1`)

The cluster DVFS registers sit at the M1 (T8103) offsets — E-cluster
0x210e00000, P-cluster 0x211e00000; PSTATE +0x20020 (DESIRED1 [4:0],
DESIRED2 [15:12], SET 25, BUSY 31), STATUS +0x20050 (current [7:4]) — and the
PMGR's ADT feature set is M1's (`cpu-apsc`, `ppt-thrtl`, `llc-thrtl`,
`amx-thrtl`, `cpu-fixed-freq-pll-relock`). Tools in `a13/tools/peek/`: `peek`
(physical read/write through /dev/mem), `dvfs-time` (timed and stress P-state
transitions), `spin` (pinned integer loop, to see the clock).

- Transitions need no m1n1 set-up and take 5-8 us; the clock really changes
  (`spin` on CPU0: 49 / 143 / 239 M/s at E-states 1 / 3 / 5).
- **They stick.** Now and then a transition reaches its state (STATUS = target)
  but BUSY never drops; the cluster then takes no command until reset, and
  apple-soc-cpufreq returns -EIO for every request after that (the old log
  spam). The P-cluster sticks on upward steps, often on the first one; the
  E-cluster passed 5000 x 1<->5 and 2000 x 2<->4 by hand but stuck at state 4
  in all three schedutil runs. Not the cause: cpuidle's "CPU PD" (same with
  `cpuidle.off=1`), a write to the current state, concurrency between the
  clusters, the 400 us driver timeout, writing 1 to cluster +0x440f8 (what
  m1n1 does on M1).
- P-state 6 on the P-cluster froze the P-cores outright (no UART output).
- **Do not read cluster +0x40240 or +0x40250** (M1's llc/amx-thrtl): there is
  no register there on A13; the read raises an asynchronous SError and the
  kernel panics.
- No iBEC route: no iBEC keys exist for iPhone12,1; surrealra1n boots the
  patched iBSS in iBEC's place too.

Left in place: `kernel/patches/0001` takes the P-cluster out of cpufreq in the
n104 DT. Next: read what XNU does with these registers in the (unencrypted)
20E247 kernelcache.

## 2026-10-31: touch -- works, XFCE with multi-touch

The whole story (ADT, the IPSW's firmware plist, SPI1 bring-up, the HBPP
bootloader, the kernelcache reversing of power and of the HID-over-SPI
transport, the frame format, the driver) is in `touch.md`. Short version: the
controller is powered and in its bootloader when Linux starts; the driver
turns on its analog LDO (Chestnut reg 5 bit 0x10), loads the firmware from
the IPSW through the bootloader, and polls HID-over-SPI for report 0x75.
The trap that cost the most: reading with zeros on MOSI -- an all-zero packet
has a valid CRC and sends the firmware back to its bootloader.

## Open small things (2026-10-31)

- **Boot logo.** fbcon's Tux (one per CPU, `fbcon=logo-pos:center`) is not
  seen at all, even for a moment. `LOGO_LINUX_CLUT224=y`, fbcon takes over at
  0.19 s, logos are freed at 1.7 s. Dropped for now (cosmetic).
- **SMC battery log.** With the phone on the cable at 100 %, the SMC firmware
  logs `battery.cpp:823 Battery OverCharged: 1` once a second (it stopped
  charging itself; see above). Harmless, but noisy on the console.
- **Hoolock D421 vs N104.** Hoolock lists cpufreq as validated on the D421
  with the same m1n1 (no T8030 cpufreq code) and, presumably, the same
  iBSS-only entry (no iBEC keys exist for A13 iPhones), so
  whatever makes it work there is either in the kernel driver's usage pattern
  or not yet understood — worth asking them before reversing XNU.
