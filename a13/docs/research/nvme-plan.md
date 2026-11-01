# NVMe: the internal storage for Linux

Goal: a persistent Linux root on the internal NVMe, so the phone boots into
its own system -- no NFS, no RAM-only.

**Status 2026-11-01: done** -- the phone boots from its own NVMe
(`/dev/nvme0n1p1`, ext4, 116.8 GiB). N0-N3 below walked on the phone.

**Decision (2026-11-01): iOS is not kept.** All of namespace 1 (the APFS
container, iOS and its data) goes to Linux. iOS can come back by a restore
in Finder / iTunes, which in turn takes the Linux disk. The small namespaces
2-7 are never written.

## What is there (read-only survey, 2026-11-01)

| Namespace | Size | What |
|---|---|---|
| `nvme0n1` | 119.2 GiB | GPT (4096-byte LBAs, 31 250 000 of them), one partition: the APFS container |
| `nvme0n2` | 8 MiB | firmware (img4 store) |
| `nvme0n3` | 128 KiB | syscfg: factory calibration, MAC addresses, serial numbers -- **irreplaceable**, a restore does not rewrite it |
| `nvme0n4` | 8 KiB | iPCB |
| `nvme0n5` | 8 KiB | nvram |
| `nvme0n6` | 4 KiB | effaceable storage |
| `nvme0n7` | 1 MiB | (unknown) |

The APFS container before the erase: latest checkpoint xid 171212, 102.4
GiB free; volumes System 6.8 GiB (unencrypted), Data 3.5 (encrypted),
Preboot 5.1, User 1.1 (encrypted), Update, xART, Hardware, Baseband Data.
Its metadata reads in plain text from Linux (checkpoint area, object map,
space manager, volume superblocks).

A copy of namespaces 2-7 and of the GPT areas of namespace 1 is on the host
in `boot/inputs/nvme-backup/` (not tracked: serial numbers), with SHA256SUMS.

## The guard: the write gate (N0 -- done, tested 2026-11-01)

Kernel patch 0007, `drivers/nvme/host/apple.c`, in `queue_rq` after the
command is set up:

- I/O queue: read and flush pass. Write and write-zeroes pass only inside an
  `apple,write-ranges` range of their namespace; a discard (DSM) only when
  it is the kernel's own (its range list in the request's special payload)
  and every range is inside. Everything else (write uncorrectable, copy,
  zone commands, passthrough DSM) is refused.
- Admin queue: namespace management / attach, format, sanitize, firmware
  download / activate, security send and all vendor opcodes (>= 0xc0) are
  refused.
- A refusal is `BLK_STS_IOERR` and a rate-limited `write gate: refused ...`.
  At probe: the open ranges, or `write gate: closed, every write refused`.
- `apple,write-ranges = <nsid lba-hi lba-lo count-hi count-lo>`, count 0 =
  to the end of the namespace. `pack` writes `<1 0 0 0 0>` (all of namespace
  1) once `periphery` has the decision; namespaces 2-7 stay closed.
- The initramfs sets every namespace read-only at boot, namespace 1 included
  unless the DT opens it.

Test on the phone (2026-11-01, gate closed): the boot says `write gate:
closed`; all namespaces `ro=1`; a block of `nvme0n7` read and written back
unchanged gives `0 bytes copied` and `write gate: refused I/O opcode 0x01
nsid 7 lba 0 +1`, the block is the same afterwards. (Harmless even had the
gate failed. Discard is not tried with the gate closed: a broken gate would
erase.)

## The steps

- **N1 -- the decision.** `a13/cascadia periphery` asks (type `ERASE`;
  `NVME=erase` for scripts) and leaves `boot/inputs/nvme-linux`. `pack` then
  opens the gate for namespace 1. Boot.
- **N2 -- the Linux disk.** `periphery` again, the phone booted from that
  image (Alpine in RAM has `sgdisk` and `mkfs.ext4`): copies namespaces 2-7
  to the host if not yet there, asks `ERASE` once more, and on the phone:
  `sgdisk --zap-all`, one partition `cascadia-root` (type 8305, Linux ARM64
  root) over all of namespace 1, `mkfs.ext4` (discards the old blocks:
  allowed inside the open range), the Alpine root unpacked onto it, the
  phone's known Wi-Fi networks along.
- **N3 -- booting from it.** The initramfs looks for `LABEL=cascadia-root`
  on namespace 1 (only when the DT opens it) and switches to it before NFS
  and before Alpine in RAM. Keys, firmware, the USB watchdog, `wifi` come
  from the image as for the other roots. (The initramfs busybox has no
  `blkid`; `findfs LABEL=...` finds it -- the first boot fell back to RAM
  over that.)
- **N4 -- after.** Check: reboot several times, `apk add` and reboot,
  power loss (journal). Updating the root from a new `build/alpine-a13.tar.gz`
  without losing what is installed: later.

## Not done

- Linux side by side with iOS. The first plan: iOS (jailbroken, or from an
  SSH ramdisk) makes an unencrypted APFS volume with one preallocated,
  contiguous file and reports its extent; Linux maps that extent with
  device-mapper linear, ext4 on top, the gate open for those LBAs only, and
  checks the extent against the APFS metadata at every boot. Dropped with the
  decision above; the gate already takes such ranges.
- A normal power-on (not DFU) finds no iOS: the phone shows the recovery
  screen. Linux boots tethered from DFU as before.
