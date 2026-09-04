# Boot handoff: getting these artifacts onto the iPad

## Read this first: pongoOS does not run on the A5

The plan this port was written against assumes a pongoOS module loads the
kernel. That will not work as written, and it is better to know now than after
building a rootfs.

**pongoOS is AArch64.** It is checkra1n's pre-boot environment for A7 through
A11, written for 64-bit Apple SoCs, and it has no 32-bit ARM build. The A5 in
iPad2,5 is a Cortex-A9 — ARMv7, 32-bit. There is no version of pongoOS to load
here.

checkm8 itself is a separate question from pongoOS, and a happier one: the
bootrom exploit does cover S5L8940X/S5L8942X, and checkra1n shipped A5 support
in its later betas. What differs is what runs *after* the exploit. On A7+ that
is pongoOS; on A5 checkra1n uses a 32-bit payload that patches and continues
into iBoot. There is no equivalent 32-bit interactive shell with a `bootux`
command to borrow.

So the missing piece for this project is a small 32-bit DFU payload that
uploads four blobs and jumps. `loader.S` in this directory is the "and jumps"
part, written out completely; the upload transport is the part still to build.

None of this affects the DTB or the kernel. The ARM Linux boot protocol is the
same regardless of who satisfies it, and everything in `dtb/` and `build/out/`
is equally valid whether a pongoOS module, a custom DFU payload, or `kloader`
running under iOS does the handoff.

## What you actually need

| Piece | Status |
|---|---|
| checkm8 code execution on S5L8942X | Pi Pico dongle or `ipwndfu` on a host; **unverified here** |
| 32-bit USB upload stub | not written — the real remaining work |
| Register/cache handoff | `pongo/loader.S`, builds to a 500-byte flat binary |
| Kernel, DTB, fixups | done; see the repository root |

### On the Raspberry Pi Pico dongle

The task brief asked me to confirm the OS can be loaded with "a basic checkm8-a5
Raspberry Pi Pico dongle". I cannot confirm that, and I want to be precise about
why rather than leave it implied:

- There is no iPad and no dongle attached to this build host, so nothing in this
  repository has been run against hardware.
- RP2040-based checkm8 dongles do exist and work by acting as a USB host that
  drives the DFU exploit, which removes the flaky-libusb problem you get running
  `ipwndfu` from a desktop. That much is real.
- Whether your specific dongle's firmware includes the S5L8942X target is a
  per-firmware question. Many builds cover only the A7–A11 range that checkra1n
  supports, because that is what most users want.

The check is quick: put the device in DFU, run the dongle, and see whether the
USB serial number string gains a `PWND:[checkm8]` suffix. That, and only that,
tells you the A5 target is supported. Everything downstream depends on it, so do
this before writing any more code.

## Memory map

These addresses appear in three places — `loader.S`, `scripts/gen_p105ap_dts.py`
and here. They must agree.

| Address | Contents | Size |
|---|---|---|
| `0x80000000` | DRAM base | 512 MiB, to `0xa0000000` |
| `0x80008000` | `zImage` | ~11 MiB, self-relocating |
| `0x87000000` | `loader.bin` | 500 bytes |
| `0x88000000` | initramfs (gzip/lz4 cpio) | budget 128 MiB |
| `0x90000000` | `p105ap.dtb` | ~4 KiB |
| `0x9fc00000` | iBoot framebuffer | do not overwrite |

`0x80008000` is DRAM base plus the conventional 32 KiB `TEXT_OFFSET`. The
decompressor relocates itself if the compressed image would land on top of the
decompressed one, so the exact size does not matter.

The DTB sits above the initramfs rather than below it so that growing the
ramdisk does not silently overwrite the device tree — the failure mode there is
a hang with no output, which is miserable to debug.

## Register state at the jump

From `Documentation/arm/booting.rst`:

```
r0 = 0
r1 = ~0            (device-tree boot; no machine ID)
r2 = 0x90000000    (physical address of the DTB)
CPU in SVC mode, IRQ and FIQ masked
MMU off, D-cache off, I-cache either way
```

`loader.S` does all of this, including the Cortex-A9 set/way cache flush that
has to happen *before* the D-cache goes off — otherwise the DTB you just patched
can still be sitting in dirty cache lines when the kernel reads it with caches
disabled, and you get a corrupt device tree with no clue why.

## Sequence

1. **Build everything.**

   ```sh
   make all          # DTBs + validation
   make kernel       # zImage
   make loader       # loader.bin
   ```

2. **Patch the DTB with the real runtime values.**

   The framebuffer base is the one value that must come off the live device.
   iBoot records it in the *live* Apple device tree's `/vram` node; the static
   firmware copy has `reg = <0 0>`. Read it however your payload can, then:

   ```sh
   python3 scripts/fdt_fixup.py dtb/p105ap.dtb -o dtb/p105ap-boot.dtb \
       --framebuffer 0x<vram base> \
       --initrd-start 0x88000000 --initrd-size $(stat -c%s initramfs.cpio.gz)
   ```

   Check it before uploading:

   ```sh
   python3 scripts/fdt_fixup.py dtb/p105ap-boot.dtb --show
   ```

3. **Enter DFU.** Hold Home + Power 10 seconds, release Power, keep Home for
   another 10. The screen stays black; `lsusb` shows Apple Mobile Device (DFU
   Mode).

4. **Run checkm8** from the Pico dongle or host tool. Confirm `PWND:[checkm8]`
   in the USB serial number before continuing.

5. **Upload** `zImage` → `0x80008000`, `p105ap-boot.dtb` → `0x90000000`,
   `initramfs.cpio.gz` → `0x88000000`, `loader.bin` → `0x87000000`, then jump to
   `0x87000000`.

6. **Watch the UART.** You are looking for, in order:

   ```
   [a5-loader] alive, handing off to Linux
   [a5-loader] CBAR/PERIPHBASE = 0x........
   Uncompressing Linux... done, booting the kernel.
   ```

   That second line is worth the whole exercise — see `docs/read-cbar.md`.

## Expected first-boot outcome

Booting `dtb/p105ap.dtb` should get you through decompression, DT parsing, AIC
registration and earlycon output, and then panic:

```
Kernel panic - not syncing: Unable to find a suitable clocksource
```

**That panic is the success condition for the first attempt.** It means the
device tree parsed, the machine descriptor matched, the AIC bound and the
console works — which is everything except the one value that was not in the
firmware dump. Take the CBAR value the loader printed, confirm it matches the
`0x3fd00000` guess, and boot `dtb/p105ap-a9timer.dtb` next.

If instead you get *nothing at all* on the UART, the problem is upstream of
Linux: either the jump did not happen, or the UART is not where we think it is,
or it needs a different access width. `loader.S` prints before it touches
anything, so silence there points at the payload rather than the kernel.
