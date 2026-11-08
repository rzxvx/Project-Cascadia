# Booting Linux from iOS without a computer (route A) -- plan, 2026-11-07

Goal: from the jailbroken iOS on the iPad, start Linux with nothing else:
no Mac, no Pi Pico, no USB cable.  Today `./cascadia a5 flash --kdfu` has
kloader on iOS boot our iBSS, which then waits in DFU for iBEC and the
bundle over USB.  Route A keeps iBoot in the chain -- the hardware is in the
state Linux is written for (iBoot's), not the state iOS's kernel leaves --
and has iBoot fetch the kernel from the NAND itself.  Route B (a kexec
straight from iOS into our loader) was set aside for that reason.

## What iBEC can already do (static RE of our decrypted 12H321 iBEC)

All addresses: iBEC iBoot-2261.30.37, base `0x9ff00000`
(`build/firmware/iBEC.dec`).

- **It relocates itself.**  The reset code compares the address it runs at
  with `0x9ff00000` and copies itself there (`0x9ff00040`).  kloader can
  therefore start iBEC directly, not only iBSS; DRAM is up (iOS was running).
- **It has the storage stack** of iBoot: the PPN NAND driver
  ("Apple PPN NAND Driver, Read-Only"), the FTL, an LwVM parser and HFS+.
- **LwVM** (`0x9ff25a40`): one block device per partition, named
  `nand0a`, `nand0b`, ... Partitions with attribute bit 48 set (Data,
  effaceable) are skipped.  A partition whose UTF-16 name is `Update`
  (data at `0x9ff439e4`, compared over 14 bytes) gets flag 2: the upgrade
  partition.
- **Boot commands**: the `boot-command` table (`0x9ff44040`) has one entry,
  `upgrade` (`0x9ff0147c`).  The platform default `boot-command=fsboot`
  (set at `0x9ff011b6`) is *not* supported by iBEC; `auto-boot=true` and
  `bootdelay=0` are already the defaults.
- **`upgrade`** mounts the flagged partition (HFS+), then loads, in order:
  `/boot/apticket.der` (optional), `/boot/applelogo` (optional),
  `/boot/devicetree` (img3 `dtre`, required), `/boot/ramdisk` (img3 `rdsk`,
  required, loaded to **`0x88000000`**, up to 64 MiB -- the address our
  bundle's layer already uses), `/boot/kernelcache` (img3 `krnl`), then jumps
  (`0x9ff008b8` -> `prepare_and_jump(3, entry, boot_args)` at `0x9ff1f504`).
  On a failure it records the failure (NVRAM) and reboots.
- **Kernelcache format** (`0x9ff1b874`): `complzss` header (`comp`,`lzss`,
  adler32, uncompressed size <= `0x3f00000`, compressed size, data at `+0x180`),
  inside a Mach-O whose segments are placed by vmaddr (32-bit iOS: VA = PA in
  `0x8xxxxxxx`) and whose entry comes from `LC_UNIXTHREAD`.

## The chain

```
iOS (jailbroken)                    -- app / command: cascadia-boot
  kloader iBEC.raw                  -- patched iBEC, plaintext
    iBEC: auto-boot -> upgrade      -- mounts the "upgrade" partition
      /boot/devicetree   (dtre, plaintext)
      /boot/ramdisk      (rdsk = our layer's cpio, lands at 0x88000000)
      /boot/kernelcache  (krnl = complzss Mach-O around linux-boot + zImage-dtb)
    prepare_and_jump -> our loader at 0x80000000 -> Linux
Linux: root on /dev/ftl0p2 (nand-rw) or the RAM root
```

## iBEC patches (`scripts/patch-ibec-upgrade.py`, on top of iBoot32Patcher's signature patches)

Each site is checked against the expected original instruction; 22 bytes
change in all.  The output is `build/firmware/iBEC.upgrade`, raw, for kloader.

1. **autoboot** `0x9ff00cb2`: the `bne` taken when `auto-boot` is not true
   becomes a nop.  NVRAM can override the built-in default, so neither that
   nor `boot-command` is left to the environment.
2. **command** `0x9ff00e30` / `0x9ff00d02`: the dispatcher's literal becomes
   the string "upgrade" itself and the `getenv` call goes.  Only that one
   `ldr` reads the literal (the patcher checks).
3. **partition** `0x9ff439e4`: UTF-16 `Update` becomes `System`.  System is
   HFS+, not encrypted, and writable from the jailbroken iOS, so the files
   live at its root.  (Alternative: a real LwVM partition named `Update`
   from another native Data shrink -- cleaner, more work; also what an OTA to
   9.3.5 would claim.)
4. **no-saveenv** `0x9ff01486`: `upgrade` starts with unsetenv(boot-command) +
   `saveenv()` (`0x9ff1c358`, the `saveenv` command's own body); that call goes.
5. **no-record** `0x9ff01504`: on a failure `upgrade` stores `ota-result` /
   `ota-failure-reason` and saves them (`0x9ff01438`); that call goes, and a
   failed boot just reboots.

With 4 and 5 nothing in this path writes the NAND.

Perhaps later: the final jump.  `prepare_and_jump(3, ...)` quiesces the
platform (USB, caches, MMU) before entering the "kernel"; today our loader is
entered from iBEC's USB callback with all that still running.  If the loader
or Linux depends on that, replace the call at `0x9ff014d0` by the same
cache-flush-and-`blx` the auto-go hook uses.

## Files and where they come from

iBEC mounts the partition at `/boot` of its own namespace (`0x9ff00a56`), so
its `/boot/kernelcache` is **`/kernelcache` at the root of System**, not a
file in a `/boot` directory.

| file | from | note |
|---|---|---|
| `iBEC.raw` | IPSW iBEC, decrypted with the public key (`build-firmware.sh`), patched | Apple's code: built locally, never shipped |
| `/devicetree` | the IPSW's DeviceTree, decrypted with its public key (`build-firmware.sh`), plaintext img3 | after kloader the GID key is locked, so iBEC cannot open a KBAG.  It must be 12H321's own: iBEC **hangs** on the iOS 6.1.3 tree (P4) |
| `/ramdisk` | a page of zeroes, img3 `rdsk` | iBEC requires one; the machine's layer already travels inside the bundle and the loader copies it to `0x88000000` |
| `/kernelcache` | `staging-bundle.bin` unchanged, as a Mach-O segment at `0x80000000` (file offset `0x1000`), entry `0x80000000` (ARM), complzss (all literals: a zImage does not compress), img3 `krnl` | about 32.6 MB; built by `scripts/make-pcless.py` |
| `kloader` | Legacy iOS Kit's, pinned by hash (as `tools/kdfu.sh`) | later: our own, tfp0 is available |

System's free space has to hold ~33 MB (the kernel with its built-in base
initramfs); check with `df` on iOS first.

Limits in iBEC: the img3 is loaded at `0x84000000` (at most `0x3f00000` bytes);
the compressed data is copied to the heap, whose large chunk is 48 MiB at
`0x8c000000` (a `malloc` that does not fit panics); the Mach-O must end below
`0x84000000`.  `make-pcless.py` enforces these.

## Packaging

A `.deb` with only our code: `cascadia-boot` (runs kloader on `iBEC.raw`),
a minimal home-screen app with one action -- enter Linux with a single tap --
and the directory layout.  The Apple-derived
files (`iBEC.raw`, the DeviceTree) and the machine's layer come from
`./cascadia a5 ...` on a computer once (an `ios-install` step over ssh);
later the app could fetch the IPSW pieces from Apple's servers itself.  No
auto-boot into Linux at iOS start-up: a bad payload must never cost iOS.

## Ladder

- **P1** (static, done above): the chain exists in iBEC.
- **P2** (done, 2026-11-08): `scripts/patch-ibec-upgrade.py`,
  `scripts/make-pcless.py`.  `tests/test-pcless.py` reads the output back with
  independent decoders; `tests/test-pcless-ibec.py` runs iBEC's own LZSS
  decoder, adler32 and Mach-O segment loader on it under Unicorn: the 29 MB
  nand-rw bundle decodes exactly, lands at `0x80000000..0x81bac000`, entry
  `0x80000000`.  Not shown by this: iBEC's mount, img3 and jump paths.
- **P3** (done, 2026-11-08): kloader on iOS with none of the files present.  The plain
  image ended in a reboot to iOS, which proved nothing (a kloader failure looks
  the same), so `patch-ibec-upgrade.py --stay` returns to the dispatcher on a
  failure instead.  With it the iPad came up in **Recovery 18 s after
  kloader**, `build-version` iBoot-2261.30.37, `auto-boot` true and
  `boot-command` unset, which only our `upgrade` path does (stock iBoot has
  `fsboot`).  So kloader starts iBEC directly, the forced dispatch works and
  `upgrade` runs.  Not shown: which step failed -- mount and missing
  devicetree leave by the same exit, and there is no console -- so whether
  iBEC mounts System is P4's question.  `reboot` over USB returned to iOS.
  Reading a variable over USB (`getenv`, then a control read): an unset one
  repeats the previous reply, or stalls if there is none.  Side note: the first
  boot after the plain run panicked in `everuntether` (the jailbreak's own
  process, pid 17) and the next boot was fine; no link to this test shown.
- **P4** (done, 2026-11-08): `tools/pcless/fill.S`, a payload that fills the
  framebuffer at `0x9F6FC000` green, as `/kernelcache`, with `/ramdisk` and
  `/devicetree` at System's root (sha1 checked on the iPad; `sync`, 40 s,
  `sync` for the HFS journal, since iBEC does not replay it).
  With the iOS 6.1.3 tree iBEC hung before the end of the load.  `--stop
  STAGE` (without `--stay`) tells the outcomes apart: Recovery = reached,
  reboot to iOS = a failure exit, neither = a hang.  With 12H321's DeviceTree
  `--stop kernelcache` reached Recovery in 15 s, and without the stop the
  screen went **green**: kloader -> iBEC -> NAND -> our code, no USB involved.
- **P5** (Linux up, 2026-11-08; the loader is still a bring-up build):
  the real bundle through kloader -> iBEC -> NAND boots Linux, repeatedly, no
  USB transfer.  `prepare_and_jump(3)` leaves the USB side off, and comparing
  184 registers printed over UART at loader entry, against the same loader
  booted over USB, showed exactly what: the PMGR gates `0x3F101080`,
  `0x3F101084`, `0x3F101098` are off (`0x200`, not `0x2ff`) and the OTG PHY at
  `0x36000000` is powered down, while `phy-apple-s5l-usb` deliberately keeps
  iBoot's PHY setup.  Restoring them before the kernel -- the three gates, then
  PHY `+0x04=1 +0x14=0xc +0x1c=6 +0x20=0xc00 +0x40=0x549 +0x44=0xf8 +0x60=0x200
  +0=6`, reset pulse on `+0x08` -- brings up the OTG gadget (ssh over USB) and
  EHCI, and with it the Wi-Fi chip on HSIC (firmware runs).  `usb-complex+0`
  bit 31 cannot be set by a write and follows by itself.
  Also: the loader's watchdog address `0x0F103020` is arm-io relative; the
  physical one is `0x3F103020` (`-DBOOT_WDT_BASE=`).
  Bring-up build: `build/pcless-phy`.  Still unexplained: the loader flickered
  (re-entered) with only the watchdog address fixed, and stopped with the
  8-entry vector table at `0x80000000` and a green fill + pause at entry; which
  of the two matters is unknown.  What the loader saw of this path: CPU in SVC,
  caches apparently off.  Only whole-screen fills are reliably visible.
- **P6**, the driver half (done, 2026-11-08): `phy-apple-s5l-usb` turns on the
  three gates and programs the PHY only when `OPHYPWR` is not `0x6`, i.e. when
  the boot left it off.  Checked both ways on the iPad with kernel #9: from
  the NAND with a loader that no longer touches USB (the "left off" path runs,
  gadget and Wi-Fi firmware come up), and over USB with the NFS root (no "left
  off" line, iBEC's own PHY setup kept).  `+0x44` is `0x2ff3`, the ADT's
  `uotgtune2-device`; the bring-up loader's `0xF8` came from a stale comment.
  `+0x14` reads `0x8` under Linux on both paths, against `0xc` at loader entry.
- **P6**, still open: drop the loader diagnostics (one change at a time, as what
  stopped the flicker is unknown), then boot the NAND root.
  The AIC timer already gives the tick without a USB host (see the boot log);
  the PPN probe still waits for USB -- check that with no cable attached.
- **P7** `.deb`, `cascadia-boot`, the app; `./cascadia a5 ios-install`.
