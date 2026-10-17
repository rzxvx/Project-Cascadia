# Linux-owned SLC rootfs: actual offline images and remaining backend work

2026-10-12. No device access, NAND write, format, UBI attach or filesystem mount.
This is a buildable filesystem preparation path, not an installation procedure.

## Concrete filesystem path

The intended independent Linux storage stack is an **exclusive, validated PPN
SLC block domain → writable MTD → UBI → UBIFS**. It avoids reimplementing Apple's
SwissPPNFTL and gives UBI responsibility for wear leveling and volume mappings.
The [pinned Linux 6.12 UBIFS documentation](https://www.kernel.org/doc/html/v6.12/filesystems/ubifs.html)
describes this MTD/UBI interface. Ordinary MLC is rejected by Linux 6.12 UBI;
the existing read-only 256-page MLC export cannot be relabelled as SLC.

`scripts/prepare-ubifs-image.py` now produces real UBIFS and UBI **regular files**
from a rootfs directory using reviewed mtd-utils 2.3.0. It never runs a format,
attach, mount, device ioctl or deployment command. It refuses an existing output
directory, device/special-file inputs and output nested inside the source tree.
Source contents/modes/timestamps are hashed before and after the build; symlinks
are recorded as links rather than followed. Image creation does not set source
xattrs or alter source ownership. Output ownership is squashed inside the image.

Geometry must be explicit, bounded and labelled `offline-image-only`. There is
no capacity detection, implicit target geometry or automatic installation.
For the synthetic 16KiB / 128-page SLC profile used in local validation:

| Field | Offline assumption |
|---|---:|
| Minimum I/O and header subpage unit | 16,384 bytes |
| Physical eraseblock | 2,097,152 bytes |
| VID header offset | 16,384 bytes |
| Data offset | 32,768 bytes |
| Logical eraseblock | 2,064,384 bytes |
| Example rootfs volume reservation | 64 LEBs |

The whole-page alignment follows the reconstructed original PPN program
envelope, not a claim that 4KiB BCH boot pages allow 4KiB MTD subpage writes.
Neither 128 SLC pages nor this reservation is a measurement of our unit.

## Offline build

Acquire and verify the maintainer's
[mtd-utils 2.3.0 release and signature](https://lists.infradead.org/pipermail/linux-mtd/2025-February/108248.html).
The [maintainer mirror](https://git.infraroot.at/mtd-utils.git/about/) documents
dependencies and build options. The local release signature verified with
fingerprint `13063F723C9E584AEACD5B9BBCE5DC3C741A02D1`; its public key came from
the HTTPS release announcement. No independent web-of-trust identity claim is
made. Build only `mkfs.ubifs` and `ubinize` locally; no system installation is
needed. This builder deliberately accepts version 2.3.0, not arbitrary tools.

Put these explicit assumptions into a local JSON file:

```json
{
  "purpose": "offline-image-only",
  "min_io": 16384,
  "peb_size": 2097152,
  "volume_lebs": 64
}
```

Then, with reviewed tool binaries at the selected paths:

```sh
python3 scripts/prepare-ubifs-image.py \
  --rootfs build/initramfs-root --geometry /path/to/synthetic-geometry.json \
  --output-dir /path/to/new-private-image-directory \
  --mkfs /path/to/mkfs.ubifs --ubinize /path/to/ubinize
```

The builder checks the UBIFS superblock identity/CRC/geometry/length; every UBI
EC and VID header and their CRCs; both volume tables and all record CRCs; the
single rootfs reservation and all payload bytes against the UBIFS file. The
manifest contains hashes of both outputs, the builder and tool binaries.
These are format/container checks, not a kernel mount or flash-crash test.
UBIFS UUID generation remains random, so identical inputs are not claimed to
produce identical binary hashes.

## Actual local result

The prepared Alpine rootfs produced a 45,416,448-byte UBIFS image (22 LEBs) and
a 50,331,648-byte UBI image (24 PEBs, including two layout copies). Independent
inspection accepted the output and rejected deliberate EC, VID, volume-table,
payload and superblock corruption plus a truncated UBI file. The original
artifacts remained unchanged. Both tools were built with existing host compiler
resources at low priority, with all dependency extraction/output inside the
project. No packages or services were installed system-wide.

The optional-feature build reported an unused `add_xattr` function, a libfec
possibly-uninitialized warning, and GNU ar's ignored `u` modifier. These are
recorded rather than hidden; the image/header checks passed. Images embed the
user's existing rootfs, including private SSH configuration and Apple firmware,
and must remain local. Only source and documentation belong in the PR package.

## What still prevents writable hardware storage

The separate staged PPN engine reconstructs single-page program and single-block
erase, bounds addresses and DMA ownership, propagates timeouts, and does not
copy the original firmware's conditional DMA-failure success. It has **no
hardware binding or MTD write callback**. Its transport-complete return explicitly
retains `durability_unknown`; FIFO/DMA completion is not promoted to fsync success.

Before binding that engine, establish this unit's actual geometry, whole-block
SLC/MLC exclusivity, protected boot regions, sequencer/CDMA setup, retirement and
bad-block policy, and completion/power-cut guarantees. SLC and MLC addresses may
alias a physical block; independent allocations through both views are unsafe.
Unknown error-list acknowledgement is not sent and failed operations are not
blindly replayed. UBI cannot compensate for a lower layer that loses acknowledged
pages after a later interrupted program.

For this user's current Pico-free boot path, giving up iOS also removes the
jailbreak/kDFU entry. A separately verified BootROM entry/recovery path is required
before any destructive Linux-owned layout. The current device remains untouched.
