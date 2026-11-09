# Debian and XFCE over NFS

m1n1 carries a small initramfs, not the desktop. Once USB ECM is up (phone
`172.16.42.1`, host `172.16.42.2`) the initramfs mounts the host's Debian root
over NFSv3 and switches to systemd. If the mount fails it stays in its
recovery shell.

The host needs USB networking, an NFSv3 server over TCP and SSH. Build the
initramfs on arm64, since it copies the host's busybox and dropbear. On
another architecture, `debootstrap` and the chroot steps also need
qemu-user-static.

## Host

1. Build the root. XFCE with LightDM is the default, `XR_DESKTOP=gnome`
   gives GNOME with GDM. It needs `debootstrap`, network access and a few GB
   of disk, and the touch firmware in `build/touch/` (see
   [FIRMWARE.md](FIRMWARE.md)).

   ```sh
   sudo XR_DESKTOP=xfce bash devices/a12/rootfs/build-debian-rootfs.sh
   ```

   The output is `build/debian-xr.tar.gz`.

2. Export it:

   ```sh
   sudo bash devices/a12/host/linux-nfs-export.sh on
   bash devices/a12/host/linux-nfs-export.sh status
   ```

   It unpacks to `/srv/cascadia-xr-debian` if that is empty
   (`FORCE_SYNC=1` replaces it), exports it to `172.16.42.0/24` and writes
   `build/nfsroot`, which the initramfs picks up. The A13 and A5 exports are
   not touched.

3. Rebuild the initramfs after the export, then pack:

   ```sh
   bash devices/a12/initramfs/make-initramfs.sh
   bash devices/a12/dts/build-dtb.sh
   XR_IM4M=/path/to/apticket.im4m XR_SYSCFG=/path/to/syscfg.bin \
     python3 devices/a12/boot/package-linux.py
   ```

   m1n1 and `Image.gz` must be built already. SysCfg is only read to fill in
   the DTB, it is never copied into the tree.

When the phone's ECM interface shows up in `ip -br link`, give the host its
address. Do it again if the interface comes back after a USB reset.

```sh
IFACE=enx...   # the phone's interface
sudo ip addr replace 172.16.42.2/24 dev "$IFACE"
sudo ip link set "$IFACE" up
```

## Phone

From DFU:

```sh
bash devices/a12/boot/boot-and-load-xr.sh
```

It boots, waits for Debian's SSH (`/etc/debian_version`, not just the
initramfs), then loads the Wi-Fi and Bluetooth firmware.

The initramfs has the building user's `~/.ssh/*.pub` as root's keys and
copies them, and its Dropbear host key, into Debian before switching:

```sh
ssh -F /dev/null -o StrictHostKeyChecking=no \
  -o UserKnownHostsFile=/dev/null root@172.16.42.1
```

The serial log shows `[nfs] mounted ...` and the switch. Then check
`/etc/debian_version`, `systemctl status lightdm`, `/dev/fb0`, touch,
NetworkManager and Bluetooth. XFCE runs on the framebuffer with touch; small
window controls and the on-screen keyboard are fiddly.

If NFS fails, check the host's `172.16.42.2`, `exportfs -v`, the path in
`build/nfsroot` and the serial log. `sudo bash
devices/a12/host/linux-nfs-export.sh off` removes only the XR export.

## Initramfs SError

Some initramfs gzip streams make m1n1 raise an L2 SError in `tinf_crc32`
while it checks the unpacked archive, before Linux starts. The same files
packed with other CPIO metadata boot. `make-initramfs.sh` therefore builds a
reproducible archive: fixed mtimes (`SOURCE_DATE_EPOCH`, default
1700000000), sorted names, `cpio --reproducible` and `gzip -n`. Why one
stream faults is not known. The tests are in [HISTORY.md](HISTORY.md).
