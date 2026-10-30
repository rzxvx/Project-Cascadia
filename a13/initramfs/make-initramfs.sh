#!/usr/bin/env bash
# make-initramfs.sh - tiny arm64 initramfs for the N104AP bring-up boots.
#
# A static busybox and an /init that mounts the basics, prints a marker on the
# console (the screen, via fbcon), then brings up a USB device-mode gadget
# through configfs:
#   - ACM serial  -> shell on /dev/ttyGS0  (Mac: /dev/cu.usbmodem*)
#   - CDC-ECM net -> usb0 = 172.16.42.1/24 + IPv6 link-local fe80::ff:fe00:104;
#     udhcpd hands the Mac 172.16.42.2 (no router option, so the Mac keeps its
#     normal default route); dropbear SSH (root, key-only) + telnetd on it
# and prints what happened with the USB controller on the screen.
#
# SSH: the image itself carries no keys. boot/package.sh adds the user's
# ~/.ssh/*.pub as /root/.ssh/authorized_keys (and the host key below, when it
# exists) in a small second cpio archive. Without a host key, dropbear makes a
# new one at every boot.
#
# Output: a13/build/payload/initramfs.cpio.gz, a13/build/ssh/dropbear_ed25519_host_key
#
# Usage:  bash a13/initramfs/make-initramfs.sh
set -euo pipefail

A13="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$A13/build/payload"
mkdir -p "$OUT" "$A13/build/ssh"

docker run --rm --platform linux/arm64 \
  -v "$OUT":/out \
  -v "$A13/build/ssh":/keys \
  arm64v8/debian:bookworm bash -euxc '
    apt-get update
    apt-get install -y --no-install-recommends busybox-static cpio gzip dropbear-bin
    root=$(mktemp -d)
    mkdir -p "$root"/{bin,sbin,usr/bin,usr/sbin,proc,sys,dev,tmp,etc,root,var/run}
    cp /bin/busybox "$root/bin/busybox"
    ln -sf busybox "$root/bin/sh"
    # dropbear + the shared libraries it links against
    cp /usr/sbin/dropbear "$root/usr/sbin/"
    for lib in $(ldd /usr/sbin/dropbear | grep -o "/[^ ]*"); do
      mkdir -p "$root$(dirname $lib)"; cp -L "$lib" "$root$lib"
    done
    [ -f /keys/dropbear_ed25519_host_key ] || \
      dropbearkey -t ed25519 -f /keys/dropbear_ed25519_host_key
    # a host key for this builder, added by package.sh, not part of the image
    mkdir -p "$root/etc/dropbear" "$root/root/.ssh"
    chmod 700 "$root/root" "$root/root/.ssh"
    echo "root:x:0:0:root:/root:/bin/sh" > "$root/etc/passwd"
    echo "root:x:0:" > "$root/etc/group"
    echo "/bin/sh" > "$root/etc/shells"
    echo "n104" > "$root/etc/hostname"
    cat > "$root/etc/udhcpd.conf" <<"EOF2"
start 172.16.42.2
end 172.16.42.2
interface usb0
max_leases 1
lease_file /tmp/udhcpd.leases
option subnet 255.255.255.0
option lease 86400
EOF2
    cat > "$root/init" <<"EOF"
#!/bin/sh
/bin/busybox --install -s
mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev 2>/dev/null
mount -t configfs none /sys/kernel/config
mount -t tmpfs none /tmp
mkdir -p /dev/pts && mount -t devpts devpts /dev/pts
hostname n104
echo
echo "########################################"
echo "##  N104AP: Linux reached userspace.  ##"
echo "########################################"
uname -a

# USB gadget: wait for the DWC3 UDC to register (dwc3-apple may defer).
udc=""
for i in $(seq 1 30); do
    udc=$(ls /sys/class/udc 2>/dev/null | head -n1)
    [ -n "$udc" ] && break
    sleep 1
done
if [ -z "$udc" ]; then
    echo "[usb] NO UDC after 30s. Kernel USB messages:"
    dmesg | grep -iE "dwc3|usb|ausb|phy|dart|usbcomplex" | tail -n 25
else
    echo "[usb] UDC = $udc"
    G=/sys/kernel/config/usb_gadget/g1
    mkdir -p $G && cd $G
    echo 0x1d6b > idVendor          # Linux Foundation
    echo 0x0104 > idProduct         # Multifunction Composite Gadget
    echo 0x0200 > bcdUSB
    echo 0xEF > bDeviceClass        # composite with IADs (ACM + ECM)
    echo 0x02 > bDeviceSubClass
    echo 0x01 > bDeviceProtocol
    mkdir -p strings/0x409
    echo "N104AP-linux" > strings/0x409/serialnumber
    echo "Linux" > strings/0x409/manufacturer
    echo "iPhone 11 Linux" > strings/0x409/product
    mkdir -p configs/c.1/strings/0x409
    echo "ACM + ECM" > configs/c.1/strings/0x409/configuration
    echo 250 > configs/c.1/MaxPower
    mkdir -p functions/acm.GS0 functions/ecm.usb0
    echo 02:00:00:00:01:04 > functions/ecm.usb0/dev_addr
    echo 02:00:00:00:01:01 > functions/ecm.usb0/host_addr
    ln -s functions/acm.GS0 configs/c.1/
    ln -s functions/ecm.usb0 configs/c.1/
    if echo "$udc" > UDC; then
        echo "[usb] gadget bound: serial ttyGS0 + ecm usb0"
    else
        echo "[usb] binding the gadget FAILED"
    fi
    cd /
    ifconfig usb0 172.16.42.1 netmask 255.255.255.0 up && \
        echo "[usb] usb0 = 172.16.42.1 (Mac gets 172.16.42.2 by DHCP)"
    touch /tmp/udhcpd.leases && udhcpd /etc/udhcpd.conf
    dropbear -R -E 2>/tmp/dropbear.log && echo "[usb] ssh: ssh root@172.16.42.1"
    telnetd -l /bin/sh
    # shell on the USB serial, respawned
    ( while true; do
        setsid sh -c "exec sh -i </dev/ttyGS0 >/dev/ttyGS0 2>&1"
        sleep 1
      done ) &
    sleep 3
    echo "[usb] state: $(cat /sys/class/udc/$udc/state 2>/dev/null)"
    dmesg | grep -iE "dwc3|ausb|gadget" | tail -n 8
fi
echo "[init] shell on console"
exec sh
EOF
    chmod +x "$root/init"
    ( cd "$root" && find . | cpio -o -H newc | gzip -9 ) > /out/initramfs.cpio.gz
    echo "INITRAMFS OK"
  '

echo
ls -la "$OUT/initramfs.cpio.gz"
