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
   live in `/boot` on it.  (Alternative: a real LwVM partition named `Update`
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

| file | from | note |
|---|---|---|
| `iBEC.raw` | IPSW iBEC, decrypted with the public key (`build-firmware.sh`), patched | Apple's code: built locally, never shipped |
| `/boot/devicetree` | DeviceTree img3, **plaintext** | after kloader the GID key is locked, so iBEC cannot decrypt a KBAG; the 12H321 DeviceTree key is not in the repo yet (theiphonewiki was down 2026-11-07); a decrypted iOS 6.1.3 p105ap DeviceTree exists locally for a first try -- Linux ignores the ADT, iBEC only needs it to parse (`/chosen`) |
| `/boot/ramdisk` | a page of zeroes, img3 `rdsk` | iBEC requires one; the machine's layer already travels inside the bundle and the loader copies it to `0x88000000` |
| `/boot/kernelcache` | `staging-bundle.bin` unchanged, as a Mach-O segment at `0x80000000` (file offset `0x1000`), entry `0x80000000` (ARM), complzss (all literals: a zImage does not compress), img3 `krnl` | about 32.6 MB; built by `scripts/make-pcless.py` |
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
- **P3** device, no Linux yet: kloader the stock-behaviour patched iBEC with
  an *empty* `/boot` -> expect "failed to mount" or "failed to load
  devicetree" -> reboot into iOS.  Proves kloader -> iBEC and that the
  failure path is harmless.  Serial/UART if the Pico is attached; otherwise
  the screen.
- **P4** a tiny payload kernelcache (fill the framebuffer, spin) -> proves
  the format and the jump.
- **P5** the real bundle with the RAM root, then with the NAND root.
- **P6** the USB dependency in Linux (PPN probe and clockevents waiting for
  USB) -- the cable must not matter.
- **P7** `.deb`, `cascadia-boot`, the app; `./cascadia a5 ios-install`.
