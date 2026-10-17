# Preserve unknown battery presence on HDQ refresh failure

The HDQ getter previously returned successful `PRESENT=0` whenever refreshing
the gauge failed. A communication failure could therefore announce a missing
battery. The getter now returns the refresh error without assigning the output
value. Successful observations still return `PRESENT=1`.

The pinned Linux 6.12 revision
`adc218676eef25575469234709c2d87185ca223a` supports this distinction:
[the power-supply ABI](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/Documentation/ABI/testing/sysfs-class-power#L452)
defines `present=0` as absent and `present=1` as present;
[power_supply_show_property()](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/power/supply/power_supply_sysfs.c#L271)
propagates a negative driver error before formatting a value.
Successful gauge communication remains this driver's existing presence
criterion; this fix adds no independent battery-detect signal.

This change is based directly on upstream main
`7373331a9f1c88d347773736a19dc3166947864d` and has no PR #9 dependency.
Only the normal getter's PRESENT error handling changes. Refresh/cache,
UART timing, word-reading and other property behavior are untouched.
Applications receive a read error during failure and can retry their next
ordinary observation. Charging is not enabled or changed.

Run `python3 tests/test-hdq-present.py`. The fixture extracts and compiles the
actual getter with a stub refresh result and mutex/supply infrastructure,
under UBSan and `-Wall -Wextra -Werror`. It exercises EIO, ETIMEDOUT, EREMOTEIO
and ERANGE; unchanged caller output and balanced locking; successful `PRESENT=1`;
existing current/voltage/temperature conversion and error behavior; and static
TECHNOLOGY without refresh. It does not test refresh/cache internals or rely
on PR #9's sample-publication fixes.

Against the unchanged upstream getter, the exact PRESENT assertion fails while
the other three tests pass. With this fix all four pass, with no skips.
`CASCADIA_HDQ_SOURCE` can select a local historical driver for reproduction.
This focused regression uses no hardware. The corrected kernel has not yet
been booted; the currently running RAM image still has the old getter.
