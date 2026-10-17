# Native PMGR direct clock column correction

Private candidate only; no device/MMIO access or power enable changes.
Pinned 12H321 kernelcache db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c and P105 ADT 5c346586116de1bb32606acb27cad3158bcaa8a1318a10ee9631de09884cfd72.

Actual native map retention leaves PMGR base unchanged. Deferred direct-clock code adds 0x38+4*index, using packed low6 indices1..51; zero terminates, highC0 flags end direct processing, startup rejects low indices>=52 across all four bytes. USBREG42 changes diagnostic clock column from b4 to ec; gate1098 is unchanged. Explicit 36 ABI remains physically unverified, invalid44 rows cannot certify a gate.

8 independent offline tests PASS on host and existing Ubuntu22 image. Historical control comes from exact local Git blob1887bed05c8b485fbde22aeb4fce35c6a29ad122; no network transport and missing local blob fails. Native fixtures are oracle-physical-v3.json/oracle-bindings.json; provided dump is synthetic input only. Patch migrates one existing clock-column assertion and corrects historical PCLK3 attribution without claiming a new measurement. Parent owns application/export.

Public tests: `python3 tests/test-pmgr-clock-column.py`. Historical layout-only
control is an exact source fixture with its independently verified Git blob hash;
no repository history/network/device is required at test runtime. Native physical
execution remains an offline fake-MMIO comparison, not an electrical test.
