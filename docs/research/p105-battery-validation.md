# Reject invalid battery percentages without replacing the cache

The HDQ driver's previous refresh accepted any successful 16-bit state-of-charge
read, including 101 and 65535, and published it as `capacity`. Linux's
[power-supply ABI at the pinned v6.12 revision](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/Documentation/ABI/testing/sysfs-class-power#L263)
defines that property as a percentage from 0 through 100.

The refresh now returns `-ERANGE` for a BQ_SOC result above 100 before changing
any cached measurement, validity flag or timestamp. It does not clamp corrupt
data to a plausible full-battery reading. Both endpoints, 0 and 100, remain
valid. No additional thermal, voltage or current thresholds are inferred.

`tests/test-hdq-battery.py` compiles the actual HDQ/cache/property C functions
against a fake UART. The new capacity assertions failed against the previous
driver, then passed with the guard. They verify that invalid 101/65535 reads
return an error while preserving the complete prior cache and caller output;
0/100 each publish a complete successful nine-register sample. Existing
timeout, read-failure, cache-wrap, current, temperature-unit and lock checks
remain in the same UBSan/-Werror fixture.

```sh
python3 tests/test-hdq-battery.py
```

The fake gauge does not validate physical measurement accuracy, battery presence
or charging safety. PRESENT still describes whether the existing driver could
obtain a valid sample; it is not proof of a dedicated battery-detect signal.
The change adds no HDQ write command or charging-control path. Target hardware
verification remains required.
