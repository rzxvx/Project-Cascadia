# Read-only path audit: PIO end-of-operation error

2026-10-12. Local correction; no device/MMIO/kernel API execution.
Baseline source is the pre-fix tools/nand/ppn.c. Actual four
source functions extracted into C with strict command/status/data/register
fakes; actual iosnand.c read_lba is included to check page-cache publication.

## Reproduced defect

Successful command/status/data followed by failing ppn_cmd1(0x77) returned0
from ppn_read_page. ppn_end returned void, so ppn_get_feature and ppn_params
also ignored late end failures. Real iosnand read_lba publishes a physical
page-cache key when ppn_read_page ret0 and status40/42; a late transport
failure was therefore eligible for success/cache publication. Kernel H2FMI
already propagates its end command failure; this fixes default userspace PIO.

The original source falsely returns success for data followed by an injected
end timeout. The automatic control verifies the exact pre-fix local Git blob
`9eecbefcfe82e0d304741fb66339af8fc30b681b` (SHA256
`70642b8f03ace401971c748ab942e8aa7bbb3f25660d964fcf04a1ab69abf3a5`).
It executes 23 historical controls and 23 corrected real-source cases on host
and Ubuntu22 under ASan/UBSan, and checks that the command allowlist, setter
and command dispatch remain byte-identical. The injected timeout is -ETIMEDOUT;
the actual low-level wait helper currently reports a nonzero generic failure. A dedicated read_lba case injects GOOD page bytes/status and
end timeout; output stays unchanged and cache key remains0. Successful control
case publishes the expected cache key and reads all4096 data bytes correctly.

## Minimal correction

Only ppn_end, ppn_get_feature, ppn_params and ppn_read_page error returns
change. Existing command order, CE release, device-reset restoration, opcode
allowlist and power-state setter are byte-identical apart from untouched
surrounding context. Earlier command/status/data failures remain failures;
an earlier data error wins over a later end error. No program/erase or new
feature operation is added. Fake R/W callbacks inspect arguments and never
dereference even the in-process address tokens.

Run the actual production functions offline:

    python3 -B tests/test-ppn-read-end.py

The fixture uses fake register and API operations; it never opens a device.

## Negative coverage and remaining boundaries

- Current iosnand metadata/raw lengths and H2FMI raw length are divisible4;
  the generic data-out rounding/tail loop is not a new overrun on these paths.
- H2FMI OOB offset/null-buffer bounds are checked by actual Linux6.12
  mtd_check_oob_ops before the driver; its erased/refresh/bad-page progress,
  fatal stale-buffer avoidance and ECC accumulation already have real-loop
  fixtures. No new kernel change justified by those checks.
- MTD MEMREAD requests are fixed16K+64 and reject changed returned lengths,
  uncorrectable ECC, ENOTTY, positive ioctl return and nonretired EBADMSG;
  geometry/type/rdev checks bind the opened die. Existing fixtures cover them.
- Cache block write pointers/u48 heads are validated before mapper indexing;
  all record bytes are CRC-covered; actual read_lba rechecks LBA+exactsequence
  before exposing data. Page cache keys are invalidated before failed fills.
- NBD allocation occurs only after subtraction-based bounds checking. Existing
  real socketpair fixtures cover truncated frames, failed allocation, rejected
  write draining and flush framing. Its signal semantics are not changed:
  EINTR after a requested termination should not be blindly retried forever.
- Cache pathname races and PIO cooperative-lock scope remain documented
  limitations; this audit does not upgrade them to hostile-process leases or
  prove live NAND immutability. No hardware validation or writable FTL follows.
