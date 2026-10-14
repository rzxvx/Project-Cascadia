# Reject damaged HDQ command echoes

Independent change against main `7373331a9f1c88d347773736a19dc3166947864d`.
Only four lines in `hdq_read8()` change. Read16, cache, PRESENT, UART setup,
break timing and response decoding are unchanged; PR9/12/15 remain separate.

The driver expects eight echoed command pulses followed by eight gauge
response pulses. Previously it checked only `n >= 16`, then decoded the last
eight bytes. A full response with a corrupted command echo was accepted as
a successful measurement. The added comparison rejects each incorrect echo
byte with `-EIO` before accepting the response value. It compares with the
driver's own emitted `0xFE`/`0xE0` bytes; it does not change pulse encoding.

Primary source: official iPad2,5 iBoot13G36 plaintext SHA256
`7dc177231f52e7032cd10ddb255c3a8fd6f62c33c5e4d731eda7e5e0a8b8fdcd`.
In HDQ transaction `0x9ff14b50`, command bits are encoded at
`0x9ff14b88..14b98` and sent to UART5 at `0x9ff14bac..14bb8`. Its encoder
uses `0xFE` for one and `0xC0` for zero. After collecting exactly the command's
eight echo bytes through `0x9ff14c60`, the loop at
`0x9ff14bdc..14bf2` compares every byte against that encoding. A mismatch
jumps to `0x9ff14c3a`, logs HDQ read mismatch and returns `-1`; the response
reader at `0x9ff14c12` is not invoked. The valid read8 wrapper
`0x9ff14cec` requests eight command and eight response pulses.

Private hash-pinned execution of these actual instructions passed nine
cases: one valid echo/response, and one mismatch at each of eight echo
positions. Break, UART byte I/O, memory clear and logging were explicitly
mocked; the original command encoder and echo comparison executed. No
firmware bytes are included in this patch or regression test.

```sh
python3 tests/test-hdq-command-echo.py -v
```

The portable test extracts the actual driver's `hdq_drain/read8` bodies.
Four UBSan/Werror groups pass: all 128 read commands with all 256 data bytes
(32,768 successful synthetic transactions); all 255 wrong byte values at
each of eight echo positions (2,040 rejections); short RX lengths 0..15;
and a valid next transaction after an echo failure. The unchanged main
source, supplied via `CASCADIA_HDQ_SOURCE`, passes valid/short RX cases but
fails both corrupted-echo cases. A synthetic immediate break clear explicitly
isolates this test from the separate break fix; it is not evidence that A5
hardware self-clears.

The source-backed software contract is exact echo matching, but Linux's
current zero-pulse encoding differs from iBoot's. The live comparison below
checks acceptance of normal traffic. The change does not prove complete framing integrity for extra
bytes, nor assign unverified UART error-status bit meanings.

PR9's earlier framing fixture always echoed eight zero command bits, even
when the command contained ones. A companion test-only update in PR9 now
echoes the actual transmitted bytes. Its full transport/cache/property
control passes both on PR9's source and with this comparison integrated
privately. Its existing transport/cache assertions remain applicable. No production
read16 or cache changes from PR9 are included here.

No additional cache or unit defect was established beyond the existing
reviewed fixes. Gauge telemetry alone does not activate charging: the normal
driver has no charger setter, the observed USB gadget declares zero bus current, and the
separate current/source/PMU-owner candidates remain uninstalled/default-off.
Analog CV/termination/fault-retention qualification and a real source budget
are still required before those experimental charging paths can be activated.

## Reversible live acceptance check — 14 October 2026

A separate external module, built against the exact frozen RAM kernel,
combined this echo comparison with the already tested PRESENT error and
explicit-break corrections. Only the gauge driver was replaced; no charger
control or NAND operation was added. The previous driver remains loaded for
the prepared rollback. The combined source passed the four echo C groups,
with poll-clear success explicitly mocked to isolate echo handling, and a
complete ARM `W=1`/`-Werror` module build without warnings. Original kernel
sources were mounted read-only; the build used at most two CPUs and 1 GiB.

After binding at 07:46 UTC, twelve samples six seconds apart produced 60
successful sysfs property reads over approximately 71 seconds. The initial
probe and these routine refreshes therefore accepted Linux's own FE/E0
command echoes. The battery reported 77%, about -384 mA, and 31.3 degrees
Celsius; it was still discharging. The visible XFCE and RAM SSH session
continued running.

The module SHA-256 is
`3487b31757268c88cb14beb39d9cefc459386d7347ce8c67fdc4b323a5b219ab`.
This short check neither injects a physical echo fault nor boots the complete
main-based PR head. It does not establish long-term reliability. The external
module adds the expected kernel taint. Rollback is prepared, but was not
required or physically exercised during the successful check.
