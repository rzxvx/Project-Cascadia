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
  the console to 128x48. A usable graphical desktop has not been verified.
- `/proc/mounts` contains only rootfs and pseudo/RAM filesystems.

Touch, Wi-Fi, USB networking, audio, suspend, CPU1, persistent storage and a
graphical interface are **not verified on K93**. Do not inherit P105 support
claims for these devices. The test session intentionally resets after 105s.

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
