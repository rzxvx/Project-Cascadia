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
    # usbwatch: keeps the USB link usable across unplug/replug (the phone
    # Lightning port is flaky). Runs from RAM only -- with an NFS root it is
    # started from a tmpfs (/run/cascadia) with this static busybox, so it
    # keeps working while the cable is out and the root is unreachable.
    cat > "$root/sbin/usbwatch" <<"EOW"
#!/bin/sh
# BB: the busybox to use (the one in /run/cascadia under an NFS root)
BB=${BB:-busybox}
G=/sys/kernel/config/usb_gadget/g1
udc=$($BB ls /sys/class/udc 2>/dev/null | $BB head -n1)
[ -n "$udc" ] || exit 0
bad=0
next=5
while :; do
    $BB sleep 2
    if [ "$($BB cat /sys/class/udc/$udc/state 2>/dev/null)" = configured ]; then
        bad=0
        next=5
    else
        bad=$((bad + 1))
    fi
    # no host for ~10 s: re-plug the gadget in software (unbind and bind
    # the UDC: the controller stops and starts, the pull-up comes back);
    # then every ~30 s while it stays away
    if [ $bad -ge $next ]; then
        echo "usbwatch: no host, re-binding the gadget to $udc" > /dev/kmsg
        echo "" > $G/UDC 2>/dev/null
        $BB sleep 1
        echo "$udc" > $G/UDC 2>/dev/null
        bad=0
        next=15
    fi
    # the address and the DHCP server the host needs to come back
    $BB ifconfig usb0 2>/dev/null | $BB grep -q "inet addr:172.16.42.1 " \
        || $BB ifconfig usb0 172.16.42.1 netmask 255.255.255.0 up
    $BB pidof udhcpd >/dev/null 2>&1 || {
        $BB touch /tmp/udhcpd.leases 2>/dev/null
        $BB udhcpd ${UDHCPD_CONF:-/etc/udhcpd.conf}
    }
done
EOW
    chmod 755 "$root/sbin/usbwatch"
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

# On-screen keyboard, when a13/cascadia periphery put it in (boot/package.sh
# adds it): fbkeyboard draws a keyboard at the bottom of the framebuffer and
# types into the console through uinput. Started here, before an NFS root is
# switched to: it holds the framebuffer, the touchscreen and uinput open, so
# it keeps working after switch_root -- one place for RAM and NFS boots.
# The touchscreen is found by name (fbkeyboard adds an event node of its own).
if [ -x /usr/bin/fbkeyboard ]; then
    ln -sf ld-musl-aarch64.so.1 /lib/libc.musl-aarch64.so.1
    ts=
    for i in 1 2 3 4 5 6; do
        for n in /sys/class/input/event*; do
            [ "$(cat "$n/device/name" 2>/dev/null)" = "Apple iPhone touchscreen" ] \
                && ts=/dev/input/${n##*/} && break
        done
        [ -n "$ts" ] && break
        sleep 1
    done
    if [ -n "$ts" ]; then
        setsid /usr/bin/fbkeyboard -d "$ts" -f /usr/share/fonts/dejavu/DejaVuSans.ttf \
            >/dev/null 2>&1 </dev/null &
        echo "[kbd] fbkeyboard on $ts"
    else
        echo "[kbd] fbkeyboard: no touchscreen"
    fi
fi

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

    # NFS root: /etc/nfsroot ("host:/path", added by boot/package.sh) names
    # the host export. Wait for the host to take its DHCP address, mount over
    # NFSv3 (macOS nfsd has no v4), hand over to the root and its own init.
    # Anything that fails here stays in RAM: an initramfs that always boots
    # beats a root that sometimes does.
    if [ -f /etc/nfsroot ]; then
        NFSROOT=$(cat /etc/nfsroot)
        HOST=${NFSROOT%%:*}
        echo "[nfs] waiting for the host $HOST"
        for i in $(seq 1 30); do
            ping -c 1 -W 1 "$HOST" >/dev/null 2>&1 && break
        done
        mkdir -p /newroot
        nfs_ok=0
        for i in 1 2 3 4 5; do
            if mount -t nfs -o vers=3,proto=tcp,mountproto=tcp,nolock,addr=$HOST,mountaddr=$HOST,rsize=65536,wsize=65536,timeo=50,retrans=2 \
                    "$NFSROOT" /newroot; then
                nfs_ok=1; break
            fi
            sleep 2
        done
        if [ $nfs_ok = 1 ] && [ -x /newroot/sbin/init ]; then
            echo "[nfs] root $NFSROOT mounted"
            # who may log in, and the host key, come from this image
            mkdir -p /newroot/root/.ssh /newroot/etc/dropbear
            [ -s /root/.ssh/authorized_keys ] && \
                cp /root/.ssh/authorized_keys /newroot/root/.ssh/authorized_keys
            [ -f /etc/dropbear/dropbear_ed25519_host_key ] && \
                cp /etc/dropbear/dropbear_ed25519_host_key /newroot/etc/dropbear/
            chown 0:0 /newroot/root /newroot/root/.ssh /newroot/root/.ssh/authorized_keys 2>/dev/null
            chmod 700 /newroot/root /newroot/root/.ssh
            chmod 600 /newroot/root/.ssh/authorized_keys 2>/dev/null
            killall udhcpd 2>/dev/null
            umount /tmp
            mkdir -p /newroot/dev /newroot/proc /newroot/sys
            mount --move /dev /newroot/dev
            mount --move /proc /newroot/proc
            mount --move /sys /newroot/sys
            # the USB watchdog lives in RAM (a tmpfs /run, which a13-init
            # keeps), chrooted into the new root: it must not need NFS
            mkdir -p /newroot/run
            mount -t tmpfs -o mode=755 tmpfs /newroot/run
            mkdir -p /newroot/run/cascadia
            cp /bin/busybox /sbin/usbwatch /etc/udhcpd.conf /newroot/run/cascadia/
            BB=/run/cascadia/busybox UDHCPD_CONF=/run/cascadia/udhcpd.conf \
                setsid chroot /newroot /run/cascadia/busybox sh /run/cascadia/usbwatch \
                </dev/null >/dev/null 2>&1 &
            exec switch_root /newroot /sbin/init
        fi
        echo "[nfs] no NFS root (mounted=$nfs_ok) - staying in RAM"
        umount /newroot 2>/dev/null
    fi

    setsid /sbin/usbwatch </dev/null >/dev/null 2>&1 &
    dropbear -R -E 2>/tmp/dropbear.log && echo "[usb] ssh: ssh root@172.16.42.1"
    telnetd -l /bin/sh
    # shell on the USB serial, respawned; cttyhack makes ttyGS0 its
    # controlling terminal (job control, Ctrl-C)
    ( while true; do
        setsid cttyhack sh -l </dev/ttyGS0 >/dev/ttyGS0 2>&1
        sleep 1
      done ) &
    sleep 3
    echo "[usb] state: $(cat /sys/class/udc/$udc/state 2>/dev/null)"
    dmesg | grep -iE "dwc3|ausb|gadget" | tail -n 8
fi
# shell on the screen (tty1, where fbkeyboard types), respawned, with tty1
# as its controlling terminal
echo "[init] shell on the screen (tty1)"
while true; do
    setsid cttyhack sh -l </dev/tty1 >/dev/tty1 2>&1
    sleep 1
done
EOF
    chmod +x "$root/init"
    ( cd "$root" && find . | cpio -o -H newc | gzip -9 ) > /out/initramfs.cpio.gz
    echo "INITRAMFS OK"
  '

echo
ls -la "$OUT/initramfs.cpio.gz"
