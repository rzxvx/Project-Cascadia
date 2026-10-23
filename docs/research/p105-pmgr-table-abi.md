# Correct P105 PMGR diagnostic table ABI: 44 bytes

No device access or runtime driver changes. Existing PMGR/USB leaf gate mapping
remains unchanged. Candidate modifies only the offline metadata diagnostic.

The pinned P105 12H321 static ADT SHA256
5c346586116de1bb32606acb27cad3158bcaa8a1318a10ee9631de09884cfd72
contains `pmgr/device-clocks` of **5104 bytes =116×44**. First seven u32words
occupy+0..+27, name is sixteen bytes at+28. Older upstream mapper/docs claim
4176/116×36 and name+20. Running that original mapper misaligns our input.

This is not a bootloader-ABI transformation guess. Original S5LPerformance
code in kernelcache12H321 SHA256
db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c
looks up `device-clocks` at80b8c422; count block80b8c43e..45e divides its
OSData length by44 (magic0xba2e8ba3 UMULL then highword>>5). The actual loop
sets stride0x2c at80b8c474 and increments pointers by0x2c at80b8c4ac/4b4.
It reads IDbyte+3, parentbytes+8 and gateindex+12. iBEC1e9fc updates only
`bridge-settings`, not this table. No evidence of a 44→36 conversion was found.

`emulate-count12.py` runs that original count/retention block with fake OSData
getLength/getBytes providers. Seven controls PASS (0,43,44,88,4176,5104,
UINT_MAX); 5104 produces116, while4176 produces94 under this native parser.
This confirms actual ABI arithmetic, not the candidate's guessed struct size.

`adt-pmgr-map.py` candidate uses44 by default, name-at-entry-end, preserves
all existing first-five-word mappings/output formatting, and adds an explicit
`--entry-size 36` compatibility option. No heuristic auto-format decision is
made. Empty/truncated/oversized/non-ASCII tables are rejected before output;
unsupported entry sizes rejected. Two additional words+20/+24 remain opaque:
no voltage/frequency/enable meaning or clock state is invented for them.
The existing standalone ADT walker is unchanged; this fix is scoped to
`device-clocks` table decoding, not a general ADT parser replacement.

Historical control is the **actual original repository blob**
42bec65d851f0b2f5a09ee57294a4a3902d104bc, preserved exactly as
`tests/fixtures/p105-pmgr44/historical-36-map.py`. Tests independently verify
its Git blob hash before executing it against a synthetic input. No Git history,
network transport or external device is required; shallow clones/source archives
can run the same negative control. A wrong historical fixture fails.
A normalized source-bound subset of eight original rows is encoded as a
synthetic ADT for portable tests; it contains metadata only, not firmware bytes.
Six tests PASS/no skips, including old mapper missing USB-EHCI versus candidate
showing id89/PMGR3f101088, independent map expectations for all four USB
controllers, all metadata names/words, explicit36compat, malformed tables,
unmapped zero indices, and original-instruction count oracle provenance.
Actual complete static table CLI output also parses all116 entries successfully.

Relevant parent dependencies, decoded only (not activated):
USB-EHCI89 parents[40,0,0,0]; USB-PHY87 parents[40,42,0,0]; USBREG42 gateindex38
(PMGR+1098), clockindex45 (native +0xec; see the clock-column correction). These make clear that leaf ID87's zero
register does not mean all its **parent** clock/supply prerequisites are absent.
This table alone does not authorize parent-gate writes or prove native parent
ownership/release graph closure. Clock interface dynamic provider remains open.

Public offline regression: `python3 tests/test-pmgr-table.py`. The two source-bound
metadata/count fixtures are in `tests/fixtures/p105-pmgr44`. Original instruction
execution remains private and requires the verified official image.
