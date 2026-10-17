# p105 boot PMU current/readback and missing CV/termination evidence

Local source-evidence checkpoint, 2026-10-12. The offline tool and tests are
reviewable repository artifacts. They make no device transfers or PMU accesses
and add no active driver implementation. The probe executes bounded original
boot-helper instructions from a separately supplied, hash-pinned local image.

## Pinned images

- Local iBEC plaintext `build/firmware/iBEC.dec`: SHA256
  `30c040052c444222edc8646316f95a355a9e338d6aadc7168f8a7acc7c4578ec`,
  282624 bytes, base0x9ff00000; iBoot2261.30.37 for p105.
- Local iBSS plaintext `build/firmware/iBSS.dec`: SHA256
  `67d8f8d6fa82e220f436233e89ba0d411b948d5d5aeee1289e2a0d268b03bfb5`,
  77824 bytes.
- iPad2,5 iOS8.4.1/12H321 kernelcache SHA256
  `db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c`.
- Raw p105 ADT SHA256
  `5c346586116de1bb32606acb27cad3158bcaa8a1318a10ee9631de09884cfd72`.

These files were already present in the local research checkout; they are not
distributed with the tool or required to be present in a fresh clone. No new firmware download or execution
on hardware occurred. Absolute literals and existing upstream RE agree on
iBEC base0x9ff00000. No Apple instruction bytes are embedded in the new probe.

## Concrete low-battery predecessor

The string `low-battery precharge` is at iBEC offset0x3df0f, absolute literal at
0x9ff256b8, loaded at **0x9ff2536e** in function **0x9ff25328**. That function
calls current policy **0x9ff24ed8**; it in turn calls PMU setter
**0x9ff0c540** at **0x9ff24fdc**.

The inspected PMU setter:

- **0x9ff0c4ec** initializes its cached ceiling from register0x3f low six bits;
  getter **0x9ff0c4d0** multiplies them by40. Cached initialization happens
  before a checked read result; the whole path does not establish a valid
  silicon configuration after transfer failure.
- **0x9ff0c540..c6c4** reads0x1b/0x3d, writes current-slot registers
  0x4b/4d/4f/51/53 for a nonzero charge request, writes0x3f, changes0x1b/0x1c
  low-input mode bits, then programs0x3d with a staged current-code increase.
- The inspected code clamps the requested input at2100 and uses an existing
  thermal current table. No literal CV/termination target was established.
- This boot setter writes0x3f with its selected code/control value; the kernel
  source setter preserves additional3f bits. They are different implementations
  and cannot be collapsed into a guessed Linux register API.

The `reset charge timer failed (%d)` log is at offset0x3dfbe, literal0x9ff25750,
loaded at0x9ff2573c in function0x9ff25708. The called timer helper
**0x9ff13718** is exactly `movs r0,0; bx lr` in this image. Its companion
**0x9ff0c43c** is `bx lr`. The log does not prove an implemented hardware charge
timer reset. It also does not prove the chip lacks an autonomous safety timer.

## Actual write/readback failure paths

`tools/power/emulate-ibec-pmu-write.py` executes both original helpers from the pinned input:

- Charger-specific **0x9ff0c6fc**: passes a register/value two-byte buffer to
  I2C write **0x9ff0bc08**, address byte0x78. If verify argument is1, passes a
  one-byte register read to **0x9ff0bae0**, address byte0x79, requesting one
  output byte. It **does not compare** the returned byte with the requested
  value. With verify enabled, its final return is the read helper's return;
  a failed write followed by successful read can appear successful. With
  verify disabled, the write helper's return remains in r0.
- Generic **0x9ff0be6c** uses the same transfer helpers. With verification it
  compares a stack output byte to the requested value and returns−1 if
  different. It does not preserve the transport status; a failed read can be
  followed by a successful numeric comparison. Without verification it
  unconditionally returns0 after the write.

Eight actual-code fake-I2C tests passed. Both constructors and CLI/test loaders
check the exact iBEC hash. No data-node or network access is used. Fake PMU byte
0x3f starts at0xbf and all stack RAM is zero; this is a fixture, not live state.

| Injected situation | Charger-specific return | Generic return |
| --- | ---: | ---: |
| Write/read succeed, matching value |0|0|
| Readback byte differs |0|−1|
| Write fails, successful read of unchanged0xbf |0|−1|
| Failed read after requesting zero, zero stack |−1|0|
| Write fails, verification disabled |−1|0|

The failed-read row does not establish that live stack bytes are zero. A real
transport error must terminate the transaction regardless of byte equality.
The probe emulates neither electrical current nor fault behavior. The complete
current setter, low-battery loop, startup and hardware I2C engine are not
executed by these eight tests.

Reproduce from the repository:

```sh
python3 tools/power/emulate-ibec-pmu-write.py
python3 tests/test-ibec-pmu-write.py
# Explicit input outside the checkout:
python3 tools/power/emulate-ibec-pmu-write.py /path/to/p105/iBEC.dec
CASCADIA_TEST_IBEC=/path/to/p105/iBEC.dec python3 tests/test-ibec-pmu-write.py
```

Both default paths resolve `build/firmware/iBEC.dec` relative to the repository
location, independently of the current working directory. The test module
requires Unicorn and the pinned local input; missing optional dependencies or
image produce eight explicit skips without importing the tool. A present image
with a wrong hash fails rather than skips. The CLI expects Unicorn; a missing
input is reported as an argument error. No request to modify the boot helpers
is made. The I2C hooks accept only the observed port/address/count and bounded
fake stack pointers. Emulation is limited to 20000 instructions per fixture.

## Kernel PMU startup writes inspected separately

Actual D1946 startup code **0x80ecb976..ba26** invokes GPIO defaults helper,
reads/writes0x9f with bits0..3 set, reads/writes0x1c bit0 set, and conditionally
writes register0x3b from provider property **dwi-buck-control**. Property PIC
literal0x80ecbc78 + PC0x80ecb9f2 resolves string0x80ed22bb. The raw p105 ADT
contains this property as05. It is not `pmu-chargetrap` and must not be renamed
as a charge-voltage target.

Other inspected startup blocks mutate0xd3 with mask0xcb/bit0x10 and later
bit0x08, and collect GPIO configuration/event/mutex state. The previously
executed Wake/Quiesce paths modify3f bit7 and1c bit0, with their analog meanings
still unresolved. No register meaning is invented from proximity to charging
registers or from an unrelated public PMIC data sheet.

Source initializer **0x80ed0558..067c** reads3f into cached ceiling, obtains
USB-current calibration and five temperature-slot maxima, and registers event
masks. It establishes no explicit CV/termination programming in that inspected
range. The existing zero-current/status/watchdog/ADC work describes additional
runtime paths; none proves complete analog safety initialization.

## Exact negative search coverage

This is bounded coverage, **not** proof of absence of CV/termination hardware:

1. Case-sensitive ASCII searches in both pinned plaintext boot files for
   `d1946`, `pmu`, `charge`, `termination`, `battery`, `voltage`. iBSS exposed
   only `usb charger` among charging strings. iBEC exposed precharge/timer logs,
   charger/boot-voltage keys, PMU clock and unrelated CPU rail-voltage names.
2. iBEC code windows manually inspected:0x9ff0be6c..bfec,
   bff8..c1fa, c214..c43c, c4d0..c854, current-policy24ed8..2500c,
   precharge25328..25694 and timer25708..2574a.
3. Halfword-aligned Thumb scan of offsets0..0x3b000 for direct `bl`/`b.w`
   calls to be6c/c6fc found19 candidates. Relevant instructions were checked
   in their original function contexts. The9 c6fc sites are current-slot3f,
   input1b/1c/3d and ADC43 paths; none establishes CV/termination. The10 generic
   helper sites are ADC43, cached0xa0+logical-register routing, GPIO61..72,
   table-driven byte writes and control bits in20/21/22/23/25/26.
   Indirect calls/data-driven tables are not an exhaustive whole-program proof.
4. Kernel D1946 startup/source, status/recovery, event, Sleep/Wake/Quiesce,
   system watchdog and ADC paths already pinned in the research ledger.
5. The full raw p105 PMU34-property and charger19-property nodes were read.
   They supply current caps/calibration/GPIO dispatch, not a demonstrated CV
   or termination register description.

Still unexamined or incomplete: physical boot-ROM PMU setup, OTP/default
configuration, full provider lifetime/failure cleanup, every indirect callback,
all data-driven boot write-table owners, separate actual iBoot image and live
readback. Any one of these might configure protection; no absence claim is
justified by a missing named Linux callback or log string.

## Manufacturer provenance limits

A first-hand [iFixit A1432 teardown, published2012-11-01](https://www.ifixit.com/Teardown/iPad+Mini+Wi-Fi+Teardown/11423)
identifies the package marking **Apple343S0593-A5**. It does not supply a charger
register map or identify a manufacturer for that particular entry. The local
firmware class/ADT identifier is D1946/pmu,d1946. An authoritative manufacturer
cross-reference between those identifiers and a public programmable-charger
data sheet has not been obtained.

Official-domain searches on2026-10-12 for D1946, D1946A5 and343S0593 on
Renesas, Dialog and Apple domains found no matching public register document.
The public [Renesas DA9168 data sheet](https://www.renesas.com/en/document/dst/da9168-datasheet?r=1564756)
is a different, explicitly named part and was **not** used for D1946 register
semantics. Rehosted schematic/vendor-reseller search hits and secondary
manufacturer claims were not promoted to authenticated primary manufacturer
evidence. This search result is a documentation gap, not a claim that no
manufacturer documentation exists.

## Concrete engineering consequence

There is now a reproducible actual boot readback/error counterexample to avoid
copying. A Linux PMU programming transaction needs checked prerequisite reads,
checked write status, checked read status and verified value/mask, plus known
lifecycle/physical safety semantics. This provides useful failure requirements
without inventing CV/termination values. Those missing configuration and safety
proofs remain blocking prerequisites for an active charger implementation.

## Later independent boot-stage qualification

Independent root repeats of the frozen iBEC12/iBoot13 direct-table classifier/wrapper and13 byte helpers pass5+5 groups. A separate actual iBSS12 PMU initializer/classifier/RMW repeat passes5 groups. These scopes execute the original instructions with fake bus/GPIO/bytes, not a physical PMU or a model of analog protection.

The selected GPIO ordinal (tokens405/406/407, not hardware pin numbers) controls identical eight raw PMU pairs in the three stages:1e02,d001,ef10,1e01,ee0c,1e02,d000,1e00. Only ordinals<=3 execute this sequence; modelA1432 does not prove its physical ordinal. Earlier failures continue through the table, and the wrapper normalizes the last error to success. The meaning of d0/ef/ee is still unqualified; there is no CV/OTP-programming inference. Subsequent original13 bank-wrapper analysis identifies1e as the selector, but the flat old byte fixture does not itself model banks or prove switch acknowledgement.

The original iBSS initializer performs104 fake transfers in the selected revision45 path. Failure of its initial1c write is hidden by later success. Its40-bit40 RMW can lose inherited bits after a failed read and still report success.13 charger verification likewise can compare stale zero after failed transport. These are exact software counterexamples; successful boot return cannot attest a retained analog configuration. Checked Linux helpers must preserve transport errors and must not replay unqualified raw tables or write40.

The [actual END/TO recovery audit](p105-eoc-recovery.md) further executes7 source-method groups plus3 complete consumer/recovery groups. A failed41 restoration still clears the native software timeout reference. Linux retains the fault and performs no automatic recovery pulse.

Private independent repeat logs: analog-tables-root-repeat-v1.log, analog-write13-root-repeat-v1.log, analog-ibss-root-repeat-v1.log, eoc-source-root-repeat-v1.log, eoc-full-status-root-repeat-v1.log. No device access, physical protection/retention certification or normal feature activation follows.

## Resolved boot prerequisite identity — 14 October 2026

The previously mocked prerequisite before the direct PMU-table loop is
`nand_boot_init()`. Its actual literal loads identify the start/result logs
and NAND boot tag. This resolves the function's identity; it does not identify
an analog charger initializer or prove all transitive callees are PMU-free.

| Binding | 12H321 iBEC | 13G36 iBoot |
| --- | --- | --- |
| Prerequisite entry | `9ff11d44` | `9ff12a54` |
| Factory call | `9ff11e16 -> 9ff0d0a0` | `9ff12b28 -> 9ff0dc58` |
| Object-init call through context+320 | `9ff11e26` | `9ff12b38` |
| Published object's init target | `9ff0e808` | `9ff0e480` |

The 13G36 input here is **iBoot**, SHA256
`7dc177231f52e7032cd10ddb255c3a8fd6f62c33c5e4d731eda7e5e0a8b8fdcd`;
the 12H321 iBEC hash is listed above. These addresses must not be reused for
13G36 iBEC. Both factories identify themselves as `nand_part_init` and
`nand_part_core`, allocate an object of size 0x9ac, set object[0] to self,
store the boot context at +44, and publish the object through context+320.
The consumer loads the object's first two words as self and callback, then
calls the callback at object+4. A C++ vtable dispatch is not assumed.

The full prerequisite bodies and factory/store/consumer relationships were
inspected statically. Original-image hashes and all recorded literal values
and strings were independently checked. No old emulation group was rerun;
no new hardware or PMU access followed. This closes the earlier unnamed-call
gap while leaving the concrete blocker intact: authenticated meanings of
analog/OTP/default fields, checked initialization completion, and retention
through the final boot handoff. The adjacent raw d0/ef/ee table writes do not
gain CV, termination, OTP or NAND semantics from this identity.
