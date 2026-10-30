# How to get early kernel logs on N104 — what works and what doesn't

The hardest part of this bring-up was not booting Linux (it executes), it was
*seeing* it. This records what was tried so nobody repeats it. Summary:

- **dockchannel UART** (`0x23d128000`): m1n1's console, on the DCSD cable, but
  **cannot be driven from the booted payload**. Dead end.
- **s5l UART** (`0x235200000`, `apple,s5l-uart`): the kernel's DT console, but
  **not on the DCSD cable**. Not observable.
- **framebuffer** (`0x8fa298000`): **works** from the payload. Only usable early
  channel. A fb earlycon was written; it is silent only because Linux currently
  hangs before earlycon setup.
- **USB gadget** (CDC-ACM `/dev/ttyACM0`): Hoolock's real interaction path, but
  it comes up only *after* Linux boots and needs `CONFIG_APPLE_USBCOMPLEX=y`.

## Proving the payload executes (framebuffer stubs)

Bare-metal arm64 stubs in `a13/tools/fb-stubs/*.S`, built with
`clang --target=aarch64-unknown-none -c`, `ld.lld --image-base=0 -Ttext=0`,
`objcopy -O binary`, padded to 0x1000, gzipped, and dropped in as the "kernel"
payload (m1n1 recognises them by the arm64 Image header: `b real` at 0, magic
`ARM\x64` at 0x38, `image_size` at offset 16). fb format is **x2r10g10b10**
(R[29:20] G[19:10] B[9:0], white = `0x3fffffff`), stride `0xd00`, 828×1792.

- `fb_white_stub.S` fills the whole fb white. **Screen goes white** with the
  real (MMU-off, `mmu_shutdown`) handoff → the payload executes in the exact
  environment Linux boots in, and the m1n1 cache patch (m1n1-patches.md) works.
- `fb_dock_stub4.S` fills green, blasts `!` to the dockchannel, fills blue.
  **Screen goes green→blue but no `!` on serial** → the payload runs end to end,
  but its dockchannel writes never reach the host.

## Why dockchannel can't be driven from the payload

Confirmed against the real drivers (web-fetch of AsahiLinux/m1n1,
aurora-silicon/u-boot `serial_apple_dockchannel.c`, aurora-silicon/linux
`apple_dockchannel_uart.c` + its earlycon — all credited to Yureka Lilian). The
protocol we used is **exactly right**: data block = reg0 + 0x4000, **TX8 = +0x04**
(byte in bits [7:0]), **TX_FREE = +0x14** (0 = full), RX8 +0x1c, RX_COUNT +0x2c;
**no enable/init writes** anywhere; real drivers only add a *bounded* TX wait and
drop on timeout (m1n1 spins forever — the known "FIFO-full hang").

Despite correct addresses/protocol, from the payload:

- polled, unpolled, and rate-limited TX8 writes all produce **nothing** on the
  host, while the code keeps running (fb colour advances past the writes);
- it is **not** m1n1's pre-jump shutdowns: a diag m1n1 that skips
  `usb_iodev_shutdown`/`display_shutdown`/`fb_shutdown` still produced no `!`;
- a bounded dockchannel earlycon in Linux (`a13/tools/fb-stubs/apple_dockchannel_earlycon.c`,
  10000-retry drop) is also silent on a real cache-fixed boot.

m1n1's own writes to the same registers work; the payload's don't, for a reason
we never pinned down (the u-boot header says dockchannel "forwards bytes to a
debug host through the USB-C port controller" — some host-side crediting or
transport state only m1n1 maintains). **Conclusion: do not use dockchannel for
the kernel console on this device.** The driver is in `a13/tools/fb-stubs/` and
harmless (compiled in, unused) if we ever revisit it.

## s5l UART

The Linux DTB has `serial@235200000` (`apple,s5l-uart`), `stdout-path = serial0`,
and the kernel has the standard `apple_s5l` earlycon (`OF_EARLYCON_DECLARE(s5l,
"apple,s5l-uart", …)` in `samsung_tty.c`). But m1n1 prints via dockchannel and
the user *sees* that on the single DCSD serial port — so the cable carries
dockchannel, not s5l. s5l output would go to a UART that isn't wired out. The
node was set `status=disabled` in the test DTB to avoid any probe issues; irrelevant
either way. (`ls /dev/cu.*` showed only one `usbserial-*` plus a
`usbmodem1101`; the usbmodem is the USB-gadget/CDC path, not an early UART.)

## The framebuffer earlycon (built, currently silent)

`a13/kernel/extras/apple_fb_earlycon.c`, `earlycon=applefb`. Renders the kernel log
as text into the fb with the kernel's `font_vga_8x8` (needs
`CONFIG_FONTS=y` + `CONFIG_FONT_8x8=y`), `early_ioremap`-ing a ~192KB window
(~7 text rows, wraps). Text is drawn synchronously per `printk`, so whatever was
printed before a hang stays frozen on screen.

On the current (wrong-config) kernel it shows **nothing**, which tells us Linux
hangs **before `parse_early_param()`** — earlycons are set up there, so a hang in
`head.S` or the first half of `setup_arch` produces no earlycon output on any
channel. This is consistent with the kernel-config root cause in
[bringup-status.md](./bringup-status.md): fix the config first. If a correct
(16K, Apple-drivers-builtin) kernel still hangs pre-earlycon, the next move is a
fb colour-marker patched into `start_kernel`/`setup_arch` milestones (only needs
a small `early_ioremap` window and no font) to bisect the hang, or an even
earlier marker written in `head.S` during the brief MMU-off window.
