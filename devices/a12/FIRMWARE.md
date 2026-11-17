# A12 firmware and private inputs

Follow [QUICKSTART.md](QUICKSTART.md) for command order. This is a reference for
the **new device-separated workflow**, not the older shared `boot/inputs/` layout.
The new workflow still needs end-to-end build/boot validation on both boards.

## Apple boot sources

| Device/profile | Build | Apple IPSW |
|---|---|---|
| XR / `xr` | 18.7.10 / 22H374 | [iPhone11,8 restore](https://updates.cdn-apple.com/2026WinterFCS/fullrestores/140-97953/5F3051CD-5405-4FEF-889E-C3B78E0F376F/iPhone11,8_18.7.10_22H374_Restore.ipsw) |
| iPad 8 Wi-Fi / `ipad8` | 17.1.1 / 21B91 | [iPad 10.2 2020 restore](https://updates.cdn-apple.com/2023FallFCS/fullrestores/042-95894/E5C89FEE-E10A-4B33-8D69-973EDF7DB0A7/iPad_10.2_2020_17.1.1_21B91_Restore.ipsw) |

`profiles.json` pins these sources, board identities and hashes. `inputs` uses
`ipsw extract --remote` for BuildManifest and these small ZIP members:

| Profile | iBSS | Apple DeviceTree | Additional firmware |
|---|---|---|---|
| xr | `Firmware/dfu/iBSS.n841.RELEASE.im4p` | `Firmware/all_flash/DeviceTree.n841ap.im4p` | none |
| ipad8 | `Firmware/dfu/iBSS.ipad11b.RELEASE.im4p` | `Firmware/all_flash/DeviceTree.j171aap.im4p` | `Firmware/SmartIOFirmware_ASCv2.im4p` |

The script validates the BuildManifest paths before accepting decoded inputs.
For encrypted iBSS, the helper calls `ipsw img4 im4p extract --lookup` with the
selected product/build. The decrypted plaintext must still match its exact pin.
This requires an ipsw version supporting firmware-key lookup and either its cached
key or access to the public key service; no key values are stored in this repo.
No full IPSW download is needed for these files. iPad patches are SHA-pinned and
instruction-guarded; they are not XR offsets applied to another binary.

The Apple DeviceTree stays pristine. Linux uses the separate selected
`t8020-n841.dtb` or `t8020-j171a.dtb`. `--serial` uses `-v wdt=-1 serial=3`;
`--no-serial` uses `-v wdt=-1`. Neither uses `launchd_unsecure_cache=1`.

## Touch

`periphery` obtains the selected `Firmware/N841_Multitouch.im4p` or
`Firmware/J171a_Multitouch.im4p` from its matching IPSW. XR uses personality
`C1FC2,1` and the A12-owned `vendor/a13-tools/mtfw.py`. J171 uses `K1F19,9`,
`tools/makez2fw` and the selected iPad's private SysCfg. Generated touch and
calibration files go to `build/devices/<device>/touch/`; initramfs includes only
that board's files. Do not publish the private J171 container/calibration.

## IM4M and SysCfg are supplied locally

An IPSW does not contain your factory SysCfg or provide a generic substitute for
this device's ticket. Set `IM4M=/absolute/path/to/ticket.im4m`; set
`SYSCFG=/absolute/path/to/syscfg.bin` or use
`boot/inputs/<device>/syscfg.bin` (0600). These paths are ignored by Git.

The new calibrated packer requires SysCfg **before** packaging. The bootstrap
path for obtaining it on a never-booted device is not automated yet. On a Linux
session already running on the selected board:

```sh
export A12_DEVICE=xr       # OR ipad8
bash devices/a12/host/read-syscfg.sh
```

This reads 128 KiB NVMe namespace 3 read-only and checks board compatibility,
size/magic, model, WMac/BMac and WCAL without printing values. XR additionally
requires WSKU. J171's tested SysCfg lacks WSKU; the code does not invent one.
To check an existing file without copying it:

```sh
python3 devices/a12/profile.py --check-syscfg /absolute/path/to/syscfg.bin
```

## Radio firmware comes from the OS filesystem

These files are **not** the small top-level iBSS/DeviceTree members. A substantial
OS image must be obtained separately; do not substitute a SystemOS cryptex.

### XR — BCM4357B1 / yodak, UART Bluetooth

For 22H374 the OS member is `094-31934-038.dmg.aea`:

```sh
export A12_DEVICE=xr
IPSW=$(python3 -c 'import json; print(json.load(open("devices/a12/profiles.json"))["xr"]["ipsw"])')
ipsw extract --remote --pattern '^094-31934-038\.dmg\.aea$' \
  -o /absolute/path/to/cache "$IPSW"
./cascadia a12 periphery /absolute/path/to/094-31934-038.dmg.aea
```

The XR recipe decrypts AEA with `ipsw fw aea` (approximately 7.1 GiB temporary
output), selectively extracts the DriverKit Wi-Fi tree and `/usr/sbin/BlueTool`
using 7-Zip, then converts them with A12-owned helpers and pinned Sandcastle
hcdpack. It checks the BlueTool/HCD hashes. Outputs are
`boot/inputs/xr/brcm/`, including `BCM4349B1.apple,n841.hcd` and the yodak Wi-Fi
files. This existing tested recipe selects the XR's DRDO/USI template; do not
assume a different hardware module is interchangeable.

### iPad — BCM4355C1 / magnoliaa, UART Bluetooth

For 21B91 the OS member is `087-51745-002.dmg` (about 5.68 GB). Its UDIF DMG
contains LZFSE-compressed **chunks**, not one standalone LZFSE stream. Selectively
extract the Wi-Fi tree and `/usr/sbin/BlueTool` through a read-only APFS-FUSE mount
with bounded memory. The laboratory extraction used a 256 MiB address-space cap
and a comma-free mount-source path. Whole-file `lzfse` exhausted the host's RAM;
do not repeat it. Mounting/decryption is not automated by the new converter.
See the evidence/constraints in [boot/IPAD8_RADIO.md](boot/IPAD8_RADIO.md).

```sh
export A12_DEVICE=ipad8
python3 devices/a12/boot/prepare-ipad-radio.py \
  --wifi-dir /absolute/path/to/extracted/wifi \
  --bluetool /absolute/path/to/extracted/BlueTool
```

Outputs are `boot/inputs/ipad8/radios/brcm/` and
`boot/inputs/ipad8/radios/bluetooth-candidates/`.

### What “vendor” means

It is the **radio module manufacturer**, not Apple, the network SSID or the A12
chip. `m` = **Murata**, `u` = **USI**. Matching Wi-Fi NVRAM templates and Bluetooth
HCD candidates differ. The tested A2270's OTP reported module YSBC, vendor `m`;
that observation must not be assumed for every iPad.

On the selected iPad's Linux USB-SSH session:

```sh
bash devices/a12/boot/load-ipad8-radio.sh probe
```

This requests the Broadcom OTP summary without guessing/loading an HCD. If there
is no trustworthy OTP vendor diagnostic, investigate rather than guess. After
confirming `m` or `u`, load externally:

```sh
export IPAD8_RADIO_VENDOR=m  # example ONLY for an OTP-confirmed Murata module
bash devices/a12/boot/load-ipad8-radio.sh wifi "$IPAD8_RADIO_VENDOR"
bash devices/a12/boot/load-ipad8-radio.sh bluetooth "$IPAD8_RADIO_VENDOR"
```

For XR's prepared firmware, the equivalent manual loading is:

```sh
export A12_DEVICE=xr
bash devices/a12/boot/load-wifi-firmware.sh
bash devices/a12/boot/load-bluetooth-firmware.sh
```

Both boards can instead use `./cascadia a12 boot radios` after preparation. It
checks the selected files before boot, waits for the matching Debian board, and
runs only that board's loaders. The iPad requires its verified vendor variable.
No Wi-Fi/Bluetooth blobs are embedded in the boot payload.

## XS Max profile

The merged `xsmax` / `A12_BOARD=d331p` profile has isolated inputs and output
directories. See [README.md](README.md#merged-xs-max-support) for its distinct
16.6.1 GPU/radio firmware and shell boot. XR/iPad firmware recipes above do
not apply to its BCM4377 PCIe radios.
