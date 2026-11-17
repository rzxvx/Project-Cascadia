# Kernel source audit

## 2026-11-04

Base: HoolockLinux/linux `6831bc701a6ce059e71e5aaa9488c9195bea6927`.
`devices/a12/build/linux` is a generated tree, not the source to publish.

On a clean checkout of that commit, the numbered patches applied in order
without conflicts. The result matched the old work tree except one Makefile
line for `apple_hidspi_touch.o`, which `build-kernel.sh` adds, and
`apple_hidspi_touch.c`, which is byte-identical to
`linux/extras/apple_hidspi_touch.c`. Nothing else was edited by hand.

Three read-only Bluetooth diagnostics and a failed no-flow-control patch were
then dropped, with their DTS properties. The UART pins, RTS/CTS, rates, SMC
power, wake GPIO and HCD loading stay. 20 patches remain.

| File | SHA-256 |
|---|---|
| `config_16k` (Hoolock docs) | `a5cbcb0d3fe655888106672e6fe03477b63d51bd36bbcfc4f5145627d906b208` |
| `build/kernel.config` | `c24bdb11af73db206129ef78e807ab7db2ad09911728421e353c6b3621de6d50` |
| `Image.gz`, before the drop | `4667bc3401a871797f5b400ce6fd7ac83798e092c467ca828a3efb2413d9f7c7` |
| `Image.gz`, after the drop | `b98b87ec9c5ba0e4136d3eeb24495d95d4dada34d43fc87403c6a99e7fc8e57f` |

The second kernel booted Debian with XFCE, touch, Wi-Fi and Bluetooth on the
phone.

## 2026-11-08

`linux-0020-agx.patch` is badapple's current GPU driver. The series still
applies cleanly to 6831bc7. `CONFIG_DRM_AGX` stays off, so it adds no code to
the kernel.

Patches 0001 to 0014 and 0020 are byte-identical to badapple's.
