# Cascadia A12 — iPhone XR and iPad 8 Wi-Fi

Experimental, contributed Linux support for two **distinct** T8020 boards:

| Profile | Device | Board | Boot firmware baseline |
|---|---|---|---|
| `xr` | iPhone XR / iPhone11,8 | N841AP, BDID `0x0c` | iOS 18.7.10 / 22H374 |
| `ipad8` | iPad 8 Wi-Fi / iPad11,6 / A2270 | J171aAP, BDID `0x24` | iPadOS 17.1.1 / 21B91 |

**Start with [QUICKSTART.md](QUICKSTART.md).** It is the single current walkthrough
for device selection, firmware preparation, Debian/NFS, builds, packaging and boot.

```sh
./cascadia a12 --device xr doctor
./cascadia a12 --device ipad8 doctor
```

XR remains the default when no device is selected. Select explicitly when using
both devices. An environment equivalent is `A12_DEVICE=xr` or `A12_DEVICE=ipad8`.

## What has actually been tested

Earlier laboratory payloads reached Debian/XFCE over USB NFS with framebuffer,
touch, Wi-Fi and UART Bluetooth on both boards. J171 also demonstrated the
login-screen keyboard/password flow. These are contributor reports, not a
promise of support or maintainer hardware testing.

The separated source workflow subsequently built a shared kernel/m1n1 and
board-specific payloads, booted both XR and iPad, and passed automatic USB/SSH/NFS
reconnect tests with the corrected A13-style handoff (now the default). The iPad
reused the freshly built XR Debian root; this does not validate a separate fresh
iPad-profile root build. Repeated iPad reconnects passed, but an earlier timeout
means intermittent recovery is not ruled out. Latest tests confirmed radio
interfaces/controllers, not complete association/pairing or stability coverage.
See [rootfs/PROFILES.md](rootfs/PROFILES.md) for lifecycle and test evidence.
This remains an experimental contributed port, not a fully validated release.

The tested host was an Android phone running Kali NetHunter with a custom kernel
supporting an NFS server. This is a test environment, not a requirement for using
the port. Native Linux ARM64 is the documented build/rootfs baseline; other host
architectures require additional toolchain/emulation setup.

## Design

- Small initramfs plus a separate host-exported Debian desktop; do not embed a
  desktop or Wi-Fi/Bluetooth firmware into the kernel payload.
- Separate per-device SysCfg, Apple inputs, DT/initramfs, manifests and boot logs.
- One shared kernel/m1n1 source/build tree. A second desktop root is **not**
  created by selecting another device.
- Pristine Apple DeviceTree for iBoot, and a separate board-specific Linux DTB.
- RAM-only boot transfers. No iOS restore/downgrade or internal-storage writes
  are part of this procedure.
- A12-owned, hash-recorded copies of its former A13 build dependencies.

## Reference documents

- [QUICKSTART.md](QUICKSTART.md) — current command sequence and prerequisites.
- [DISPLAY.md](DISPLAY.md) — iPad native scanout, brightness and desktop setup.
- [FIRMWARE.md](FIRMWARE.md) — IPSW sources, SysCfg, radio vendors and extraction.
- [DEBIAN_XFCE_NFS.md](DEBIAN_XFCE_NFS.md) — root reuse, networking and recovery.
- [DEVICE_SEPARATION.md](DEVICE_SEPARATION.md) — paths, guards and refactor limits.
- [rootfs/PROFILES.md](rootfs/PROFILES.md) — accounts, greeter and RAM watchdog.
- [boot/README.md](boot/README.md) — generated boot files and upload order.
- [vendor/README.md](vendor/README.md) — frozen dependency provenance.
- [LINUX_PLAN.md](LINUX_PLAN.md) and [boot/IPAD8_RADIO.md](boot/IPAD8_RADIO.md) —
  chronological engineering evidence; older paths/commands there are historical,
  not an alternative current installation guide.

Apple binaries, tickets, factory identifiers/calibration, private keys, generated
payloads, desktop roots and logs stay local and ignored by Git.

## Merged XS Max support

The contributed D331p (`iPhone11,6`) path from kieroid is selected with
`./cascadia a12 --device xsmax COMMAND` or the compatible `A12_BOARD=d331p`.
XR remains the default; conflicting selectors are rejected. Inputs, SysCfg,
DTB, m1n1, initramfs, NFS settings and bootsets remain per device.

The XS Max uses BadApple's m1n1 patch series, `--any-im4m` iBSS patching,
D331p touch operations, embedded BCM4377 PCIe radios and pinned iOS 16.6.1
GPU firmware. It does not require SysCfg to package its shell boot. Without
an NFS configuration it boots its initramfs shell. `boot radios` is for
XR/iPad external firmware loading; XS Max uses `boot` instead.

For XS Max, `periphery /path/to/16.6.1.ipsw` extracts radio firmware.
The shell/SSH/display/touch/radio/GPU-node result is reported by kieroid;
this merged profile has not yet been hardware-tested here. The later iPad GPU/native-display integration is documented in
[GPU.md](GPU.md) and [DISPLAY.md](DISPLAY.md); it is not an XS validation result.

On non-arm64 hosts, initramfs dropbear and libraries are taken from Debian
arm64 packages. BlueZ host-copy bundles still require arm64. Native Linux
m1n1 builds are available with `M1N1_DOCKER=0`.

## Credits


- **dr1408**: the XR port: N841 DTS, the HBPP touch driver, the BCM4357 and
  N88 radio patches, and the boot, NFS and firmware scripts
- **kiki**: [badapple](https://forgejo.toe.quest/kiki/badapple), the iPhone XS
  Max port: the T8020 DTS and PMGR, Linux patches 0001 to 0014, the GPU
  driver 0020, and the T8020 m1n1 work (DWC2, chicken bits) the XR m1n1
  series builds on
- **Hoolock Linux**: [linux](https://github.com/HoolockLinux/linux),
  [m1n1 `idevice`](https://github.com/HoolockLinux/m1n1) and `config_16k`
- **Asahi Linux**: m1n1, and the Apple silicon drivers Hoolock builds on
- **Corellium**: Project Sandcastle's `hcdpack`, which builds the Bluetooth
  HCD
- **Pauli1Go**: the iPhone 7 Bluetooth notes that pointed at the full UART
  pinmux and RTS/CTS
- **Paradigm Shift**: usbliter8
- **blacktop**: [ipsw](https://github.com/blacktop/ipsw)

GPU build/firmware/Mesa instructions: [GPU.md](GPU.md). The iPad built-in GPU path has passed boot, pixel-readback and temporary GPU-desktop tests; see the documented limits.

The iPad profile selects guarded native H11P scanout and SPMI/SMC brightness
with writes enabled by default and an inherited-brightness cap. A normal-profile
boot verified automatic AGX desktop selection and the user confirmed its slider.
The old 4096-job render stop has been removed and host-tested, but that fix still
needs a fresh hardware endurance test. See [DISPLAY.md](DISPLAY.md) and
[GPU.md](GPU.md) for evidence, rootfs setup and limits.
