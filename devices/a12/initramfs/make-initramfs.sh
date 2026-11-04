#!/usr/bin/env bash
set -euo pipefail
A12="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$A12/build/payload"
ROOT="$A12/build/initramfs-root"
rm -rf "$ROOT"
mkdir -p "$ROOT"/{bin,sbin,usr/bin,usr/sbin,proc,sys,dev,tmp,etc/dropbear,etc/bluetooth,etc/dbus-1/system.d,run,var/run,root/.ssh,usr/share/dbus-1/system.d,usr/share/dbus-1/system-services}
cp /usr/bin/busybox "$ROOT/bin/busybox"
ln -s busybox "$ROOT/bin/sh"
copy_deps() {
    ldd "$1" | awk '{for(i=1;i<=NF;i++) if($i ~ /^\//) print $i}' | sort -u | while read -r lib; do
        mkdir -p "$ROOT$(dirname "$lib")"
        cp -L "$lib" "$ROOT$lib"
    done
}
copy_deps /usr/bin/busybox

# Key-only SSH over USB Ethernet.  Keep the host identity stable between
# initramfs rebuilds, while accepting every public key already authorized on
# this development host.
install -m 755 /usr/sbin/dropbear "$ROOT/usr/sbin/dropbear"
copy_deps /usr/sbin/dropbear
mkdir -p "$A12/build/ssh"
HOSTKEY="$A12/build/ssh/dropbear_ed25519_host_key"
[ -s "$HOSTKEY" ] || /usr/bin/dropbearkey -t ed25519 -f "$HOSTKEY" >/dev/null
install -m 600 "$HOSTKEY" "$ROOT/etc/dropbear/dropbear_ed25519_host_key"
find /root/.ssh -maxdepth 1 -name '*.pub' -type f -exec cat {} + > "$ROOT/root/.ssh/authorized_keys"
chmod 700 "$ROOT/root" "$ROOT/root/.ssh"
chmod 600 "$ROOT/root/.ssh/authorized_keys"
cat > "$ROOT/etc/passwd" <<'EOF'
root:x:0:0:root:/root:/bin/sh
messagebus:x:100:100:D-Bus system bus:/run/dbus:/sbin/nologin
EOF
cat > "$ROOT/etc/group" <<'EOF'
root:x:0:
messagebus:x:100:
bluetooth:x:101:
EOF
echo /bin/sh > "$ROOT/etc/shells"
[ ! -s "$A12/build/nfsroot" ] || install -m 644 "$A12/build/nfsroot" "$ROOT/etc/nfsroot"
cat > "$ROOT/etc/udhcpd.conf" <<'EOF'
start 172.16.42.2
end 172.16.42.2
interface usb0
max_leases 1
lease_file /tmp/udhcpd.leases
option subnet 255.255.255.0
option lease 86400
EOF

cat > "$ROOT/sbin/usbwatch" <<'WATCH'
#!/bin/sh
BB=${BB:-busybox}
UDHCPD_CONF=${UDHCPD_CONF:-/etc/udhcpd.conf}
G=/sys/kernel/config/usb_gadget/g1
udc=$($BB ls /sys/class/udc 2>/dev/null | $BB head -n1)
[ -n "$udc" ] || exit 0
bad=0
next=5
$BB killall udhcpd 2>/dev/null || true
dhcp=
while :; do
    $BB sleep 2
    if [ "$($BB cat /sys/class/udc/$udc/state 2>/dev/null)" = configured ]; then
        bad=0; next=5
    else
        bad=$((bad + 1))
    fi
    if [ "$bad" -ge "$next" ]; then
        echo "" > "$G/UDC" 2>/dev/null || true
        $BB sleep 1
        echo "$udc" > "$G/UDC" 2>/dev/null || true
        bad=0; next=15
    fi
    $BB ifconfig usb0 2>/dev/null | $BB grep -q '172.16.42.1' || \
        $BB ifconfig usb0 172.16.42.1 netmask 255.255.255.0 up 2>/dev/null
    [ -n "$dhcp" ] && kill -0 "$dhcp" 2>/dev/null || {
        $BB touch /tmp/udhcpd.leases
        $BB udhcpd -f "$UDHCPD_CONF" </dev/null >/dev/null 2>&1 &
        dhcp=$!
    }
done
WATCH
chmod 755 "$ROOT/sbin/usbwatch"

# Matching XR touch firmware, generated from N841_Multitouch.im4p.  The
# kernel driver will keep retrying harmlessly if a diagnostic build omits it.
if [ -s "$A12/build/touch/n841-mtfw.bin" ]; then
    mkdir -p "$ROOT/lib/firmware/apple"
    install -m 644 "$A12/build/touch/n841-mtfw.bin" \
        "$ROOT/lib/firmware/apple/n841-mtfw.bin"
    install -m 644 "$A12/build/touch/n841-mtcal.bin" \
        "$ROOT/lib/firmware/apple/n841-mtcal.bin"
fi

# XR BCM4357 B1 (yodak) firmware extracted from the matching iOS root.  The
# kernel's firmware fallback is disabled, so a missing board-specific name
# fails immediately instead of stalling boot for a minute.
if [ "${XR_EMBED_BRCM_FIRMWARE:-0}" = 1 ] && [ -d "$A12/boot/inputs/brcm" ]; then
    mkdir -p "$ROOT/lib/firmware/brcm"
    find "$A12/boot/inputs/brcm" -maxdepth 1 -type f -exec \
        install -m 644 {} "$ROOT/lib/firmware/brcm/" \;
fi

cat > "$ROOT/init" <<'INIT'
#!/bin/sh
/bin/busybox --install -s
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev 2>/dev/null || true
mount -t configfs configfs /sys/kernel/config
mount -t tmpfs tmpfs /tmp
mkdir -p /dev/pts
mount -t devpts devpts /dev/pts
hostname xr-linux
echo
echo '########################################'
echo '## Project Cascadia XR reached /init  ##'
echo '########################################'
uname -a
echo '[xr] CPUs:'; grep -E 'processor|model name' /proc/cpuinfo

# The Hoolock base config enables the legacy g_multi gadget, but that tries to
# bind before the Apple DWC2 UDC exists and its ACM+ECM+mass-storage layout is
# too large for this controller.  The kernel build disables it.  Build only
# the two interfaces we need after the UDC has registered, as Cascadia's A13
# initramfs does.
udc=""
for i in $(seq 1 30); do
    udc=$(ls /sys/class/udc 2>/dev/null | head -n1)
    [ -n "$udc" ] && break
    sleep 1
done

if [ -n "$udc" ]; then
    echo "[usb] UDC = $udc"
    G=/sys/kernel/config/usb_gadget/g1
    mkdir -p "$G"
    cd "$G"
    echo 0x1d6b > idVendor
    echo 0x0104 > idProduct
    echo 0x0200 > bcdUSB
    echo 0xEF > bDeviceClass
    echo 0x02 > bDeviceSubClass
    echo 0x01 > bDeviceProtocol
    mkdir -p strings/0x409 configs/c.1/strings/0x409
    echo XR-Linux > strings/0x409/serialnumber
    echo 'Project Cascadia' > strings/0x409/manufacturer
    echo 'iPhone XR Linux' > strings/0x409/product
    echo 'ACM + ECM' > configs/c.1/strings/0x409/configuration
    echo 250 > configs/c.1/MaxPower
    mkdir -p functions/acm.GS0 functions/ecm.usb0
    echo 02:00:00:00:02:04 > functions/ecm.usb0/dev_addr
    echo 02:00:00:00:02:01 > functions/ecm.usb0/host_addr
    ln -s functions/acm.GS0 configs/c.1/
    ln -s functions/ecm.usb0 configs/c.1/
    if echo "$udc" > UDC; then
        echo '[usb] gadget bound: ttyGS0 + usb0'
        cd /
        ifconfig usb0 172.16.42.1 netmask 255.255.255.0 up 2>/dev/null || true
        touch /tmp/udhcpd.leases
        setsid /sbin/usbwatch </dev/null >/dev/null 2>&1 &
        /usr/sbin/dropbear -R -E -s -p 22 2>/tmp/dropbear.log && \
            echo '[usb] SSH ready: ssh root@172.16.42.1'
        (
            while true; do
                [ -e /dev/ttyGS0 ] && \
                    setsid sh -c 'exec sh -l </dev/ttyGS0 >/dev/ttyGS0 2>&1'
                sleep 1
            done
        ) &

        # The iPhone 11 port keeps Alpine outside rkrn and switches to a host
        # NFS export after USB ECM is ready.  Use the same proven NFSv3 flow
        # on XR; any failure leaves this recovery initramfs running.
        if [ -s /etc/nfsroot ]; then
            NFSROOT=$(cat /etc/nfsroot)
            HOST=${NFSROOT%%:*}
            echo "[nfs] waiting for $HOST"
            for i in $(seq 1 30); do
                ping -c 1 -W 1 "$HOST" >/dev/null 2>&1 && break
                sleep 1
            done
            mkdir -p /newroot
            nfs_ok=0
            for i in 1 2 3 4 5; do
                if mount -t nfs -o vers=3,proto=tcp,mountproto=tcp,nolock,addr=$HOST,mountaddr=$HOST,rsize=65536,wsize=65536,timeo=50,retrans=2 "$NFSROOT" /newroot; then
                    nfs_ok=1
                    break
                fi
                sleep 2
            done
            if [ "$nfs_ok" = 1 ] && [ -x /newroot/sbin/init ]; then
                echo "[nfs] mounted $NFSROOT"
                mkdir -p /newroot/root/.ssh /newroot/etc/dropbear
                cp /root/.ssh/authorized_keys /newroot/root/.ssh/authorized_keys
                cp /etc/dropbear/dropbear_ed25519_host_key /newroot/etc/dropbear/
                chmod 700 /newroot/root /newroot/root/.ssh
                chmod 600 /newroot/root/.ssh/authorized_keys
                killall dropbear 2>/dev/null || true
                mkdir -p /newroot/dev /newroot/proc /newroot/sys /newroot/run
                mount -t tmpfs -o mode=755 tmpfs /newroot/run
                mkdir -p /newroot/run/cascadia
                mount --move /dev /newroot/dev
                mount --move /proc /newroot/proc
                mount --move /sys /newroot/sys
                killall usbwatch 2>/dev/null || true
                cp /bin/busybox /sbin/usbwatch /etc/udhcpd.conf /newroot/run/cascadia/
                BB=/run/cascadia/busybox UDHCPD_CONF=/run/cascadia/udhcpd.conf \
                    setsid chroot /newroot /run/cascadia/busybox sh /run/cascadia/usbwatch </dev/null >/dev/null 2>&1 &
                exec switch_root /newroot /sbin/init
            fi
            echo "[nfs] mount failed; staying in recovery initramfs"
            umount /newroot 2>/dev/null || true
        fi
    else
        echo '[usb] gadget bind failed'
        cd /
    fi
else
    echo '[usb] no UDC after 30 seconds'
    dmesg | grep -iE 'dwc2|usb|ausb|phy|dart' | tail -n 30
fi

echo '[init] framebuffer shell'
exec setsid cttyhack sh
INIT
chmod 755 "$ROOT/init"
mkdir -p "$OUT"
# Do not copy host filesystem inode numbers and build-time mtimes into the
# archive.  Besides making the payload non-reproducible, one such regenerated
# stream reproducibly triggered an A12 m1n1 L2 SError during gzip CRC
# verification.  This exact normalized form is phone-boot tested on N841.
SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-1700000000}"
case "$SOURCE_DATE_EPOCH" in
    ''|*[!0-9]*) echo "invalid SOURCE_DATE_EPOCH: $SOURCE_DATE_EPOCH" >&2; exit 1 ;;
esac
find "$ROOT" -exec touch -h -d "@$SOURCE_DATE_EPOCH" {} +
(cd "$ROOT" && LC_ALL=C find . -print0 | LC_ALL=C sort -z | \
    cpio --null --reproducible -o -H newc 2>/dev/null | gzip -9 -n) \
    > "$OUT/initramfs.cpio.gz"
echo "XR initramfs OK: $OUT/initramfs.cpio.gz"
