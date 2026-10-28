# Actual D1946 register-bank owner and handoff failure — original source evidence

Primary inputs: official p10513G36 kernelcache SHA256 `8eae16f060a71d4f3790385951588f8198d0f21cdb74146f9750941a3d764125`,18968576B; existing p10512H321 kernelcache SHA256 `db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c`; actual temporary-boot12H iBSS SHA256 `67d8f8d6fa82e220f436233e89ba0d411b948d5d5aeee1289e2a0d268b03bfb5`. All execution is original instructions in fake RAM/lower-I2C, never a device access. No Apple code bytes are distributed.

## Newly closed physical-access software descriptor

13G PMU ctor81144f00 stores actual vptr8114d214. Its+3a0 slot contains81145cf5. That actual method81145cf4 loads its PIC literal at81145cfc, adds PC81145cfa, and returns const **8114e458**. Bytes are **01 03 57 1e 01**. Common Dialog PMU startup811408f4 dispatches+3a0 and stores the returned descriptor in object+70 (81140952), bus provider+78 and serialized access state+7c/+80.

Common helper **81140c48** consumes this descriptor before transfers. It proves fields by actual use: byte0=one wire address byte; byte1=3 banks, valid0..2; byte2=shared-page1 endpoint57; byte3=selector register1e. Byte4 is consumed by separate startup/system publication, not named analog configuration here. There is no OTP/CV interpretation of any field.

The actual helper extracts requested bank=`logical_register>>8`, caches current bank at PMU+84, and sends lower-bus command2 with address prefix1e and a single bank byte when a change is needed. Only successful lower-bus return0 updates+84. Failed selection propagates error and prevents the enclosing data transfer. The selector is **written**; there is no hardware selector readback/read semantics in this path.

Bank1 has a firmware shortcut: when requesting another bank from cached1, `logical_register+count <0x57` skips selection. Equality switches. Thus0a+6,43+1,44+2,53+1,55+1 skip, while56+1/55+2/57+1/61+1 select0. This establishes the **software shared-prefix contract**, not physical mirrored-register behavior independently. Bank2 has no such shortcut. A matching cached bank skips the selector, trusting software cache.

The same exact descriptor fields exist in12H321: actual PMU vptr80ed3208+38c→80ecc445; method80ecc444 PIC resolves const80ed44f0 bytes **01 03 57 1e 01**. Slot numbers changed between versions; the descriptor link is separately checked from each actual vptr/code, not an old-offset substitution. This bridges the12H boot sequence to the matching D1946 bank-access ABI.

## Complete original read/write wrapper qualification

For13G actual PMU+378 points to **81140cb0** (read) and+37c to **81140b4c** (write). Do not infer direction from function address ordering. The read's lower provider command is1, write's is2, both via provider vptr+350. Each calls81140c48 before constructing the single low register-address byte and passing buffer/count to the lower provider. The selected source callers confirm read versus write by their buffer use.

`emulate-bank-transfer-private.py` executes the descriptor, bank helper and full wrappers with original kernel globals/import cells mapped in fake RAM. Current-thread helper is explicitly mocked with an already-held serialized-owner fixture, so it does not prove original mutex implementation. Lower I2C and diagnostic logging are mocked; original packet construction/selector/cache/error/stack-guard paths execute.

Selector failure prevents data. A data failure propagates but leaves the original cached selection unchanged. Explicitly desynchronizing fake hardware bank2 from cached0 makes the original wrapper skip selection and access that fake bank: **it does not detect inherited/cache mismatch**. Distinct per-page fake bytes are synthetic; this is a cache-trust counterexample, not measurement of a physical bank's contents.

Six selector/descriptor groups and six full transfer-wrapper groups pass,0skip (`test-results.log`,`transfer-tests.log`). `native-page0-oracle.json` in the sibling Linux group contains16 normalized successful original read/write packet cases. It records original hashes/entries/descriptor and has no executable bytes. Reads cover status/masks/control/current/ADC and selected odd-current slots; writes cover known experiment controls only.

## Last temporary-boot selector may not be0 despite successful wrapper

The existing iBSS original wrapper34007cb8 calls actual eight-pair loop34009c24 and returns0. The ordered selector sequence is1e02, then1e01, then1e02, finally1e00, with raw intervening d0/ef/ee writes. Their **bank-selection role is now source-established** through the matching12/13 D1946 descriptor; their OTP/analog role remains unknown.

`ibss-final-selector-private.py` executes this actual wrapper/classifier/loop using the already frozen hash-pinned iBSS fake-I2C harness. Successful transfers record final selector0. Failure of transfer8, the final1e00, records last successful selector2 **while wrapper still returns0**. `ibss-final-selector-results.json` preserves both complete traces. This is a last-success wire-state model, not a physical PMU simulation. It proves boot return0 cannot attest successful bank0 selection.

Accordingly a flat8-bit Linux transport must not blindly equate inherited boot completion or cached0 with actual page0. A source-qualified global owner can select0 with checked transport before known page0 accesses and invalidate/latch on uncertainty. Bank0 qualification still says nothing about whether earlier configuration writes succeeded or what CV/termination/protection/OTP defaults are.

## Reproduction

Use already present Python Unicorn in the resource-limited container and matching read-only KC13 input, optionally `CASCADIA_TEST_KERNELCACHE13`. Original tools: `emulate-bank-private.py`, `test-bank-private.py`, `emulate-bank-transfer-private.py`, `test-transfer-private.py`. The iBSS last-selector proof additionally references the existing frozen private `../ibss-analog-owner/emulate-ibss-init-private.py` and `/fw12/iBSS.dec`; it is not a normal build dependency.

No register dump, blind selector read, OTP-table replay, firmware patch/transfer, normal driver change or physical charging operation occurs. Each call is instruction bounded; all I2C effects are explicit mocks.
