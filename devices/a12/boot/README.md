# Boot set

`prepare-xr-linux-bootset.py` writes the first stage to `boot/inputs/`:

| File | What |
|---|---|
| `iBSS.linux.serial.raw` | iBSS patched by `patch_iboot.py`, boot arguments `-v wdt=-1 serial=3` |
| `DeviceTree.img4` | the N841 ADT, unchanged, as `rdtr` |
| `RestoreTrustCache.img4` | an empty `rtsc` |
| `m1n1.img4` | m1n1 alone, as `rkrn` |

```sh
python3 devices/a12/boot/prepare-xr-linux-bootset.py --ticket /path/to/apticket.im4m
```

`--no-serial` drops `serial=3`. The file name stays the same.

The ADT keeps iOS's `content-protect` and has no `boot-ios-diagnostics`.
Linux gets its own `t8020-n841.dtb` inside the m1n1 payload.

Upload order once usbliter8 has started the iBSS:

1. `DeviceTree.img4`, then `devicetree`;
2. `RestoreTrustCache.img4`, then `firmware`;
3. `m1n1.img4`, then `bootx`.

That boots the m1n1 proxy. For Linux, `package-linux.py` packs m1n1, the DTB,
the kernel and the initramfs into `m1n1-linux.img4`, which `boot-xr-linux.sh`
sends in place of `m1n1.img4`.
