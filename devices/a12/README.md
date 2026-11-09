# iPhone XR and XS Max (A12)

Linux on the A12 (T8020), booted tethered from DFU.

| Board | Phone | `A12_BOARD` | Tested |
|---|---|---|---|
| N841AP | iPhone XR (`iPhone11,8`) | `n841`, the default | by its author: Debian with XFCE over NFS |
| D331pAP | iPhone XS Max (`iPhone11,6`) | `d331p` | initramfs shell: SSH, display, touch, Wi-Fi and Bluetooth come up |

State, hardware and to do are in [LINUX_PLAN.md](LINUX_PLAN.md).

The boot reuses the A13 track's method. The T8020 DTS and most kernel
patches come from [badapple](https://forgejo.toe.quest/kiki/badapple), the
iPhone XS Max port. One kernel serves both phones; the DTB, m1n1, initramfs
and firmware are per board.

## XS Max

Prefix every command below with `A12_BOARD=d331p`. The differences:

- `periphery` takes a 16.6.1 (20G81) `.ipsw` for the radio firmware, whose
  root is not encrypted, instead of the OS image.
- m1n1 is badapple's series (`m1n1/d331p/`). The XR's starts on the XS Max
  but never reaches Linux.
- The iBSS is patched with `--any-im4m`, so iBoot loads m1n1 with any ticket.
- The touch and radio firmware are in the initramfs, there is nothing to
  load after boot, and there is no SysCfg step.
- `inputs` also fetches the GPU firmware, 16.6.1's, which the agx driver was
  written against, and `boot` sends it before `bootx`. The GPU comes up with
  a render node; GL and Vulkan need badapple's Mesa in the root filesystem.

## Host

An arm64 Linux host: the initramfs copies the host's busybox and dropbear.
The tested host was a phone running Kali NetHunter with NFS server support.
`./cascadia a12 doctor` lists the tools it needs.

You also need this phone's IM4M ticket (`apticket.im4m`) and, for the radios,
the 22H374 OS image (`094-31934-038.dmg.aea`), see
[FIRMWARE.md](FIRMWARE.md).

## Build

```sh
./cascadia a12 periphery /path/to/094-31934-038.dmg.aea   # touch and radio firmware
./cascadia a12 build                                      # m1n1, kernel, DTB, initramfs
IM4M=/path/to/apticket.im4m ./cascadia a12 inputs         # iBSS and the boot set
IM4M=/path/to/apticket.im4m ./cascadia a12 pack           # m1n1-linux.img4
```

Without the OS image, `periphery` only makes the touch firmware.

## Boot

Put the phone in DFU, then run the line below. `usbliter8ctl` sends the
patched iBSS, the rest goes over `irecovery`.

```sh
./cascadia a12 boot
```

It comes up on USB at `172.16.42.1`, `ssh root@172.16.42.1`. A DCSD cable
on `/dev/ttyACM0` gets a serial log in `boot/logs/`, it is optional.

On the first boot, save this phone's SysCfg, then pack again. `pack` picks
it up and puts the Wi-Fi MAC, calibration and Bluetooth address in the DTB:

```sh
bash devices/a12/host/read-syscfg.sh
IM4M=/path/to/apticket.im4m ./cascadia a12 pack
```

## Desktop

The desktop is a Debian root the host serves over NFS, see
[DEBIAN_XFCE_NFS.md](DEBIAN_XFCE_NFS.md):

```sh
sudo ./cascadia a12 build rootfs
sudo ./cascadia a12 nfs on
./cascadia a12 build initramfs
IM4M=/path/to/apticket.im4m ./cascadia a12 pack
./cascadia a12 boot radios       # boot, then load Wi-Fi and Bluetooth
```

Apple firmware, the ticket, SysCfg, payloads and logs stay local and are
ignored by git. Nothing is written to the phone's storage.

## Credits

- **dr1408**: the XR port: N841 DTS, the HBPP touch driver, the BCM4357 and
  N88 radio patches, and the boot, NFS and firmware scripts
- **kiki**: [badapple](https://forgejo.toe.quest/kiki/badapple), the iPhone XS
  Max port: the T8020 DTS and PMGR, Linux patches 0001 to 0014, the GPU
  driver 0020, and the T8020 m1n1 work (DWC2, chicken bits) the XR m1n1
  series builds on
- **Hoolock Linux**: [linux](https://github.com/HoolockLinux/linux),
  [m1n1 `idevice`](https://github.com/HoolockLinux/m1n1) and `config_16k`
- **Asahi Linux**: m1n1, and the Apple silicon drivers Hoolock builds on
- **Corellium**: Project Sandcastle's `hcdpack`, which builds the Bluetooth
  HCD
- **Pauli1Go**: the iPhone 7 Bluetooth notes that pointed at the full UART
  pinmux and RTS/CTS
- **Paradigm Shift**: usbliter8
- **blacktop**: [ipsw](https://github.com/blacktop/ipsw)
