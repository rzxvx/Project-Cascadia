# A12 quickstart — select XR or iPad 8

> **Validation:** source separation is implemented and host-tested, but a clean end-to-end
> build/package/boot through this exact workflow still needs testing on both
> boards. The older successful laboratory boots are not that validation.

Run commands from the repository root. No step here requires a full IPSW download
for iBSS/DeviceTree. The separate OS filesystem needed for radios is much larger.

## 1. Select the hardware

Choose **one**:

```sh
export A12_DEVICE=xr       # iPhone11,8 / N841AP
# OR:
export A12_DEVICE=ipad8    # iPad11,6 / J171aAP / A2270 Wi-Fi only

./cascadia a12 doctor
```

An explicit `./cascadia a12 --device ipad8 doctor` also works. `A12_PROFILE` is a
compatibility alias for older Debian commands; conflicting selections are an
error. Selection alone does not build anything or create another Debian root.

Required tools include Python 3/PyUSB/libusb, git, bash, curl, SSH, `ipsw`, `img4`,
`irecovery`, dtc (`dtc`, `fdtget`, `fdtput`), cpp, C/LLVM build tools, make,
cpio/gzip, file/readelf and dpkg-deb. Debian creation additionally requires root,
debootstrap and working ARM64 chroot execution. The host kernel needs USB Ethernet
and NFSv3 server support. Tool availability is not proof that all inputs are ready.

## 2. Supply private prerequisites

You need the selected device's own IM4M and SysCfg. Do not use the other board's
calibration. Keep absolute paths for external inputs:

```sh
export IM4M=/absolute/path/to/this-device-ticket.im4m
export SYSCFG=/absolute/path/to/this-device-syscfg.bin
python3 devices/a12/profile.py --check-syscfg "$SYSCFG"
```

Alternatively, put SysCfg at `devices/a12/boot/inputs/$A12_DEVICE/syscfg.bin`
(mode 0600) and omit `SYSCFG`. No shared `boot/inputs/syscfg.bin` fallback is used.
Tickets and SysCfg are **not** downloaded from Apple. SysCfg contains factory
calibration and private identifiers, not generic firmware.

If you already have a working Linux USB-SSH/recovery session on this board:

```sh
bash devices/a12/host/read-syscfg.sh
```

That reads NVMe namespace 3 read-only, checks the selected board and validates
SysCfg before saving it privately. **Obtaining SysCfg on a completely fresh
unbooted device is not automated by this refactor.** The current calibrated
packer requires it in advance; do not follow an old instruction to pack without
SysCfg and assume that is supported here. See [FIRMWARE.md](FIRMWARE.md).

## 3. Download the small Apple boot inputs and prepare touch

```sh
./cascadia a12 inputs --serial
./cascadia a12 periphery
```

`inputs` range-downloads BuildManifest, the matching iBSS/Apple DeviceTree and
**J171-only SmartIO**, then checks board/build identities and pinned hashes.
`periphery` fetches/converts only the selected touch personality. See
[FIRMWARE.md](FIRMWARE.md) for the exact Apple IPSW links.

For no DCSD serial use `inputs --no-serial`, and make the same choice at `pack`.
Linux iBSS arguments are `-v wdt=-1 [serial=3]`, not the iOS `normal` preset.
For encrypted iBSS (including J171/21B91), the helper uses device/build-specific
`ipsw` firmware-key lookup before checking the plaintext pin. Wrong keys or builds
remain a hard failure; no nearby-offset fallback is used.

## 4. Prepare radios outside the boot image

### XR

After obtaining the matching 22H374 OS image:

```sh
./cascadia a12 periphery /absolute/path/to/094-31934-038.dmg.aea
# An already-decrypted .dmg also works.
```

This repeats touch preparation and runs the existing XR radio extraction recipe.
Allow its disk requirements; do not use an unrelated cryptex image.

### iPad 8

Selectively extract the 21B91 Wi-Fi tree and BlueTool using the documented
read-only, bounded-memory OS mount, then:

```sh
python3 devices/a12/boot/prepare-ipad-radio.py \
  --wifi-dir /absolute/path/to/extracted/wifi \
  --bluetool /absolute/path/to/extracted/BlueTool
```

The script prepares both module candidates; it does not guess which belongs to
your board. **Vendor means the Wi-Fi/Bluetooth module maker: `m` = Murata,
`u` = USI.** They have different NVRAM/HCD variants. Our tested A2270 reported
Murata in hardware OTP; that does not establish a default for all iPads.

Once this board has Linux USB SSH, identify it without choosing a firmware:

```sh
bash devices/a12/boot/load-ipad8-radio.sh probe
```

Read its OTP diagnostic. Only after confirming the vendor, set one value:

```sh
export IPAD8_RADIO_VENDOR=m   # only when OTP confirms Murata; u means USI
```

You can do an initial `boot` without radios to inspect OTP, then load externally.
If there is no trustworthy vendor result, stop instead of guessing. See
[FIRMWARE.md](FIRMWARE.md) for loading commands and extraction limitations.

## 5. Prepare or reuse ONE Debian root

**If a root already exists, do not rebuild it.** A rootfs build creates only the
selected profile but replaces that profile's staging root and is unnecessary
for switching devices during sequential testing.

For a genuinely new installation only:

```sh
sudo -E bash devices/a12/rootfs/build-debian-rootfs.sh
```

The builder prompts for the selected account's password; no common password is
shipped. Default accounts are `xr` / iPhone XR User and `ipad` / iPad 8 User.
Password login and a touch keyboard are configured. Fresh users are not granted
sudo automatically. See [rootfs/PROFILES.md](rootfs/PROFILES.md).

To export the selected profile's default root:

```sh
sudo -E bash devices/a12/host/linux-nfs-export.sh on
```

To **reuse an existing root**, explicitly select its existing directory instead:

```sh
sudo -E env DST=/srv/cascadia-ipad8-debian \
  bash devices/a12/host/linux-nfs-export.sh on
```

A populated root is reused, not copied/reinstalled, unless `FORCE_SYNC=1` is
explicitly set. The account/hostname of a reused root remains its existing one.
Do not export the same directory through two profile export files: disable the
previous profile's export first, without deleting its root. Never run two devices
concurrently on the same writable root. See [DEBIAN_XFCE_NFS.md](DEBIAN_XFCE_NFS.md).

The exporter writes `build/devices/$A12_DEVICE/nfsroot`. This must be done
**before** building the initramfs; an old packed image retains its old NFS path.

## 6. Build and package

Shared sources/builds are reused; only small device-specific outputs are separate:

```sh
./cascadia a12 build m1n1     # when the shared loader needs building
./cascadia a12 build kernel  # prepares a baseline if absent; otherwise incremental
./cascadia a12 build dtb
./cascadia a12 build initramfs
./cascadia a12 pack --serial
```

For no DCSD console use `pack --no-serial`. `build kernel-clean` explicitly resets
the script-owned kernel baseline; it is not required for every build. Kernel and
linker jobs default to one on low-memory hosts; raise `JOBS` / `LINK_JOBS` only when
resources allow. m1n1's builder still recreates its own ignored build tree.

The new packer requires the selected SysCfg, Linux DTB, stamped initramfs and NFS
setting. It refuses other-board touch firmware or stale NFS metadata. It does not
fall back to the earlier shared laboratory input/output directories.

```sh
python3 devices/a12/boot/boot-device.py --verify-only
```

This checks artifacts/manifests with **no USB interaction**. It is not a hardware
boot test. Do not publish firmware, private calibration or generated images.

## 7. Boot the selected device

Keep the NFS server running. Put only the selected device in **pwned DFU**, then:

```sh
./cascadia a12 boot
# OR, after preparing/identifying radios:
./cascadia a12 boot radios
```

Both commands support XR and iPad. The old `boot-and-load-xr.sh` name is XR-only.
The generic tool checks PRODUCT/CPID/BDID, pwned state, exactly one Apple DFU device,
and artifact hashes before sending components. iPad includes SmartIO; XR does not.
Serial capture is started for serial-enabled images. See [boot/README.md](boot/README.md).

When USB ECM appears, make sure its **host** interface has `172.16.42.2/24`:

```sh
ip -br addr
# Identify the actual new ECM interface (host MAC 02:00:00:00:02:01).
IFACE=YOUR_ECM_INTERFACE
sudo ip addr replace 172.16.42.2/24 dev "$IFACE"
sudo ip link set "$IFACE" up
```

Do this promptly so initramfs can reach NFS; a host DHCP client may already assign
it. The boot script does not guess/reconfigure arbitrary network interfaces.

After Debian SSH is available, `boot radios` loads the selected external firmware.
Manual commands are listed in [FIRMWARE.md](FIRMWARE.md). USB SSH is
`ssh root@172.16.42.1`, key-authenticated using the public keys packed from the
build host. Desktop password login is separate from root SSH authentication.

## 8. Verify, then test reconnect

Check display/touch, password entry, desktop apps, Wi-Fi connection and Bluetooth.
Then, while connected:

```sh
ssh root@172.16.42.1 'test -e /run/cascadia/a13-usbwatch && echo A13_HANDOFF; ps -eo pid,ppid,args | grep -E "[u]sbwatch|[u]dhcpd"; cat /sys/class/udc/*/state'
```

The default A13-style handoff should show one watchdog and one DHCP child;
the Debian replacement service is intentionally inactive, and this quiet
upstream worker has no watchdog log. No build-mode option is needed.
Unplug for 15 seconds, reconnect, then check SSH, NFS and an application. Debian is still **USB-NFS-root**:
apps can stall while disconnected even if Wi-Fi works. This is reconnect recovery,
not a cable-free root. A RAM-resident or other independent root is separate work.

If anything fails, preserve the manifest and serial logs and stop at that stage;
do not substitute the other board's DT, ticket, calibration or firmware.
