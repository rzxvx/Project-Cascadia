# P105 read-only MTD media type

Assessment: 2026-10-11. Cascadia source reviewed at local revision
`55ff2b5cf7ed266ee91a8a824b5a997784bf850c`, Linux v6.12
`adc218676eef25575469234709c2d87185ca223a`.
This correction was tested only with synthetic host fixtures. No device
was opened, booted, mounted or changed.

The fixed H2FMI mapping exposes 256-page MLC blocks with 16 KiB data and
64-byte OOB per page. Its row encoding does not select an SLC mode. The
[upstream hardware notes](p105-nand.md) distinguish 256 MLC from 128 SLC
pages; this is evidence about upstream's supported geometry, not a new
measurement of our iPad or a guarantee about its capacity.

[Linux v6.12 MTD UAPI](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/include/uapi/mtd/mtd-abi.h#L145)
defines `MTD_NANDFLASH` as SLC and `MTD_MLCNANDFLASH` as MLC including TLC.
The former therefore mislabelled Cascadia's exported mapping. Internal PPN
ECC does not change its cell type or prove an SLC programming contract.

[MTD core type reporting and registration](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/mtd/mtdcore.c)
reports this type as `mlc-nand`. SLC emulation is a separately guarded
partition mode requiring a pairing scheme; changing the type does not
enable that mode or change the read address mapping.

The driver now reports `MTD_MLCNANDFLASH`. Its flags remain exactly
`MTD_NO_ERASE`: no `MTD_WRITEABLE` or `MTD_SLC_ON_MLC_EMULATION` is added.
Callbacks, geometry, row addressing and read-only command allowlist are
unchanged. No program, erase or FTL write path was introduced.

[Linux v6.12 UBI initialization](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/mtd/ubi/build.c#L898)
rejects MLC without the explicit SLC-emulation flag. Accurate type metadata
preserves that check. This is not permission to attach UBI or evidence that
a writable PPN/SLC mode exists; the current no-write/no-erase boundary is
independent of the type correction.

`iosnand`'s optional MTD backend accepts only the corrected MLC type, its
fixed 64-bit geometry and a non-writable, non-SLC-emulated export. Existing
sysfs name/device-number binding remains required. Older kernel builds
that report SLC fail with `EINVAL`, even if their geometry otherwise matches.
Build the matching kernel and tool together. There is no type override,
automatic remapping or fallback to `/dev/mem` after rejection.

The production backend fixtures now include acceptance of the corrected
MLC export and rejection of legacy SLC, NOR, writable and SLC-emulated
exports. The strict discovery fixture also checks descriptor cleanup and
`EINVAL` for rejected types. The suites cover modern and old MEMREAD header
environments; no real device node or ioctl is used. The existing actual
H2FMI/MEMREAD read-path cases still pass.
