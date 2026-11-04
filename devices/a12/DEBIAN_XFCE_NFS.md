# XR Debian/XFCE over USB NFS

This follows Cascadia's split-root approach: m1n1 carries a small Linux
initramfs, not a desktop. USB ECM comes up at `172.16.42.1` on the XR;
the host is `172.16.42.2`. The initramfs mounts the host's Debian root over
NFSv3 and `switch_root`s into systemd. This is the **A12/N841** path only.

The documented phone test used an Android phone as the host, running Kali
NetHunter with a custom kernel that provides NFS server support. A conventional
Linux PC works as well; the requirements are USB host networking, NFSv3/TCP,
SSH, and the build/boot tools listed below.

## Prepare the host (no phone boot yet)

1. Build the Debian trixie ARM64 tarball. XFCE/LightDM is the default profile;
   `XR_DESKTOP=gnome` selects the older GNOME/GDM profile instead:

   ```sh
   sudo XR_DESKTOP=xfce bash devices/a12/rootfs/build-debian-rootfs.sh
   ```

   This requires `debootstrap`, network access and substantial temporary disk
   space. Output is `devices/a12/build/debian-xr.tar.gz`; no rootfs goes in Git.
   Touch firmware must first exist at `devices/a12/build/touch/` (see FIRMWARE.md).

2. Start the XR-only export:

   ```sh
   sudo bash devices/a12/host/linux-nfs-export.sh on
   bash devices/a12/host/linux-nfs-export.sh status
   ```

   The default destination is `/srv/cascadia-xr-debian`, exported to
   `172.16.42.0/24`. `on` populates it from the tarball if it is empty;
   `FORCE_SYNC=1` deliberately replaces an existing destination. The script
   also writes `devices/a12/build/nfsroot`, which the initramfs builder copies into
   `/etc/nfsroot`. Once the phone enumerates its USB ECM gadget, identify the
   new host interface with `ip -br link` and assign the host address (replace
   `YOUR_ECM_INTERFACE` with that interface's name):

   ```sh
   IFACE=YOUR_ECM_INTERFACE
   sudo ip addr replace 172.16.42.2/24 dev "$IFACE"
   sudo ip link set "$IFACE" up
   ip -br addr show dev "$IFACE"
   ```

   Keep the NFS export enabled before boot; repeat the address assignment
   after host-side USB re-enumeration if the interface loses it.
   Avoid changing an A13 or A5 export; those use separate paths.

3. Build the small initramfs **after** the export has written `nfsroot`, then
   build the N841 DTB and package the Linux IMG4 using your own IM4M:

   ```sh
   bash devices/a12/initramfs/make-initramfs.sh
   bash devices/a12/dts/build-dtb.sh
   XR_IM4M=/path/to/this-XR/apticket.im4m \
     XR_SYSCFG=/path/to/this-XR/syscfg.bin \
     python3 devices/a12/boot/package-linux.py
   ```

   The m1n1 Mach-O and `Image.gz` must already have been built. The SysCfg is
   private per-phone; it is read for temporary DTB injection, never copied
   into the repository. Keep generated IMG4s and the ticket untracked.

## Test on XR

Boot from DFU with the A12 boot script and DCSD logger. It uploads the
previously prepared iBSS, pristine Apple DeviceTree, trust cache, and Linux
IMG4 in order. `boot-and-load-xr.sh` then waits for Debian USB SSH and loads
Wi-Fi/Bluetooth firmware separately. Do not use the A13 or A5 boot scripts.

```sh
bash devices/a12/boot/boot-and-load-xr.sh
```

The initramfs starts Dropbear on the phone at `172.16.42.1:22` and embeds the
host's `/root/.ssh/*.pub` keys as `root` authorized keys when the initramfs is
built. Use the matching private key to inspect the running system:

```sh
ssh -F /dev/null -o StrictHostKeyChecking=no \
  -o UserKnownHostsFile=/dev/null root@172.16.42.1
```

After a successful NFS mount, the initramfs copies those authorized keys and
the Dropbear host key into Debian, transfers the USB watchdog, and
`switch_root`s into systemd. `boot-and-load-xr.sh` waits for
`/etc/debian_version` before loading either radio firmware, so a working SSH
connection to the initramfs alone does not imply that Debian mounted.

Check the serial log for `[nfs] mounted ...` and `switch_root`; then check
`/etc/debian_version`, `systemctl status lightdm`, Xorg, `/dev/fb0`, touch,
NetworkManager, and Bluetooth. The user should see the XFCE desktop. XFCE
was phone-tested through the framebuffer with multitouch. Notch-safe panel
placement and touch behavior were adjusted during the session; small window
controls and the on-screen keyboard remain less polished than a native mobile
shell.

If NFS fails, the initramfs stays in its recovery shell rather than starting
Debian. Diagnose the host's `172.16.42.2` address, `exportfs -v`, NFSv3/TCP,
the path in `devices/a12/build/nfsroot`, and the DCSD log. After testing,
`sudo bash devices/a12/host/linux-nfs-export.sh off` removes only the XR export.

### XR test result (2026-11-04)

The NFS-capable kernel plus the already-built NFS initramfs booted from DFU
without flashing. This controlled test used the base N841 Linux DTB (no
per-device SysCfg injection); the temporary IMG4 had SHA256
`62e486a7e8df76123962487c74f9ab68eb9c43b70cb253cc1f8cab3b765115f8`.
DCSD log: `devices/a12/boot/logs/xr-linux-20261104-025238.log` (local, ignored).
Serial shows both gzip payloads decompressed, Linux started, and Debian
`systemd` became PID 1. USB SSH returned Debian 13.7; the root mount source
was `172.16.42.2:/srv/cascadia-xr-debian` (NFS). `lightdm`, Xorg, and XFCE
session processes (`xfce4-session`, `xfwm4`, `xfce4-panel`, `xfdesktop`) ran;
`/dev/fb0` and `/dev/input/event1` existed. Visual quality and touch behavior
in this Debian/XFCE session have not yet been checked by the user. Wi-Fi and
Bluetooth were not tested in this boot because the base DTB lacks private
SysCfg board data. In this boot, both firmware loaders were run manually
**after** Debian started: Broadcom Wi-Fi firmware loaded and `wlp1s0`
appeared; the Bluetooth HCD patched `hci0`, `bluetooth.service` was active,
and the controller reported powered on. Wi-Fi association, Bluetooth scanning,
and radio stability were not tested; the missing SysCfg data may still matter.

An earlier SysCfg-injected NFS package returned to recovery after an m1n1
`SError` while decompressing the initramfs, before Linux or NFS started.
The same NFS initramfs succeeded with the base DTB in this test, so do not
attribute that earlier failure to NFS itself. The exact trigger remains
unproven; compare payload placement and m1n1 heap addresses before changing
the initramfs or kernel.

Follow-up A/B test: the exact SysCfg-injected DTB from the failed package
combined with the unchanged boot-proven recovery initramfs also decompressed
and reached Linux/USB SSH (`devices/a12/boot/logs/xr-linux-20261104-030237.log`,
local/ignored). Its temporary IMG4 SHA256 was
`d88c3787245c3cef6c14eeb5ad92c778655225a2a9255fde309c8bb8b4462068`.
Thus neither the SysCfg DTB nor the NFS initramfs fails independently.
Historical `devices/a12/LINUX_PLAN.md` also documents earlier successful boots with
SysCfg-injected images. Repeat the **exact** combined failed image before
claiming a deterministic interaction; the one SError could also depend on
m1n1's per-boot payload/heap placement. The packer concatenates components
without alignment or padding, so changing DTB size shifts both gzip streams.

The exact combined image was repeated and failed again at the same m1n1
instruction (`tinf_crc32` + `0x20`, an `ldrb` reading the already-inflated
initramfs during gzip CRC verification); both failures reported an asynchronous
`SError` and L2 cache error. This is reproducible for that IMG4, not an NFS
mount error. The old and new `make-initramfs.sh` scripts are byte-identical.
The old NFS archive and new NFS archive contain the same files and file data
except for the generated Dropbear host key; every CPIO entry has a new mtime.
Both decompress to 5,389,824 bytes, but their gzip streams differ (old
2,581,009 bytes, new 2,581,054 bytes). A temporary IMG4 with the identical
m1n1, SysCfg DTB and kernel but yesterday's exact NFS gzip stream is staged
at `/tmp/xr-syscfg-yesterday-nfs.img4` (SHA256
`73c9fd7ee25c971fb26b9876821da165d01d6c320fba2c5d9ad8e78e27c222a2`).
**Phone-tested successfully:** the old-gzip image decompressed both payloads,
mounted Debian over NFS, and ran systemd, Xorg and XFCE. DCSD log:
`devices/a12/boot/logs/xr-linux-20261104-031024.log` (local/ignored). Wi-Fi firmware
was loaded and `wlp1s0` appeared; the Bluetooth HCD loaded, `hci0` reached
build 1072 and `bluetooth.service` was active. The two repeat failures used
the newly regenerated gzip stream, while this success changed only that
stream. This isolates the **failing artifact** to the regenerated initramfs
gzip bytes, not the source script, DTB, kernel or NFS server. The exact
low-level reason m1n1's CRC read raises an L2 `SError` for one gzip stream
but not the other is still unknown. Do not claim the Dropbear key or mtimes
individually are the cause until separately tested.

Two hybrid archives were then produced to separate the only remaining archive
differences: the 83-byte generated Dropbear host key and 503 bytes of CPIO
inode/mtime header fields. The image using yesterday's CPIO headers with
today's newly generated host key (`/tmp/xr-old-headers-new-key.img4`, SHA256
`9264fe74e651b3bfe94a8ca838ff22ac18c62f73b3103c4643f5977373aab7a3`)
**booted Linux successfully**; log
`devices/a12/boot/logs/xr-linux-20261104-032952.log`. This rules out the new host key
and rules out an extra embedded file. The regenerated CPIO metadata/its
resulting compressed stream is the trigger. At the recurrent CRC fault offset
`0x39588c`, both uncompressed archives contain identical bytes inside
`usr/lib/aarch64-linux-gnu/libgmp.so.10`; the differing metadata is earlier in
the stream, so the asynchronous fault is not a direct read of a changed CPIO
header. A reproducible archive-generation fix still needs a phone test rather
than assuming any newly normalized metadata pattern is safe.

That fix was subsequently tested. `make-initramfs.sh` now fixes all archive
mtimes to `SOURCE_DATE_EPOCH` (default `1700000000`), sorts path names, uses
GNU cpio's `--reproducible` mode, and uses `gzip -n`. The resulting stream was
5,389,824 bytes uncompressed and 2,580,272 bytes compressed (SHA256
`7c347e6615fcd128acff6dfe2e1287ae5509b7ebc47f893500e72d7091431784`).
The ticketed test image `/tmp/xr-repro-initramfs.img4` (SHA256
`e9618a2a70fc49f9d276c679fa4751e1f041c224508527f89adc010a534f271d`)
booted m1n1, decompressed both payloads without an SError, entered Linux, and
ran `/init`. DCSD log: `devices/a12/boot/logs/xr-linux-20261104-033251.log`.

The same deterministic archive was then rebuilt from the source script and
packaged with the XR SysCfg-derived Wi-Fi calibration/MAC and Bluetooth
address. The source-tree candidate is `devices/a12/boot/inputs/m1n1-linux.img4`,
SHA256 `e9618a2a70fc49f9d276c679fa4751e1f041c224508527f89adc010a534f271d`;
it is byte-identical to the phone-tested image. Use
`devices/a12/boot/boot-and-load-xr.sh` for the proven automated order: boot with DCSD
logging, wait for the Debian NFS `switch_root`, then transfer/rebind Wi-Fi and
Bluetooth firmware over USB SSH. The radio blobs intentionally remain outside
the initramfs.

The complete `boot-and-load-xr.sh` automation was phone-tested from DFU with
this source-tree image. DCSD log:
`devices/a12/boot/logs/xr-linux-20261104-033744.log`. It reached Debian 13.7, then
loaded BCM4357 Wi-Fi firmware (`13.10.706.0.0.0.88`) and the Bluetooth HCD
(build 1072). `wlp1s0` was present under NetworkManager; `hci0` was powered,
Bluetooth service was active, and the controller reported its public address.
Wi-Fi remained unassociated (`NO-CARRIER`) because this test verified loading,
not joining an access point.
