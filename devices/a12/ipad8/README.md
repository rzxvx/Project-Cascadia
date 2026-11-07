# iPad 8 (A12) bring-up

This directory tracks the incremental Linux port for the 2020 iPad 8.
The first target is `iPad11,6` / `J171aAP` on iPadOS 17.1.1 (`21B91`).

## Current stage: minimal Linux boot

The A12 patcher in `../boot/patch_iboot.py` auto-detects the exact raw iBSS
by SHA-256 and refuses to use the iPhone XR resolver on it.  The iPad profile
currently provides:

- the Image4 validation-result override;
- the manifest-authentication success branch;
- replacement Linux boot arguments;
- serial (`serial=3`) and non-serial variants; and
- a dry-run mode which verifies every original instruction without writing.

Example verification:

```sh
python3 ../boot/patch_iboot.py iBSS.ipad11b.RELEASE.raw normal \
  --serial --dry-run
```

No offsets from the XR build are reused.  The profile is pinned to:

```text
feeaf79f8a11dffaafe5d4f334f6a6b868c29bc9117f716a4fbfa7e8d2f82fde
```

## Firmware source

```text
Product:       iPad11,6
Board:         J171aAP
SoC:           T8020 / A12
Version:       17.1.1
Build:         21B91
iBoot:         iBoot-10151.42.2
IPSW:          iPad_10.2_2020_17.1.1_21B91_Restore.ipsw
iBSS member:   Firmware/dfu/iBSS.ipad11b.RELEASE.im4p
DeviceTree:    Firmware/all_flash/DeviceTree.j171aap.im4p
```

IPSW URL:

```text
https://updates.cdn-apple.com/2023FallFCS/fullrestores/042-95894/E5C89FEE-E10A-4B33-8D69-973EDF7DB0A7/iPad_10.2_2020_17.1.1_21B91_Restore.ipsw
```

Apple firmware and generated boot artifacts remain untracked.

### Hardware result

The serial profile was tested on the physical iPad11,6. USBLiter8 loaded the
raw patched iBSS, the device re-enumerated as `05ac:1281`, `irecovery -q`
reported `CPID 8020`, `BDID 24`, `PRODUCT iPad11,6`, and the DCSD console
reached:

```text
Microkernel iBoot for ipad11b
Remote boot, Board 0x24 (j171aap)/Rev 0x5
BUILD_TAG: iBoot-10151.42.2
Entering recovery mode, starting command prompt
```

The corrected boot-argument patch was then tested through the complete minimal
bring-up chain:

```text
USBLiter8 -> patched iBSS -> Apple J171a DeviceTree -> m1n1 -> Linux
```

m1n1 identified `iPad11,6`, initialized the existing 1620x2160 framebuffer,
and exposed its USB proxy.  A minimal Linux DTB built from
`../dts/t8020-j171a.dts` subsequently booted the existing A12 kernel and
initramfs.  Six CPUs came online, simpledrm took over the 1620x2160 display,
the USB Ethernet gadget enumerated, and the initramfs was reachable over SSH
at `root@172.16.42.1`.  `/proc/device-tree/model` reported:

```text
Apple iPad 8 (Wi-Fi)
```

The first-boot DTB deliberately disables board-specific PCIe, radio, touch,
and secondary-UART descriptions until their J171a wiring is derived and
tested.  It does not reuse the iPhone XR peripheral descriptions.

## Next increments

1. Record the J171a display result and remove any XR-only display assumptions.
2. Derive the J171a multitouch SPI, GPIO, power and firmware properties.
3. Derive the iPad Wi-Fi and Bluetooth topology and add their firmware flow.
4. Add an iPad packaging/boot command after the incremental artifacts settle.

