# Experimental iPad 2 Wi-Fi (K93AP) RAM boot

Hardware tested: iPad2,1, S5L8940 / CPID 8940, BDID 04, K93AP.
This is an initial bring-up, not parity with the P105 port or an installer.
No Apple firmware binaries, device identifiers, credentials, or personal logs
are included. This workflow does not mount or write internal storage.

## Verified on hardware

- Linux 6.12 reaches the embedded `/init` and an interactive root shell over
  CDC ACM (`/dev/ttyACM0` on the Linux host).
- USB gadget enumerates as 0525:a4aa; USB interrupts reach the CPU.
- CPU MPIDR is 0x80000900, not the P105 CPU affinity.
- The watchdog counter at physical 0x3f103020 is a single 16-byte bank running
  at 24 MHz. The existing read-only platform clock code measures CPU frequency
  at approximately 1 GHz and registers the counter as clocksource/delay timer.
- The existing AIC CPU0 timer reports a successful 24 MHz clockevent self-test.
- simplefb registers 1024x768, 32 bpp, stride 4096 at 0x9f6fc000 and switches
  the console to 128x48. GUI4 also rendered a native Xorg/TWM/xterm desktop
  through fbdev; touch and everyday desktop usability remain unverified.
- `/proc/mounts` contains only rootfs and pseudo/RAM filesystems.
- GUI3 now also boots a complete Alpine3.24.2 ARMv7 root as read-only
  SquashFS plus bounded RAM overlay. A small boot payload avoids the observed
  Recovery transfer cutoff; the separate hash-pinned image is pushed over
  direct USB IP, with ACM kept closed during that transfer. Guest hash,
  mount/memory proof and native Xorg/xterm **version execution** are recorded
  in local `build/k93/bundle-gui-v3/root-verified.json`. About387MiB was
  available before starting graphics. Version execution alone is not evidence
  of rendering; the separate GUI4 framebuffer capture below provides that.

Direct USB IP networking is now verified by repeated scoped host/guest ping
tests (V41–V44), without Internet routing. Touch, Wi-Fi, audio, suspend, CPU1,
persistent storage and browser usability are **not verified on K93**.
Do not inherit P105 support claims. Current touch diagnostic sessions reset
after120s; the original V8 session described below used105s. See
the GUI4 evidence section below for the newer userspace result.

## GUI4 native framebuffer desktop (2026-10-18)

On physical K93 hardware, Alpine 3.24.2 ARMv7 ran from a read-only SquashFS
image with a 64 MiB RAM overlay, 96 MiB `/run` and 32 MiB `/tmp`. Xorg
1.21.1.25, TWM and xterm 410 started successfully. A read-only `/dev/fb0`
capture was decoded and visually inspected: the TWM title bar, terminal text
and shell prompt were rendered at 1024x768. This is software framebuffer
graphics, not GPU acceleration or a usable touch/browser demonstration.

The guest diagnostic sources are `rootfs/k93/{squash-init,alpine-init,
desktop-test.sh,xorg-fbdev.conf,twm-test.conf}`. The desktop is manual and
bounded: Xorg has a 35-second limit, clients 30 seconds, private MIT-cookie
authentication and TCP listening disabled. Input auto-discovery is disabled;
the diagnostic terminal runs as root, not as a production browser account.
`tools/k93-fb-read.c` accepts only the verified K93 framebuffer mode and
performs read-only GET ioctls/pixel reads. The decoder requires successful
test/cleanup markers and explicitly requires human/visual review afterward.

Evidence digests from this run:

- Full root image: 67,293,184 bytes, SHA256
  `d7413f2ca57ca2629e624b44e73b6e00d54d889527252eb40b11fdf711fc4d5f`.
- Boot bundle: 11,272,707 bytes, SHA256
  `7a55dbc59c9273ec51c56963c1ba5cf3656a203b5e70ec4ecec0baff7e3c9808`.
- Captured RGB PPM SHA256:
  `10da2714202408d956b5f142643b947615beb9a48421676ea5da7a1a1666a49b`.
- Converted PNG SHA256:
  `07b6cbc65246fbd7c254140b4a6ef570e47aa72749ca8f981842d3ffa304b623`.

All three Recovery uploads reached 100%. Linux USB appeared after 3.321s.
The separate root image was pushed over direct USB IP; the guest checked its
hash before mounting. Keep ACM closed during the bulk ECM transfer: an
earlier overlapping capture stalled the transfer. Root-transfer TCP output
drained before host network cleanup. Subsequent USB ping returned 4/4 packets,
zero loss, average 0.320ms. The watchdog reset at 120.117s; iOS USB returned
at 183.419s, and a new boot with automatic root access was verified afterward.
No internal storage was mounted or written.

The observed Recovery transfer cutoff makes embedding this full image unsafe;
the tested split-image bootstrap receives only on guest USB 10.93.0.2:48932,
with host peer 10.93.0.1. It does not route Internet traffic. The image builders
and diagnostic sources are published here; the experiment-specific kernel
configuration, device-selection/transport harness and jailbreak credentials
are not. This is not a turnkey reproduction recipe or persistent installer.
The original diagnostic build recipe below remains a separate baseline.

Host-only checks (no device or mount access):

```sh
TMPDIR=/var/tmp python3 tools/k93-test-fb-read.py
python3 tools/k93-test-desktop-frame.py
python3 tools/k93-test-squash-init.py
python3 tools/k93-test-alpine-root.py
```

## Important calibration dependency

Without the `apple,s5l8940x-wdt-timer` node, the watchdog clock/delay timer is
not registered. The PMCCNTR fallback skips registration in an SMP build, even
with `maxcpus=1`. Generic delay calibration then waits for ticks before the
later AIC timer initialization can provide them. Hardware trace stopped inside
`calibrate_delay()` in this configuration. Adding the verified counter node
allowed calibration to be skipped using its frequency and userspace to boot.

## Reproducing the tested diagnostic build

Use the upstream toolchain and patched Linux tree at `build/linux`; run build
commands in the same environment/path namespace (e.g. the project's container).
The Python preparation step needs network access for its SHA256-pinned official
Alpine BusyBox package. Firmware extraction also needs PyCryptodome.

```sh
python3 tools/k93-prepare-root.py
CONFIG_FRAGMENT="$PWD/build/k93/k93.config" \
INITRAMFS_DIR="$PWD/build/k93/rootfs" bash scripts/build-kernel.sh
bash tools/k93-build-kernel-trace.sh --late-stages
python3 tools/k93-build-bundle.py --counter-clock
```

Output is `build/k93/bundle-diag-v8-wdt120/`, with a hash manifest. The bundle
contains the loader, padding to the kernel entry, traced zImage, and appended
DTB. The diagnostic loader uses stack 0x80007000, below the kernel at
0x80008000, and the verified physical watchdog base (not the P105 default).

The optional trace mechanically instruments the ignored kernel tree using
unique, prevalidated anchors. It preserves upstream source outside the selected
sites; repeat application is idempotent. It adds temporary identity mappings
for diagnostics and must not be treated as a production MMU configuration.
Late C checkpoints set watchdog reset deadlines to 30/45/60/75/90/105 seconds:
post-console check, before/after delay calibration, before `rest_init`, before
first schedule, and init task before `kernel_init_freeable`, respectively.
These are absolute counter deadlines, not delays at each checkpoint.

## Boot chain

`tools/k93-extract-firmware.py` reads a user-supplied stock
`iPad2,1_8.4.1_12H321_Restore.ipsw` in the repository root. It uses public
firmware decryption material and validates the board. Then:

```sh
python3 tools/k93-extract-firmware.py
python3 tools/k93-build-bootchain.py
```

The latter needs upstream `iBoot32Patcher` and builds into
`build/k93/bootchain-v2`. Its iBEC trampoline sits in preserved text alignment
padding, not the file tail/BSS (which startup erases). Layout, hook bytes,
relocation survival, changed-byte scope and plaintext IMG3 packing are checked.
The plaintext iBEC is for an already exploited kDFU boot chain; stock DFU is
not sufficient. Establishing kDFU is deliberately outside these offline tools.

The tested upload sequence is patched iBEC, wait for Recovery, staging bundle,
then the matching standalone loader if Recovery remains responsive. Select
and verify the exact device before every transfer. Do not run a restore or
flash persistent storage. Open the CDC ACM console promptly: the diagnostic
watchdog still resets the machine at its last checkpoint deadline.

Tested V8 bundle SHA256:
`db114e51c9d2acf1ae3153f68b0ab31b7d3f1d61ab38423fd1f101be320e4281`.
Build timestamps can change the kernel/bundle hash on a rebuild.

## Controlled reboot from a working K93 RAM session

The local `pongo/k93_user_watchdog.S` helper is a tiny static Linux ARM ELF,
built offline by `bash tools/k93-build-user-watchdog.sh`. It first validates
the compatible string `apple,k93ap`, maps only the verified watchdog page,
requires CTRL=0 and checks that CUR_TIME advances. With no arguments it is
read-only and prints `K93_WDT_AUDIT_OK`. Only explicit `--reset` arms a
five-second reset: BITE_TIME=current+120000000, then CTRL=4. It never resets
CUR_TIME, never accesses storage, and refuses deadline addition overflow.

Exit codes:2 invalid arguments,3 wrong/missing board,4 mapping failure,
5 unexpected control state/readback,6 stopped counter,7 impending counter
wrap. Do not bypass these checks or use this helper on another board.
Run the read-only audit first and retain a recovery path. The reset mode was
tested on the live V11 K93 session: it returned `K93_RESET_ARMED_5S`, followed
by USB disappearance and normal iOS enumeration without button presses.
This does not itself prove that untether/root recovery completed.

Validated build:816 bytes, SHA256
`956994bb6482309446f63a83e8a3aecac67aa35e5c51138637da665b246f4c36`.
It can be transferred to `/tmp` over the known USB shell using BusyBox base64;
never write it to internal storage. Invalid-argument and wrong-board guards
were checked with host-side ARM emulation; MMIO/reset execution was only on K93.
