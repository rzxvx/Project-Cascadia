# UART5 HDQ break pulse

Independent candidate based on `7373331a9f1c88d347773736a19dc3166947864d`.
Candidate preparation was offline; the separate runtime comparison is described
below. The read16,
cache, property/PRESENT and UART setup code is unchanged. PR9's bounded
break wait overlaps this read8 hunk and must be reconciled with this explicit
pulse sequence when combining the changes.

The old driver sets UCON bit4 and waits for it to clear automatically. Main
also sends command bytes if that wait expires. The explicit operation in the
original firmware for this device is set→200us delay→clear→40us recovery→RX
drain. The fix follows that sequence and checks the clear before transmitting.
A stuck clear returns `-ETIMEDOUT` without command bytes. Polling uses Linux
`readl_poll_timeout(...,10,5000)`; the old jiffies deadline at HZ100 offered
only the remaining part of one 10-ms tick, not a phase-independent 5-ms wait.

Primary inputs were decrypted from the project's official Apple iPad2,5 IPSW
downloads. These hashes pin the analysis inputs, rather than claiming a new
signature verification. No Apple executable bytes are included in this change:

| Input | SHA256 |
| --- | --- |
| iBoot13G36 | `7dc177231f52e7032cd10ddb255c3a8fd6f62c33c5e4d731eda7e5e0a8b8fdcd` |
| iBEC12H321 | `30c040052c444222edc8646316f95a355a9e338d6aadc7168f8a7acc7c4578ec` |

In iBoot13, HDQ transaction `0x9ff14b50` calls break helper `0x9ff14b04`
at `0x9ff14ba0`. The helper calls UART control `0x9ff162b4` with `(5,1)`,
delay API `0x9ff1c88c` with `(200,0)`, UART control with `(5,0)`, then delay
with `(40,0)` and RX reader `0x9ff16100` until it returns negative.
Control reads descriptor+4 and ORs/BICs bit4, preserving the other bits.
Descriptor lookup `0x9ff15d88` uses table `0x9ff4194c`, stride56; port5's
descriptor at `0x9ff41a64` contains UCON `0x32a00004`. This matches DTS HDQ
bus-local resource `0x02a00000` plus parent range `0x30000000`.

iBEC12 independently has the same set/delay/clear/delay/drain sequence in
helper `0x9ff13d98`, UART control `0x9ff1534c` and RX reader `0x9ff151bc`.
Lookup `0x9ff14e74` uses table `0x9ff424bc`; port5's descriptor at
`0x9ff425d4` again contains UCON `0x32a00004`.

The delay APIs feed interval scheduler `0x9ff1ba14` (iBoot13) or
`0x9ff1f540` (iBEC12). They add interval conversions `0x9ff01c0c` /
`0x9ff01b1c`, respectively, to the current timer counter. Both conversions
multiply a 64-bit interval by 24, and frequency getters `0x9ff01c1c` /
`0x9ff01b2c` return 24,000,000. iBoot13's reverse conversion `0x9ff01bfc`
divides by 24. This establishes microseconds in the software timer contract;
actual pin timing and clock frequency were not measured. The source proves
explicit clearing, not a universal claim that A5 hardware never self-clears.

```sh
python3 tests/test-hdq-explicit-break.py -v
```

Four actual-driver C tests pass with UBSan and `-Werror`: explicit operation
order across 1,000 HZ100 tick phases; rejected clear/timeouts before TX;
non-break UCON bits preserved; short/no RX response still returns `-EIO`.
The same tests against unchanged main using `CASCADIA_HDQ_SOURCE` fail all
four groups. MMIO, time, delays and responses are declared fakes. The small
Linux6.12 polling macro fixture retains its GPL header, exact upstream source
URL/revision and SHA256 in `tests/fixtures/hdq-explicit-break/source-provenance.json`;
the test checks its SHA before compilation. Native iBoot helper emulation was
separately verified against three fake UART/drain cases, with actual descriptor
lookup and bit operations and declared mock delay/RX providers.

The fix changes no HDQ command encoding or UART baud/framing configuration.
It introduces no gauge write command, charger control or PMU write. These
offline results establish software sequencing; a reversible live comparison
was subsequently performed to assess the reported `-110` failures.

Separate runtime validation tested a merged live variant containing frozen runtime
commit `1ff01fad8280632fa0a3f25ee9e4cd023a4a0be0`, the PRESENT error fix and
this explicit pulse sequence. Its module SHA256 was
`59b31230456cdc3ab115aa33b55c2939a63a1b2be007c26da256e575db6d2c41`.
The reported run comprised 12 samples 6s apart, each reading present/capacity/voltage/current/
temperature: 60 successful sysfs reads over 68s and no new no-answer warnings.
The earlier PRESENT-only variant had one `-ETIMEDOUT` in six samples.
This short observation supports the change but does not establish complete
reliability or a unique physical failure cause. It tests the merged live
variant, not main's older read16/cache implementation. No charging control
was changed, and the battery remained discharging. Candidate preparation and
regression were offline; this runtime observation was reported separately.
