# Kernel (N104AP / t8030)

**Source:** [`yhavry/Hoolock-linux`](https://github.com/yhavry/Hoolock-linux/tree/t8030)
branch `t8030`, pinned in `../kernel/build-kernel.sh`. It carries
`arch/arm64/boot/dts/apple/t8030-n104.dts` and the t8030 drivers. Not
`hoolocklinux/linux` `hoolock` (the A7-A11 tree, no t8030).

**Config:** Hoolock's A13 `config_16k` (fetched at a pinned commit, checked by
sha256) + `../kernel/n104.fragment` (DWC3 + Apple glue + AUSB PHY, configfs
gadget functions, simpledrm/fbcon, the 8x8 font for the framebuffer earlycon).
Do not start from `make defconfig`: 4K pages, PAC/BTI in the kernel and the
Apple drivers as modules, and it hangs in early boot
(`research/bringup-status.md`).

**Build:** `bash a13/kernel/build-kernel.sh`. The macOS file system is
case-insensitive, so the tree is cloned into a Docker volume and built in an
arm64 Ubuntu 24.04 container (clang 18; the kernel needs clang >= 17). Native
on Apple silicon, a few minutes. Output: `a13/build/payload/{Image.gz,
t8030-n104.dtb}`, the final config in `a13/build/kernel.config`.

**Our additions** (`../kernel/extras/`): `apple_fb_earlycon.c` —
`earlycon=applefb` draws the kernel log on iBoot's framebuffer (middle of the
screen, clear of the notch) before simpledrm exists. Use with `keep_bootcon`.
