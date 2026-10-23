# Raw PPN / bound kernel H2FMI ownership rejection

Frozen private candidate. No device/MMIO/boot/NAND operation, shared source edit,
commit or new service. Added C uses sysfs metadata only; offline tests substitute
every open/map/read/lock/directory operation. Existing NAND opcode allowlist and
power-state setter are unchanged.

Baseline is actual local Cascadia revision
`3cf90cafbc295e87d1ff7a1436efc8667201be51`; its exact ppn.c/h are preserved
under baseline with pinned SHA256 in baseline-hashes.json. These are project GPL
source, not Apple firmware. Historical controls use original files, never an
inverted candidate. Candidate is candidate/ppn.c and candidate/ppn.h.

## Reproduced ownership conflict

ppn_open maps /dev/mem O_RDWR; ppn_claim checks clock gates then takes a userspace
flock. Kernel H2FMI uses a separate per-bus mutex. That flock cannot serialize
with kernel read/read_oob/block_isbad transactions. Historical actual functions
accept raw access even when a fake apple-h2fmi bound-device link is present. The
opcode allowlist protects against NAND program/erase; it does not make two
concurrent read command sequences safe.

Pinned Linux6.12 drivers/base/dd.c::driver_sysfs_add creates a device link under
the driver's own sysfs directory BEFORE probe, as well as the reverse
device/driver link. A device link inside genuine
/sys/bus/platform/drivers/apple-h2fmi therefore gives a concrete bound/in-probe
owner to reject. This is specific to that directory, not arbitrary /proc text
or a guessed MTD index.

## Candidate behavior

Before writable raw open/mapping and before clock-gate reads/flock, inspect the
real sysfs parent and driver directory. Both must identify SYSFS_MAGIC. A missing
parent, unreadable directory, non-sysfs replacement, symlink-directory traversal,
failed directory/stat/close operation or unknown/malformed entry refuses access.
A missing apple-h2fmi child under an accessible genuine sysfs parent means only
that this rejection test found no bound driver. Module and the standard
bind/unbind/uevent metadata entries must have their expected symlink/regular-file
types; device symlink refuses with EBUSY, unknown entry with EPROTO.

Recheck after mapping and after flock, before ppn_open returns or ppn_claim
succeeds. Never unbind/reset the driver or bypass refusal with an environment
variable. Close only newly owned sysfs FDs once; close failures aren't retried.
All initial raw paths inherit ppn_open guard, including nandctl regs.

**Refusal guard, not positive ownership lease.** Binding can occur AFTER the
final snapshot; unbinding can overlap teardown/in-flight contexts. No IOP/old
CDMA/IRQ no-future-access receipt is created. No transfer identity, PMGR shutdown
status, device reservation or kernel mutex is inferred from an empty directory.
Use existing explicit MTD backend for kernel PIO reads; no automatic raw fallback.
Full kernel-only diagnostics/future DMA binding still need qualified ownership.

## Actual function regressions

Native and limited Ubuntu22 image, ASan+UBSan/-Werror: PASS.

* 2 historical caller controls demonstrate raw open/maps and claim acceptance
  despite bound kernel-device metadata.
* 25 candidate cases exercise actual helper AND ppn_open/claim: bound device,
  valid controls/module/no binding, absent child, missing/inaccessible parent or
  child, wrong root/child filesystem, ELOOP, fstatfs/fdopendir/readdir/stat/dirfd/
  closedir/close faults, malformed module/control/unknown entry, simulated binding
  during initial map/flock, and no-binding controls.
* Initial refusal preserves zero raw opens/maps/gate reads/flocks. Late-binding
  controls permit the earlier map/gate phase but reject before caller transaction.
  Synthetic scheduling does not prove atomic real bind exclusion.
* New metadata FDs close exactly once on errors. No actual FD/mmap/flock,
  filesystem/module/hardware operation is invoked by Fakes.

## Firstboot diagnosis boundary

Safe metadata collection can read Linux MTD sysfs name/type/flags/dev/size/
erasesize/writesize/oobsize and /proc/mtd; actual mtdcore show callbacks format
mtd_info memory and issue no NAND commands. Current H2FMI reports mlc-nand,
MTD_NO_ERASE 0x1000, no MTD_WRITEABLE 0x400, writesize16384/OOB64,
erasesize4194304 and size4462739456 per die. These are hardcoded driver geometry,
NOT measured identification/capacity of our unit. No own-unit NAND evidence yet.

nandctl ppninfo is not metadata-only: ppn_open maps /dev/mem, ppn_claim checks
PMGR/flock (itself no controller writes), ppn_get_feature executes CE selection,
IRQ masks/ACKs, EE/E7/7A/77 sequences and PIO writes. Keep opt-in after explicit
device-phase authorization/exclusive raw assessment; never alongside bound MTD.
Neither full iosnand scan nor page/dump/userdata tracing belongs in firstboot
metadata collection.

## Reproduce

python3 run.py tests frozen candidate. --source /absolute/path/ppn.c exercises
proposed production file (sibling ppn.h used). Exact original fixture hashes
always checked. Limited image: readonly /code=this group; independent private
writable /out; no device mounts; existing wrapper CASCADIA_BUILD_OFFLINE=1,
--read-only, user1000:1000 and small /tmp tmpfs; run python3 run.py --output-dir /out.
Root handles integration/conventional tests/real ARM build. No shared edits.
