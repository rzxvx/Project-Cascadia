# PPN PIO partial-tail copy

The userspace `ppn_data_out` helper could copy up to three bytes not delivered
by the current transfer when a requested length was not divisible by four.
It copied `n` bytes from its word buffer while requesting only
`nw = n & ~3` bytes for tails of at least four bytes. A later successful
tail read overwrote those bytes, so the final successful output was correct.
If that later read failed, the extra bytes remained in the failure buffer.

This is a concrete allowed caller case: `nandctl getfeat` permits length1029
within its4096-byte buffer. After1024+4 bytes, the third PIO transfer can fail;
the old helper has already copied byte1028 from the earlier full-chunk word
buffer. A strict fake writes only delivered bytes, so the historical control
reproduces this deterministically without invented poison or hardware access.
`nandctl page` likewise permits unaligned lengths up to0x4400. Current iosnand
metadata/raw reads and the kernel driver's fixed raw page length are aligned
to four bytes and do not exhibit this tail condition.

The narrow correction copies `min(n, nw)` bytes. Command order, requested
PIO lengths, FIFO consumption and control-register values remain unchanged.
Later successful tails still complete the requested logical data; failed
tails leave bytes beyond the actually delivered prefix untouched. No opcode,
setter, geometry, program/erase operation or write permission changes.
Callers must still reject failed reads before publishing their buffers.

`python3 -B tests/test-ppn-data-out.py` extracts the actual helper and runs
strict register/PIO API fakes under ASan/UBSan. It exhausts every length0..0x4400
with independent byte-pattern and destination-guard checks, then injects
ten partial-transfer failures. The17419 cases pass for the fixed helper.
Another17419 cases check the actual historical helper from local Git blob
`9eecbefcfe82e0d304741fb66339af8fc30b681b`, SHA256
`70642b8f03ace401971c748ab942e8aa7bbb3f25660d964fcf04a1ab69abf3a5`,
including its known stale-tail behavior. Git access uses `protocol.allow=never`;
no fetch or fallback is attempted when that pinned local object is absent.

The tests passed on host and Ubuntu22 image without device mounts, firmware
execution or MMIO. They prove software copy bounds and failure-prefix
behavior only; actual NAND timing/controller behavior remains unvalidated.
Length zero still issues the existing read-side command in the fake trace
and is not presented as a proven hardware no-op.
