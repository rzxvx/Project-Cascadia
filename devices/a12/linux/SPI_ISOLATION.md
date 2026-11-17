# XR legacy SPI / J171 SmartIO isolation

## Regression found by the first clean XR build

Remote `dr1408/Project-Cascadia:a12-xr`, commit
`0feaffa54fd8999c5ba58e0b6c0d6112a44808bd`, deliberately keeps N841's legacy
SPI controller out of runtime PM. Its patch documents the exact failure:

```
PS spi1: Failed to reach power state 0xf (now: 0xa4f)
Failed to power device: -110
no HBPP bootloader
```

The first newly separated source-built XR package reproduced those errors.
N841's board DTS and PMGR map matched the remote branch. The build script had
applied the legacy patch and then overwritten `drivers/spi/spi-apple.c` with the
J171 SIO/DMA source. That replacement unconditionally enabled controller runtime
PM. Thus the XR safeguard existed in the patch but not in the compiled driver.
Earlier successful boots of old packaged binaries did not detect this regression.

## Source correction

- `linux-0010-spi-apple-legacy.patch` is restored byte-for-byte from the remote
  commit. SHA-256: `a7fa14024da85c5e87eab48ba1bb490a9feead277c8f1b4e671ed2cd1239bc98`.
- XR keeps the patched `drivers/spi/spi-apple.c`, matching `apple,t8020-spi`.
  Its legacy probe returns without enabling runtime PM, as in the working branch.
- The SIO implementation is staged separately as `spi-apple-j171.c` with platform
  driver name `apple-spi-j171`. It matches **only** `apple,j171a-spi`: no generic
  `apple,spi`, `apple,t8020-spi` or M1 fallback can bind an XR controller.
- Only the J171 board's SPI3 compatible is changed to select that driver. DMA,
  runtime PM and the existing J171 transfer implementation remain on that path.
  No new SPI register sequencing or touch firmware format was introduced here.
- An incremental migration restores the old overwritten SPI source only if its
  SHA matches the exact known previous replacement. Unknown edits are not reset.
  The whole kernel tree/object cache is never cleaned for this correction.

The J171 diagnostic parameter, if deliberately used, now belongs to the
`spi_apple_j171` module rather than the generic `spi_apple` module. Normal boot
arguments do not enable it.

## Verification and limits

`tests/test-spi-device-gating.py` checks the exact remote patch, actual patched
legacy probe, disjoint match tables/names, staging and both compiled DTs. Existing
J171 setup/snapshot tests still exercise the transport helpers. These are host
checks, not proof of working touch.

An incremental kernel relink and XR repack are required; XR's m1n1, DT,
initramfs, factory calibration and Debian root need no changes. Record the actual
hardware result only after rebooting the corrected package. J171 must also be
retested after rebuilding its selected DT with the new compatible.
