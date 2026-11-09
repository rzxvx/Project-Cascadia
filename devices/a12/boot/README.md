# XR first-stage boot set

Prepare the iBSS and ticketed m1n1 components. DCSD serial is enabled by
default; use `--no-serial` to omit `serial=3` from the iBSS boot arguments:

```sh
python3 devices/a12/boot/prepare-xr-linux-bootset.py
python3 devices/a12/boot/prepare-xr-linux-bootset.py --no-serial
```

The output is written to `devices/a12/boot/inputs/`:

- `iBSS.linux.serial.raw`: the bundled `patch_iboot.py` applies the XR
  validation patches with `-v wdt=-1 serial=3` by default, or
  `-v wdt=-1` with `--no-serial` (the filename is unchanged);
- `DeviceTree.img4`: the **pristine** N841 Apple ADT, ticketed as `rdtr`;
- `RestoreTrustCache.img4`: minimal empty `rtsc` staging object;
- `m1n1.img4`: the T8020-enabled m1n1 proxy, ticketed as `rkrn`.

The iOS-only DeviceTree changes (`content-protect` removal and
`boot-ios-diagnostics`) are intentionally absent. The later Linux hardware
description is a separate `t8020-n841.dtb` appended to the complete m1n1
payload.

Expected upload order after USBLiter8 hands off to the raw patched iBSS:

1. `DeviceTree.img4`, then `devicetree`;
2. `RestoreTrustCache.img4`, then `firmware`;
3. `m1n1.img4`, then `bootx`.

This set boots the m1n1 proxy only. For Linux, `package-linux.py` packs m1n1,
the N841 DTB, the kernel and the initramfs into `m1n1-linux.img4`, which
`boot-xr-linux.sh` sends in place of `m1n1.img4`.
