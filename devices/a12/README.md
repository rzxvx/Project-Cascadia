# Cascadia A12 — iPhone XR bring-up

Initial target: iPhone XR (`iPhone11,8`, `N841AP`, T8020).  This track reuses
the A13 boot architecture but not its N104 DeviceTree or Linux DTB.

The detailed status, completed patches, verified payload and remaining work
for display, touch, Wi-Fi and Bluetooth are tracked in
[`LINUX_PLAN.md`](LINUX_PLAN.md).

Known hardware facts extracted from the existing XR Apple DeviceTree:

- T8020, four Tempest E-cores (`reg` 0..3) and two Vortex P-cores (`0x100`,
  `0x101`).
- AIC v2 at `0x3b100000`, event block at `0x3b108000`.
- PMGR compatible `pmgr1,t8020`.
- Lightning uses `usb-complex,t8020|usb-complex,t8011`, the DWC2 path already
  present in m1n1, not the iPhone 11's DWC3/AUSB path.
- USB DART is `dart,t8020` at `0x39900000`.
- Touch is HID-over-SPI at `spi1` (`0x35104000`).
- PCIe is `apcie,t8020`, ports 2 and 3.

First milestone:

1. patched XR iBSS starts m1n1;
2. m1n1 identifies Vortex/Tempest and keeps its USB console;
3. m1n1 hands off to a one-core, 16K-page Linux kernel;
4. Linux prints through the iBoot framebuffer and reaches an initramfs shell.

Build the first-stage m1n1 payload:

```sh
devices/a12/m1n1/build-m1n1.sh
```

The m1n1 builder pins its upstream commit, applies Cascadia's A13 series,
then the XR patches in `devices/a12/m1n1/`. For Linux, fetch the pinned Hoolock source
and verified 16K config before building:

```sh
bash devices/a12/linux/prepare-kernel.sh
bash devices/a12/linux/build-kernel.sh
bash devices/a12/dts/build-dtb.sh
```

The Linux source baseline is pinned to the XR-tested commit; the numbered
`devices/a12/linux/patches/` record the XR differences and the build script checks
whether its incremental patches are already present. Do not treat a patch's
presence as proof that the corresponding hardware is enabled or verified.

Prepare the XR iBSS/DeviceTree/m1n1 boot set with DCSD serial enabled:

```sh
python3 devices/a12/boot/prepare-xr-linux-bootset.py --ticket /path/to/apticket.im4m
```

See `devices/a12/boot/README.md` for the generated files and upload order. The Apple
DeviceTree remains pristine; Linux DT changes belong in the later N841 DTB.
See [`FIRMWARE.md`](FIRMWARE.md) for 22H374 partial extraction, touch/radio
firmware inputs, private SysCfg handling, and the `--serial`/`--no-serial`
first-stage options.
For the host-exported Debian XFCE desktop, follow
[`DEBIAN_XFCE_NFS.md`](DEBIAN_XFCE_NFS.md). This is a separate N841 NFS root,
not the iPhone 11 or A5 desktop image.

## Rebuild and test

The clean A12 source path, Debian/XFCE NFS handoff, framebuffer, touch, Wi-Fi
and Bluetooth were phone-tested on iPhone XR. Reproduce them in this order;
none of the first four steps needs a phone:

1. `bash devices/a12/m1n1/build-m1n1.sh` — rebuild the A12 m1n1 proxy from its pinned
   source and patch series.
2. `bash devices/a12/linux/prepare-kernel.sh` then
   `JOBS=8 bash devices/a12/linux/build-kernel.sh` — fetch the pinned Linux source and
   compile the cleaned N841 patch series. Preserve the produced `Image.gz`
   and `kernel.config` hashes.
3. `bash devices/a12/dts/build-dtb.sh` and
   `bash devices/a12/initramfs/make-initramfs.sh` — build the N841 hardware description
   and a **small recovery initramfs** first, without `devices/a12/build/nfsroot`.
4. With this XR's private IM4M ticket, run
   `python3 devices/a12/boot/prepare-xr-linux-bootset.py --ticket /path/to/apticket.im4m`
   and `XR_IM4M=/path/to/apticket.im4m python3 devices/a12/boot/package-linux.py`.
   The bootset rebuild preserves `boot/inputs/brcm/` radio files.
5. Verify IMG4 payloads and hashes, then boot once with
   `bash devices/a12/boot/boot-xr-linux.sh` and DCSD logging. Confirm m1n1, Linux,
   `/init`, USB ECM, SSH, display and touch before adding a desktop root.
   While USB SSH is up, run `bash devices/a12/host/read-syscfg.sh` to capture this
   XR's private calibration. Then repackage with
   `XR_IM4M=/path/to/apticket.im4m XR_SYSCFG=devices/a12/boot/inputs/syscfg.bin python3 devices/a12/boot/package-linux.py`
   before testing Wi-Fi/Bluetooth.
6. Only after the small boot succeeds, build the Debian/XFCE NFS root using
   [`DEBIAN_XFCE_NFS.md`](DEBIAN_XFCE_NFS.md), enable the host export, rebuild
   the initramfs and Linux IMG4, and test `switch_root`. Then run
   `boot-and-load-xr.sh` to deliver the verified Wi-Fi/Bluetooth firmware.

The source-built, SysCfg-injected payload completed the full automated
boot-and-radio sequence on 2026-11-04. The tested host was an Android phone
running Kali NetHunter with a custom kernel that supports serving NFS. Any
arm64 Linux host works: the initramfs and BlueZ bundle copy the host's own
busybox, dropbear and BlueZ binaries. Apple firmware, device
SysCfg, IM4M, generated payloads and logs remain local and ignored by Git.
Storage writes remain disabled.
