# Debian/XFCE over USB NFS — XR and iPad

Use [QUICKSTART.md](QUICKSTART.md) for the complete sequence. Select `A12_DEVICE=xr`
or `A12_DEVICE=ipad8` before these commands. The new separated source workflow
still needs a fresh end-to-end build/boot test; earlier lab boots are separate
evidence.

## One root, reused sequentially

| Selected fresh profile | Default account | Hostname | Default export |
|---|---|---|---|
| xr | xr | xr-debian | /srv/cascadia-xr-debian |
| ipad8 | ipad | ipad8-debian | /srv/cascadia-ipad8-debian |

A profile does not create another root merely by being selected. For a **new**
installation only, run the builder; it replaces the selected staging directory:

```sh
sudo -E bash devices/a12/rootfs/build-debian-rootfs.sh
```

It produces `build/debian-<device>.tar.gz`. Password login is the default, with
an interactive password prompt. There is no shipped password or automatic sudo
membership. `A12_AUTOLOGIN=1` explicitly selects passwordless bench autologin.
The graphical login keyboard is Onboard; root SSH is a separate key-only service.

For an existing populated root, **skip building** and export it in place:

```sh
sudo -E env DST=/srv/cascadia-ipad8-debian \
  bash devices/a12/host/linux-nfs-export.sh on
```

This example can reuse that directory even for a sequential XR test. Its existing
`ipad` account/hostname remain unchanged; it is not converted into a fresh XR root.
Only the device payload/calibration must change. Do not run the profile installer
on an active root just to switch boards.

Before changing which profile exports the same directory, turn the old export off:

```sh
A12_DEVICE=ipad8 sudo -E bash devices/a12/host/linux-nfs-export.sh off
# Then select xr and enable its export with DST pointing to the same existing root.
```

`off` removes that profile's export/address file, not the desktop directory.
Never boot two devices on the same writable root concurrently. `FORCE_SYNC=1`
explicitly replaces the destination; do not use it to reuse a working root.

## NFS and host networking

The phone/tablet is `172.16.42.1`; the host ECM interface is `172.16.42.2/24`.
The host exports NFSv3/TCP to `172.16.42.0/24` with no_root_squash. The export helper
starts the relevant NFS/rpc services on Linux when supported. On NetHunter the
host kernel itself must provide NFS server support.

When ECM appears, identify the correct host interface (`ip -br addr`, MAC
`02:00:00:00:02:01`) and give it the host address if DHCP has not done so:

```sh
IFACE=YOUR_ECM_INTERFACE
sudo ip addr replace 172.16.42.2/24 dev "$IFACE"
sudo ip link set "$IFACE" up
```

The exporter writes `build/devices/<device>/nfsroot`; the selected initramfs embeds
that address. Enable the export **before** building initramfs and packing. Changing
the directory/configuration after packaging does not change the image; rebuild
initramfs and repack. The new packer/boot tool rejects stale selected NFS metadata.

## Desktop and Internet

NetworkManager/nm-applet manage Wi-Fi; polkit/lxpolkit authorize active local
`netdev` users. Wi-Fi DHCP supplies DNS. There is no need to replace working DNS
just because a particular website has no address. `systemd-timesyncd` synchronizes
the clock when online; bad early RTC time can otherwise break APT/TLS verification.
Do not disable repository signatures to work around time errors.

APT needs root privileges. A newly built desktop user is not automatically a
sudoer; use the root SSH session, or deliberately configure sudo/password policy.
The experimental framebuffer desktop is not a claim of accelerated graphics.
See [rootfs/PROFILES.md](rootfs/PROFILES.md) for login and keyboard details.

## USB watchdog and unplug/replug

Both profiles default to the tested A13-style initramfs handoff. Static BusyBox,
the watchdog and DHCP config are copied into tmpfs `/run/cascadia` before
`switch_root`. DHCP alone runs during NFS discovery; cleanup happens before
moving `/proc`; one RAM-resident worker owns DHCP after handoff. The Debian
replacement service intentionally skips boots containing the handoff marker.
See [rootfs/PROFILES.md](rootfs/PROFILES.md) for exact checks and limitations.

```sh
ssh root@172.16.42.1 'test -e /run/cascadia/a13-usbwatch && echo A13_HANDOFF; ps -eo pid,ppid,args | grep -E "[u]sbwatch|[u]dhcpd"; cat /sys/class/udc/*/state'
```

Expect one worker and one DHCP child. Unplug for 15 seconds, reconnect, then
check SSH, NFS and an application. This upstream loop does not write a watchdog
log; an inactive `cascadia-usbwatch.service` is expected in handoff mode.

Fresh handoff images passed automatic reconnect tests on XR and iPad, including
repeat iPad boots/replugs. An earlier iPad timeout means intermittent recovery
is not ruled out. No manual cleanup was required in the corrected passing tests.

**This is not cable-free Debian.** A hard USB NFS root can stall applications
while disconnected, even if Wi-Fi works. Recovery restores its transport.

## Keep test evidence distinct

The earlier XR source-built desktop and later J171 laboratory images demonstrated
hardware functionality. Reusing those images to test this persistent root does
not validate a rebuilt kernel, m1n1 or newly generated initramfs. For release/PR
validation, record source commit, clean-build commands, input/manifest hashes,
serial logs and hardware results for each selected board.

## XS Max profile

The merged `xsmax` / `A12_BOARD=d331p` profile has isolated inputs and output
directories. See [README.md](README.md#merged-xs-max-support) for its distinct
16.6.1 GPU/radio firmware and shell boot. XR/iPad firmware recipes above do
not apply to its BCM4377 PCIe radios.
