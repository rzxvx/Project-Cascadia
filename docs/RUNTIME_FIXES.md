# P105 runtime error fixes

This branch extracts corrections to the existing normal P105AP port from
[research draft #8](https://github.com/rzxvx/Project-Cascadia/pull/8).
It adds no charging implementation, writable NAND backend, external USB host
profile or VBUS supply. Those proposals remain in the separate draft.

## Corrections

- HDQ break timeouts propagate. High/low/high word sampling refuses incoherent
  rollover values, and refresh publishes a complete sample under the property
  lock. Invalid percentage values fail rather than entering the battery ABI.
- PMU GPIO callbacks return regmap errors instead of false success. Probe no
  longer dumps event registers before child IRQ consumers can handle them.
- S5L GPIO direction/mux changes clear both selector bits; explicit input
  selection also clears the retained peripheral software flag.
- H2FMI MEMREAD advances through ECC-failed pages and reports ECC diagnostics;
  transport/end-of-operation failure does not publish stale page bytes. The
  fixed geometry is described as MLC rather than SLC; it is not autodetected.
- NAND readers validate lengths/ranges, retain exact chunk sequence metadata,
  verify caches and refuse partial-error publication. MTD nodes bind to their
  sysfs identity. NBD startup/cleanup and wrapper readiness bind to the owned
  worker generation/pidfd rather than an unrelated PID or stale block size.
- Raw PPN readers refuse a bound H2FMI driver or ambiguous sysfs before opening
  controller memory. This exclusion is not proof of exclusive DMA ownership.
- S5L PHY resource/gate/regulator errors propagate. The separate generic PHY
  and USB consumer patch retains failed power-release state and coordinates
  balanced acquisition/release through suspend, resume and teardown.
- The RAM loader reserves its descending stack below the bundled zImage and
  refuses oversized code at link time. The CLI preserves bundle-build errors
  through the tail pipeline. Boot declarations fix diagnostics without
  changing the corresponding generated apple.c object in prior qualification.
- ADT parsing bounds nesting/node/property sizes and rejects duplicate keys
  and incomplete alignment padding. P105 PMGR entries use the 44-byte ABI;
  packed clocks use the physical base plus0x38. Encoded and older-format
  addresses remain explicitly unverified.

## Offline verification

Use an existing C compiler, ARM GNU tools and Python with Unicorn. Obtain the
pinned Linux v6.12 source (`adc218676eef25575469234709c2d87185ca223a`) using
`./cascadia kernel`, or point CASCADIA_LINUX_SOURCE at an existing local copy.
Source-based controls check hashes and use locally available Git objects; they
never download replacement inputs during tests. A full Cascadia checkout with
base history is required for historical file controls.

```
CASCADIA_LINUX_SOURCE=/path/to/local/linux python3 scripts/test-offline.py
```

Tests compile actual functions with explicit MMIO/regmap/USB/process/MTD
providers. They do not access the iPad, mount NAND, start real NBD workers,
change host services or transmit boot artifacts. Small pre-recorded metadata
fixtures are distinct from executing original Apple firmware. No firmware,
rootfs, host keys, pairing records or user device logs accompany this PR.

Hardware boot/charging/WLAN continuity and physical recovery from injected
errors have not been verified on the target iPad. Review the generic PHY/USB
interface changes separately from P105 provider changes before merging.

## Verified extracted revision

Code commit `2118e42c9993b6ad47d620bb9d10d277e303e48e` passes30 offline test entrypoints with0
failures/skips from an isolated, single-branch checkout.11 affected kernel
objects pass fresh ARM compilation with the saved normal kernel flags plus
Werror and0 warnings. The sources were reconstructed from pinned Linux6.12,
both extracted patches and the normal overlays. No production kernel tree
was edited. The3 NAND tools link as static ARM ELF files under their project
flags plusWerror, with0 reported compiler warnings. Loader linking, reserved
stack assertions, original-overwrite control and actual prologue emulation
are part of the30-entrypoint run. This is not a fresh complete-kernel link
or a device-boot test.

The first container test attempt used a non-executable temporary filesystem
and could not run C fixtures. The first isolated ARM attempt lacked a sibling
header lookup. Corrected qualification settings pass; candidate code was
unchanged. No failed attempt is counted as a PASS.

Existing source comments refer to the separate
[USB research notes](https://github.com/Marcolist/Project-Cascadia/blob/research/p105-power-storage-usb/docs/research/p105-usb-host.md);
those experiments are not included in this extraction.

## USB PMGR ON acknowledgement

The gate helper requested state `0xf`, but accepted any equal request/status
nibbles. A readback of `0x00` (including an unresponsive register that reads
zero) or `0x44` could therefore report success and allow OTG PHY reset writes
although the requested ON state was not observed. This is a reproducible
software defect; neither readback has been observed as the cause of a failure
on our device.

The helper now requires both nibbles to equal the requested ON value `0xf`.
The existing gate request, preserved upper bits, 1,000-poll bound and error
propagation remain unchanged. No rollback, power-source policy or new register
write is added. EHCI/OHCI use the same helper and retain their existing error
cleanup. This does not qualify external USB host mode or Lightning VBUS.

The actual-C regression covers all 256 request/status pairs, acknowledgement
at the last allowed poll, and acknowledgement beyond the deadline. A temporary
copy restoring only the old predicate reproduces `status=0x00`, return zero
and two PHY reset writes; the new predicate times out before reset instead.
The full focused entrypoint passes six tests: 282 positive-contract cases and
two historical negative controls, compiled with UBSan and strict warnings.
The changed driver also builds and links as an ARM module against the prepared
Linux 6.12 tree, with no warnings, in an isolated two-CPU/1-GiB container.
The module was not loaded and no new kernel image was booted.
