# XR / iPad 8 device separation (work in progress)

> **Developer reference:** follow [QUICKSTART.md](QUICKSTART.md) for the single current user walkthrough. This document explains layout and refactor constraints.


## Selection and scope

```sh
./cascadia a12 --device xr doctor
./cascadia a12 --device ipad8 doctor
# Equivalent for every helper invoked below:
export A12_DEVICE=ipad8
```

`xr` is N841 / iPhone11,8 / T8020 / BDID 0x0c. `ipad8` currently means **only
J171a / iPad11,6 / A2270 Wi-Fi / T8020 / BDID 0x24**, not other iPad models.
XR remains the default. `A12_PROFILE` remains an alias for existing Debian
commands; conflicting selections are rejected. `profiles.json` is the hardware
and firmware metadata source. Merely selecting a device does not create roots,
build kernels, move files, write to the device or change host NFS exports.

## What is separate, what is shared

| Content | Location |
|---|---|
| Private SysCfg, ticket, extracted radio files | `boot/inputs/xr/` or `boot/inputs/ipad8/` |
| Raw Apple inputs | `boot/extracted/<device>/` |
| Touch firmware/private calibration | `build/devices/<device>/touch/` |
| Linux DTB and initramfs | `build/devices/<device>/payload/` |
| Patched iBSS, ticketed components, Linux IMG4, manifests/logs | `build/devices/<device>/boot/` |
| NFS address packed into initramfs | `build/devices/<device>/nfsroot` |
| Kernel source/config/objects and m1n1 source | existing shared `build/linux`, `build/m1n1` |
| Shared compiled kernel/m1n1 | existing `build/payload/Image.gz`, `m1n1-idevice.macho` |

Small board-specific outputs are separate. This does not create a second kernel
checkout or Debian root. Building/exporting a fresh root is always an explicit
command; do not run the rootfs builder to switch an existing live root's profile.

The old shared inputs and laboratory payloads are intentionally left untouched.
The new workflow does **not** silently use them. Before copying a private SysCfg:

```sh
A12_DEVICE=ipad8 python3 devices/a12/profile.py --check-syscfg /path/to/syscfg.bin
# Then explicitly copy into boot/inputs/ipad8/syscfg.bin, mode 0600.
```

Never use the iPad SysCfg for XR. The validator checks model, size, MAC validity
and required calibration keys without printing private values. XR requires WSKU;
J171's verified SysCfg does not have it, so it must not be invented.

## Reproducible preparation

1. Select the device. Supply its own IM4M with `IM4M=/path/to/ticket.im4m`.
2. `./cascadia a12 inputs [--serial|--no-serial]` range-extracts the pinned
   Apple build's iBSS, pristine ADT and (J171 only) SmartIO, checks the
   BuildManifest board/product/component paths and raw hashes (using device/build-
   specific ipsw key lookup for encrypted iBSS), and prepares the
   first-stage wrappers. No full IPSW download is needed. The pinned firmware
   sources/versions are in `profiles.json`: XR 18.7.10/22H374, iPad 17.1.1/21B91.
3. `./cascadia a12 periphery` prepares the selected touch firmware. J171 uses
   the A12-owned makez2fw and validated per-device SysCfg. It no longer runs
   merely because an old J171 file happens to exist in a shared directory.
4. Prepare the selected radios **outside the initramfs**:
   - XR: `periphery /path/to/22H374-OS.dmg[.aea]` retains the existing recipe.
   - J171: after selectively extracting the Wi-Fi tree and BlueTool through a
     bounded-memory, read-only APFS-FUSE mount, run:
     `python3 devices/a12/boot/prepare-ipad-radio.py --wifi-dir DIR --bluetool FILE`.
     This script converts only the provided small files and checks both known
     Bluetooth candidate hashes. It does not mount/decompress an OS image or
     select the module vendor. See `boot/IPAD8_RADIO.md` for the extraction
     constraints; standalone lzfse on the entire DMG previously exhausted RAM.
5. Prepare/export the chosen Debian root explicitly, or reuse the existing one:
   `DST=/srv/cascadia-ipad8-debian ./cascadia a12 nfs on`. This writes the selected
   device's NFS address file. Reuse is sequential: do not boot both devices on
   the same writable root concurrently. Reusing a root does not rename its user.
6. Build shared m1n1/kernel when required, then the selected DTB/initramfs:
   `./cascadia a12 build dtb`, `./cascadia a12 build initramfs`.
   `build kernel` no longer cleans an existing checkout on every invocation;
   `build kernel-clean` explicitly restores the pinned baseline first.
   Default kernel/link jobs are one; override `JOBS` / `LINK_JOBS` deliberately.
7. `./cascadia a12 pack [--serial|--no-serial]`. The first-stage patcher uses
   **Linux-only** `-v wdt=-1 [serial=3]`, not the iOS `normal` preset containing
   `launchd_unsecure_cache=1`. The packer refuses a mismatched Linux DTB, wrong
   SysCfg, stale NFS setting, unstamped initramfs or other-board touch firmware.
8. `python3 devices/a12/boot/boot-device.py --verify-only` checks selected artifact
   identities/hashes without USB access. In pwned DFU, `./cascadia a12 boot`
   checks exactly one Apple DFU device plus PRODUCT/CPID/BDID before sending it.
   J171 order includes SmartIO before the restore trust cache. `boot radios`
   waits for the selected board's Debian SSH, then runs only its radio loaders.
   For J171, `IPAD8_RADIO_VENDOR=m|u` must be set from verified OTP evidence;
   missing radio files/vendor are rejected before boot. Configure the host USB
   address/DHCP and NFS server as documented; the boot tool does not guess and
   reconfigure arbitrary host interfaces.

The legacy `boot-xr-linux.sh` / `boot-and-load-xr.sh` names now delegate to the
identity-checked XR workflow. Legacy `XR_LINUX_IMG4` overrides are rejected;
use the selected boot directory and matching manifests rather than mixing files.

## Frozen A13 dependencies

A12 previously consumed ten A13 m1n1 patches, usbliter8ctl, mtfw.py, brcmfw.py
and syscfg.py directly. Exact local copies now live in `m1n1/base/` and
`vendor/a13-tools/`; `vendor/DEPENDENCIES.json` records provenance and hashes.
The m1n1 base/overlay ordering is unchanged. Existing local mtfw.py changes are
preserved explicitly; no Apple firmware or private data was copied into source.
There are no executable references to changing `devices/a13` helpers/patches.
Documentation links mentioning A13 remain provenance, not build dependencies.

## Validation status — do not overstate

Host tests cover profile/alias selection, distinct private/output paths,
wrong-device SysCfg rejection, frozen dependency hashes, USB board checks and
manifest mismatch/tamper rejection. Existing five J171 radio guard/helper tests
also pass. No kernel, m1n1 or hardware drivers were rebuilt/changed by this
refactor. The known-good boot artifacts remain as they were.

**The new separated workflow still needs a clean end-to-end build/package test
and real boots on both devices.** Earlier boots of laboratory images are not
proof that the refactored workflow reproduces them. J171 OS-image extraction
still uses the documented bounded-memory manual mount; only conversion/loading
has a dedicated script. Do not call the PR reproducible until those gaps pass.


## SPI regression gate

The first fresh XR boot exposed a previously hidden shared-driver runtime-PM
regression. See [linux/SPI_ISOLATION.md](linux/SPI_ISOLATION.md) for the remote
branch comparison and transport separation. This is why clean source builds and
both-board hardware tests are required in addition to artifact/manifest checks.
