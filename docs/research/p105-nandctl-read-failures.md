# NANDctl failed-read publication

`nandctl` formerly published payload bytes after a read-side API returned a
transport failure: ID bytes, feature/parameter hexdumps, firmware strings,
decoded boot pages and raw page files. For `page`/`bootpage`, file opening in
write mode could truncate an existing output even when the read had failed.
The diagnostic's failure marker did not prevent that payload publication.

The narrow gates check the existing API return before printing payload,
decoding boot data or opening an output. Status diagnostics remain available.
`ppninfo` continues inspecting both buses, publishes each successful query,
and returns failure if either claim or read fails. The firmware query after
`recover` follows the same payload gate; recovery operations themselves are
unchanged. No status byte is reinterpreted, and no command or setter is added.

`dump` already replaces failed read bytes with zero data and status zero.
The test verifies that no failed payload reaches its writes and retains that
existing diagnostic-dump schema/exit behavior. This patch does not change
successful file-output semantics, fix every CLI input-validation concern,
or permit unapproved NAND/recovery operations.

`tests/test-nandctl-read-gate.py` compiles the actual current `nandctl` main
against strict fake PPN, register, output and file APIs. No device, MMIO,
recovery action or real output file is accessed. The 38 cases cover all
single-read commands in success, read-failure and claim-failure modes;
both-bus `ppninfo` failure combinations; preserved prior mock-file content;
and explicit zero/status output on a failed dump. Existing good status40
with a failing return models a late read-completion error without inventing
new status semantics.

Another 38 historical control cases compile the actual original main from
local Git blob `27001e89faaae57551cb6f697c29fae175c11a10`, SHA256
`31ffd55cc469a5bbf0948ebff736957eca8ac86808c9af52ebf04163491ac891`.
They positively demonstrate the earlier failed-buffer publication and
all-success `ppninfo` exit code. Git is restricted with `protocol.allow=never`;
missing or changed historical input fails without fetching a replacement.
Host and Ubuntu22 ASan/UBSan cases pass; actual hardware behavior is untested.
