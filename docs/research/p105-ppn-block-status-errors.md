# PPN block-status errors

This independent change is based on upstream commit
`7373331a9f1c88d347773736a19dc3166947864d`. It needs no PR9 read-path changes.

`h2fmi_mtd_block_isbad()` previously returned `status == PPN_ST_RETIRED`
after a successful page transfer. Consequently, every unknown byte, including
the reported `0x50`, became a successful good-block result. A successful
transport alone does not establish a recognized PPN block state.

The callback now returns:

| Existing named status | Result |
| --- | --- |
| GOOD `0x40`, REFRESH `0x42`, ERASED `0x49` | `0`, block not marked retired |
| RETIRED `0x45` | `1`, bad block |
| Any other byte | `-EIO`, block state could not be established |
| Nonzero page-reader return | Original error, unchanged |

The Linux v6.12 MTD implementation provides this error channel. Its
[mtd_block_isbad() wrapper](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/mtd/mtdcore.c?h=v6.12#n2317)
returns the driver's result. The
[MEMGETBADBLOCK ioctl](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/mtd/mtdchar.c?h=v6.12#n1076)
passes it through. The
[UBI caller](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/mtd/ubi/io.c?h=v6.12#n580)
explicitly distinguishes negative errors from positive bad-block indications.
The
[raw NAND marker reader](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/mtd/nand/raw/nand_base.c?h=v6.12#n252)
also propagates negative read errors before interpreting a marker. These
sources were checked against the local Linux v6.12 tree, commit
`adc218676eef25575469234709c2d87185ca223a`.

`-EIO` here means an unrecognized block-status response; it does not assert
that `0x50` means retired, identify its physical cause, or repair page data.
The change adds no command, retry, reset, NAND write, or probe operation.
Existing status constants and the data/ECC read path remain unchanged.

Run the focused offline regression:

```sh
python3 tests/test-h2fmi-block-status.py -v
```

It extracts and compiles the actual callback and status constants with an
explicit fake `h2fmi_read_page_at()`. The four tests cover all 256 status
bytes, six transport errors against every byte (1,536 combinations), one
reader invocation with null data/OOB buffers, block alignment including
64-bit arithmetic, and the reported `0x50` response. The high-offset fixture
tests callback arithmetic directly; it does not bypass or test the MTD core's
device-size validation. Compilation uses `-Wall -Wextra -Werror` and UBSan.
No MMIO or devices are accessed.

With `CASCADIA_H2FMI_SOURCE` pointing to the unchanged upstream driver, the
same regression fails the all-status and `0x50` tests and passes the transport
and offset tests. The candidate passes all four tests. Thus the control
demonstrates the classification defect without changing transport semantics.
