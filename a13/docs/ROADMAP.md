# Roadmap — iPhone 11 (N104AP, A13)

Same order as the iPad mini 1. Each step notes what already exists (Hoolock
`t8030`, upstream / Asahi) so the effort goes where the gap is.

## Phase 0 — Preparation ✓

- [x] Pwned DFU via usbliter8.
- [x] N104AP ADT decoded (hardware map below).
- [x] m1n1 (`HoolockLinux/m1n1` idevice) builds on macOS; kernel builds in Docker.
- [x] The boot sequence after pwned DFU: iBSS → Recovery → `rdtr` DeviceTree →
      empty `rtsc` trust cache → m1n1 as `rkrn` → `bootx`.

## Phase 1 — Linux boots to a shell ✓

- [x] m1n1 runs on A13 (CTRR read fixed for T8030).
- [x] The hand-off executes (payload cleaned to PoC before the caches-off jump).
- [x] Linux boots: Hoolock's `config_16k` (16K pages, no PAC/BTI) instead of
      `defconfig`.
- [x] Framebuffer console (simpledrm), early text on the framebuffer
      (`earlycon=applefb`).

## Phase 2 — Usable from the Mac ✓

- [x] USB gadget: CDC ACM shell, CDC ECM network, SSH (m1n1 kept disabling
      the controller; fixed).
- [x] Working tick (arch timer), wall clock from the PMIC RTC.
- [x] A root file system beyond the initramfs: Alpine 3.24 over NFS from the
      Mac, `apk` through the Mac's NAT.
- [x] One host command to boot: `a13/cascadia` (doctor, build, pack, boot,
      nfs, net, release), macOS and Linux.

## Phase 3 — Hardware, in A5 order

- [x] All six cores: PMGR CPU start at 0xd4000 in m1n1 for T8030, A13
      Lightning/Thunder in m1n1's `midr_parts[]` (CYC_OVRD; chicken bits
      still unknown).
- [ ] cpufreq — **parked** (`cpufreq.off=1`). The cluster DVFS works at M1's
      offsets and needs no m1n1 set-up, but transitions sometimes never drop
      BUSY and the cluster then hangs until reset (P-cluster on upward steps,
      E-cluster under schedutil). Next: what XNU does with these registers, from
      the unencrypted 20E247 kernelcache. Details: `research/bringup-status.md`.
- [x] Touch: `spi1` (`spi-1,samsung`, our `spi-apple-s5l`), the controller's
      firmware from the IPSW (`N104_Multitouch.im4p`) loaded through its HBPP
      bootloader, Apple's HID-over-SPI (iOS's AppleHIDTransportSPI, reversed),
      report 0x75 -> input-mt; analog LDO on the display PMU Chestnut.
      Driver `kernel/extras/apple_hidspi_touch.c`, notes `research/touch.md`.
      Polled: the AOP GPIO interrupt line does not move yet.
- [x] Desktop: XFCE on simpledrm with the touchscreen (udev in the root).
- [x] On-screen keyboard: fbkeyboard on the screen console (periphery asks).
- [x] No cable needed after the boot: the Alpine root in RAM (an iBoot
      ramdisk), Wi-Fi with its tools in the image.
- [x] Wi-Fi: BCM4378 over PCIe (`apcie,t8030`, brought up by our m1n1 patch
      from the ADT's tunables), powered through the SMC (`gP11`), brcmfmac;
      firmware + NVRAM from the IPSW, MAC and calibration from syscfg; the
      `wifi` command. L1 ASPM off for now (`pcie_aspm.policy=performance`).
- [x] Internal storage: all of NVMe namespace 1 for Linux (iOS erased, asked),
      one ext4 root, booted before NFS and RAM; a write gate in the NVMe driver
      keeps the other namespaces unwritable. (Side by side with iOS: planned,
      not done -- see research/nvme-plan.md.)
- [ ] Native display: `disp0` + Synopsys MIPI DSIM + panel "pinot" + display
      PMU "chestnut".
- [ ] Backlight (LM3539, I2C), battery, charging (SMC).
- [x] Bluetooth: BCM4378 PCIe, `hci_bcm4377` (hci0; nothing on top yet).
- [ ] Audio: cs42l77 codec + cs35l27 amp over MCA.
- [ ] Buttons, haptics.
- [ ] GPU (AGX G12): started 2026-11-01 -- firmware interface = Asahi's `V13_3`
      (iOS 16.4's iBoot is macOS 13.3's), the iOS driver observed through
      Dopamine; firmware structures dumped and parsed, the firmware loaded
      by iBoot under Linux (G1-G3); **2026-11-03 (G4): the GPU powers up
      and its firmware runs under the m1n1 proxy** once the PMGR voltage
      manager's GPU channel is handed to the hardware (VOLMAN +0x14 bit 0,
      which iBoot leaves set); next G5, a first job. Plan G1-G7 in
      `research/gpu.md`.

## Phase 4 — For non-experts

- [ ] One installer: fetches the IPSW pieces from Apple, makes the inputs
      locally, boots Linux. Nothing from Apple shipped.
- [ ] Boot from internal storage after entry.

## Parked

- Cameras, baseband, GPS, sensors, SEP.

## Hardware map (from the N104AP ADT)

Addresses relative to the arm-io window at 0x2_0000_0000.

| Block | ADT node / compatible | Linux |
|---|---|---|
| Debug UART | `uart0` `uart-1,samsung` @ 0x35200000 | driver present; not on the DCSD cable |
| Interrupts | `aic,1` | AIC v1 ✓ |
| Power/clocks | `pmgr1,t8030` | ✓ |
| PMU | `pmu,spmi` / `pmu,avus` on `spmi,t8015` | RTC ✓ |
| SMC | `smc-pmu`, RTKit | ✓ (RTC, reboot, poweroff, input) |
| Storage | ANS2 + `sart,t8030` | detected (holds iOS) |
| USB | `usb-drd,t8030` + `atc-phy,t8030`, Lightning via `tristar,cbtl1612` | gadget ✓ |
| Display | `disp0,t8030` → `mipi-dsim-1,synopsys` → panel `lcd,pinot` | simpledrm on iBoot's framebuffer |
| Display power | `display-pmu,chestnut` (I2C2 @0x27) | LDO 2 for touch ✓ |
| Backlight | `backlight,lm3539` (I2C) | — |
| Touch | `multi-touch` on `spi1`, HID over SPI, HBPP fw `wftm`, cal `syscfg/MtCl` | ✓ (`apple_hidspi_touch`) |
| Wi-Fi / BT | `wlan-pcie,bcm4378` / `bluetooth,n88` on `apcie,t8030` | — (the M1 chips) |
| Audio | `cs42l77`, `cs35l27`, `mca,t8030` | — |
| GPU | `gpu,t8030` (AGX G12) + `gfx-asc` (RTKit) | powers up, firmware runs under m1n1 (`research/gpu.md`) |
