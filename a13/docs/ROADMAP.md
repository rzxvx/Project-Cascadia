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

## Phase 2 — Usable from the Mac (mostly ✓)

- [x] USB gadget: CDC ACM shell, CDC ECM network, SSH (m1n1 kept disabling
      the controller; fixed).
- [x] Working tick (arch timer), wall clock from the PMIC RTC.
- [x] A root file system beyond the initramfs: Alpine 3.24 over NFS from the
      Mac, `apk` through the Mac's NAT.
- [ ] One host command to boot (the `./cascadia` of this device).

## Phase 3 — Hardware, in A5 order

- [ ] All six cores: CPU start in m1n1 for T8030, A12/A13 in m1n1's
      `midr_parts[]` (chicken bits).
- [ ] cpufreq: cluster DVFS set-up in m1n1 for T8030 (the kernel driver exists).
- [ ] Touch: SPI HID on `spi1`, HBPP firmware (`wftm`), MtCl calibration. Closest
      to the A5 Z2 work; no Linux driver exists.
- [ ] On-screen keyboard, desktop (XFCE on simpledrm first).
- [ ] Wi-Fi: BCM4378 over PCIe (`apcie,t8030`), brcmfmac; firmware + NVRAM from
      the IPSW, MAC from syscfg.
- [ ] Internal storage: iOS's files read-only, then a Linux partition of its own.
- [ ] Native display: `disp0` + Synopsys MIPI DSIM + panel "pinot" + display
      PMU "chestnut".
- [ ] Backlight (LM3539, I2C), battery, charging (SMC).
- [ ] Bluetooth: BCM4378 PCIe, `hci_bcm4377`.
- [ ] Audio: cs42l77 codec + cs35l27 amp over MCA.
- [ ] Buttons, haptics.

## Phase 4 — For non-experts

- [ ] One installer: fetches the IPSW pieces from Apple, makes the inputs
      locally, boots Linux. Nothing from Apple shipped.
- [ ] Boot from internal storage after entry.

## Parked

- GPU (AGX G12): after Phase 3.
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
| Display power | `display-pmu,chestnut` | — |
| Backlight | `backlight,lm3539` (I2C) | — |
| Touch | `multi-touch` on `spi1`, HID over SPI, HBPP fw `wftm`, cal `syscfg/MtCl` | — |
| Wi-Fi / BT | `wlan-pcie,bcm4378` / `bluetooth,n88` on `apcie,t8030` | — (the M1 chips) |
| Audio | `cs42l77`, `cs35l27`, `mca,t8030` | — |
| GPU | `gpu,t8030` (AGX G12) | parked |
