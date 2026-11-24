# J171: the NVMe, a partition for Linux, the write gate

## The device

Apple's ANS behind PCIe port 0 (`apcie` / `s3e`, PCI 106b:2002), driven by
nvme-pci with Pauli1Go's ANS FlatDMA, NVMMU and HMB support; the link and
the controller's warm state come from m1n1 ([j171-boot.md](j171-boot.md)).

```
nvme nvme0: pci function 0000:01:00.0
nvme nvme0: 1/0/0 default/read/poll queues
 nvme0n1: p1 p2
```

4096-byte logical blocks. Seven namespaces (`namespaces-present = 07` in
the ADT): n1 the disk (32 GB, 7812500 blocks), n2-n7 Apple's small ones --
n3 is **SysCfg** (128 KiB, magic `gfCS`): the factory calibrations and
addresses that touch, Wi-Fi and Bluetooth need.

## The layout, as iPadOS left it

GPT on n1: both copies intact and equal, 128 entries; one partition,
`7C3457EF-...` "Container", LBA 6-7812494, filling the disk. The APFS
container: 4096-byte blocks, 7812489 of them. iPadOS 18.7.9's own view:
system 5.7 GiB, data 3.3 GiB, preboot 3.8 GiB, 16 GiB free.

## Room for Linux: iPadOS shrinks itself

Linux cannot write APFS, and reading it from Linux was no good either
(apfs-fuse: "Initialization of KeyManager failed" -- the iOS keybag). So the
container is shrunk by iPadOS's own APFS, live, as OrchardLinux's
resize-apfs (MIT) does -- from a terminal here, not an app
(`devices/a10/storage`):

- `cascadia-storage`, C, built with the iPhoneOS SDK, ad-hoc signed by ldid
  with `platform-application`, `no-sandbox`, `no-container`,
  `disk-device-access` and `iokit-user-client-class: AppleAPFSUserClient`;
  run as root (sudo) in a jailbroken iPadOS over SSH through usbmux.
- `APFSContainerGetMinimalSize("disk0s1", &bytes)` and
  `APFSContainerResize("disk0s1", bytes)`, resolved with `dlsym` from
  `/System/Library/PrivateFrameworks/APFS.framework/APFS` -- the BSD name is
  the partition's, the physical store.
- The plan: the Linux partition at the end, its start aligned down to 1 MiB;
  APFS ends right before it. Refused: a GPT that is not intact, copies that
  differ, anything but one APFS partition filling the disk, APFS less than
  2 GiB over its own minimum.
- The order: APFS first (then the container's superblock read back), the
  GPT after -- the backup entries and header, then the primary, `fsync` and
  `DKIOCSYNCHRONIZECACHE` between, everything read back and compared with
  the plan. `/dev/rdisk0` opened `O_SHLOCK`, as GPT fdisk does on Darwin.

On this iPad: APFS's minimum 15.57 GB. Linux 10 GB (the default; 14 the
recommended maximum, 17 refused): APFS 21.999 GB (LBA 6-5370879, 5370874
blocks), `disk0s2` "Linux" (`0FC63DAF-...`) LBA 5370880-7812494, 10.0 GB.
About four minutes; meanwhile `df` in iPadOS already shows the cut part as
gone (6.5 GiB free afterwards, the volume 20 GiB).

## The write gate (kernel patch 0002)

In nvme-pci, for 106b:2002 only, after the A13's (`apple.c` there):

- writes, write-zeroes and the kernel's own discards (DSM with its ranges
  in reach) must lie inside a range of `apple,write-ranges` on the PCIe
  host bridge: `<nsid lba-hi lba-lo count-hi count-lo>`, up to four; none:
  no writes anywhere;
- format, firmware download/commit, namespace management/attach, sanitize,
  security send and every vendor admin command: never;
- refused before it reaches the controller, logged:
  `write gate: refused I/O opcode 0x01 nsid 1 lba 5370880 +1`.

`./cascadia a10 nvme setup` (read-only) reads the GPT entries over ssh,
needs exactly one "Linux" entry of the Linux type, checks it against
`/sys/class/block/nvme0n1pN/{start,size}`, and notes
`nsid=1 first=5370880 count=2441615 part=nvme0n1p2`; `pack` writes it with
fdtput into the board's DTB. `/init` then leaves `nvme0n1` and the
`PARTNAME=Linux` partition writable at the block layer (a read-only disk
would make its partitions read-only too) and sets everything else -- APFS,
n2-n7 -- read-only.

Tested: with no range, a write to the empty Linux partition, made writable
at the block layer, came back an I/O error and the line above. With the
range, `mkfs.ext4` and the root's unpacking: no refusal at all.

## Linux's root on it

`./cascadia a10 nvme format`: `mkfs.ext4 -L cascadia-root` (9.1 GiB usable),
the Alpine root with the desktop unpacked onto it, `sync`. `/init` finds
`LABEL=cascadia-root` (busybox `findfs`) and switches to it. ext4 recovers
its journal at most boots: every way off is a hard reset.
