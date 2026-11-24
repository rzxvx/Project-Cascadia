# J171: the boot, from DFU to /init

Every boot, tethered: `./cascadia a10 boot` (iPad in DFU). Open source end
to end; the sequence of HoolockLinux's `remote_boot`, rewritten as
`inputs | pack | boot`. Measured on one iPad7,11, 2026-11-22.

## 1. checkm8 and iBSS (`inputs`, once per board)

- `irecovery -q` must say CPID 0x8010 and MODEL `j171ap` / `j172ap` (the
  iPad 8's `j171aap` is an A12 and refused).
- **gaster** (HoolockLinux/gaster, Apache-2.0) `pwn`, then a throwaway
  `decrypt_kbag`: it leaves the AES engine usable for the real decryption.
- iPadOS **16.4** (20E246) gives `Firmware/dfu/iBSS.ipad7c.RELEASE.im4p` (one
  for J171 and J172) and `Firmware/all_flash/DeviceTree.j171ap.im4p` -- a
  partial download (`ipsw download ipsw --pattern`). 16.4 is the version
  remote_boot uses for boards whose latest iOS is 16 or newer; which iPadOS
  is installed does not matter, the boot never runs it.
- **hBootPatcher** `-airs` on the decrypted iBSS: AES (keeps the engine),
  IORVBAR, reconfig, the signature checks. It reports "Image is stage2":
  the A10 boots in one stage, no iBEC.
- Every image is wrapped with a T8010 IM4M from remote_boot (`0x8010.im4m`,
  its SHA-256 pinned): iBoot still parses a manifest, the patched iBSS does
  not check whose.

## 2. iBSS loads (`boot`)

`gaster reset`, then over irecovery: iBSS (it comes back in Recovery within
a few seconds), the DeviceTree (`rdtr`, `devicetree`), an empty trust cache
(`rtsc`, `firmware`: version 1, a UUID, no entries -- 24 bytes), the Alpine
ramdisk (`rdsk`, `ramdisk`), m1n1 as the restore kernel (`rkrn`, LZSS),
`bootx`. ipsw 3.1.714 writes a bad ASN.1 compression record into LZSS IMG4s;
`devices/a13/boot/repair_lzss_img4.py` drops it.

The ramdisk: iBoot refuses a large `rkrn` (an iBoot panic at ~36 MB on the
A13), so the Alpine root travels apart. 9.8 MB and 19.4 MB were taken.

## 3. m1n1

HoolockLinux/m1n1 `idevice` (d5a10ac, the A13 track's) with
`devices/a10/m1n1/0001`-`0011`. What it prints on the DCSD UART, in order:

```
CPU: A10/T2 Hurricane-Zephyr
kboot: iBoot ramdisk at 0x805c34000, 0x99392f bytes
kboot: initrd + ramdisk: 0x12ba483 bytes                       (patch 0011)
Found a m1n1 SysCfg payload at ..., 0x20000 bytes               (0003)
FDT: added SysCfg nvmem-rmem at 0x8ba4b8000, 0x20000 bytes      (0002)
FDT: NVMe HMB at 0x8bee00000 size 0x1200000                     (0001)
pcie: Using SecureROM T8010 sequence for first link (profile 0)
pcie: T8010 NVMe first link reached operational state 0x93401101
pcie: initial S3E state CAP=0x0 ... marker=0xbfbfbfbf
pcie: Exact uninitialized S3E signature detected
pcie: Cold S3E straps prepared while PERST# is asserted
pcie: post-restart S3E state CAP=0x20f00103ff VS=0x00010001 ...
      HMB=0x00b00000/0x00001000 core=0x00000003 log=0x000000ff marker=0x00badadd
pcie: T8010 NVMe link trained (status 0x9b001101)               (0007, 0008)
```

**The NVMe has to be up before Linux.** Pauli1Go's kernel driver (H9P PCIe
+ ANS) expects m1n1 to have brought the link up and the controller into its
"warm" state, and the HMB region filled in the DT (`nvme-hmb`). HoolockLinux's
m1n1 does none of it; Paul Praschl's commits (0001-0009 here, authorship
kept) do. Their S3E code accepts only exact, known controller states --
"a different board, firmware profile, or partially initialized endpoint
must fail without a write" -- and the J171 differed from the J172 in the HMB
size alone: 0x00b00000 (J172 0x00c00000, D111 0x00e00000). Before 0010 m1n1
said `Cold-S3E reset-time straps did not produce the exact warm signature`,
`refusing kernel handoff`, and the screen "No valid payload found". The
straps themselves (boot_from_host, the debug GPIO) matched J172's values.

The payload m1n1 finds behind itself (`pack`): `chosen.bootargs=...`, the
board's DTB (with `apple,write-ranges` when the NVMe gate is open:
[j171-nvme.md](j171-nvme.md)), the initramfs (+ an overlay cpio: ssh keys,
the host key, Apple's firmware, fbkeyboard), `Image.gz`, the SysCfg trailer
(`m1n1_syscfg`, u32 length, 128 KiB), four zero bytes.

## 4. Linux to /init

Linux 7.2.0-rc1 (Pauli1Go/HoolockLinux 7b3e4e8 + our two patches), 16K
pages, `earlycon console=ttySAC0,115200 console=tty0`. On a normal boot:
simpledrm and fbcon at ~1.0 s, the touchscreen at 1.3 s, the NVMe at 2.0 s,
`/init` at ~4.5 s (its NVMe wait included), the USB gadget bound and the
root mounted by 4.8 s, Alpine up ~5 s in.

`/init` (the A13's, `initramfs/make-initramfs.sh`): busybox, a configfs
gadget on dwc2 (ACM `ttyGS0` + ECM `usb0`, 172.16.43.1, udhcpd hands the Mac
.2), dropbear (keys, or root without a password), fbkeyboard on the
touchscreen (found by name), the NVMe's write protection, then a root:
`cascadia-root` on the NVMe, else NFS, else Alpine from the ramdisk into a
tmpfs, else its own shell. `cascadia.root=ram` skips the NVMe.

## The first boots, as they went

1. m1n1 proxy: first try.
2. Linux from the proxy (`linux.py`): the kernel log on the screen, then a
   data abort at 62.7 s. A built-in `g_multi` had taken the UDC and failed
   (-22); the missing SmartIO firmware made the kernel wait 60 s for a
   sysfs helper; the H9P probe failed (no HMB) and its error path
   dereferenced NULL (`apple_h9p_pcie_detach_genpd`).
3. Through DFU, with a DCSD log: the same oops, now with a backtrace.
4. With Pauli's m1n1 commits: "No valid payload found" (the HMB size).
5. With 0010: the NVMe found, USB up, ssh.
