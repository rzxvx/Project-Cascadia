# Unregistered Linux-C wall-source measurement candidate

This group is source-only and unregistered. `p105_wall_init()` always sets `enabled=false`; there is no activation setter, platform driver, DT node, normal overlay, Kconfig selection, power-supply registration, current budget publication or charger callback. It does not enable loading or charging on hardware. The successful results below are **synthetic observations**, not permissions produced by a real device.

Source: pinned iPad2,5 13G36 original iBoot, SHA256 `7dc177231f52e7032cd10ddb255c3a8fd6f62c33c5e4d731eda7e5e0a8b8fdcd`, with selected original evidence summarized below and independently reproducible via `check-original-oracle.py`. The matching 12H321 ADT independently points `function-brick_id_voltage` to PMU `adcV`, arguments channel6/scale65536, and `function-usb_dmonitor` to OTG PHY `DMON`: the existing public [charging investigation](../../docs/research/p105-charging.md). ADT dispatch identifies owners, not a Linux reservation interface or a physical calibration guarantee.

## Actual code boundary

`p105-wall-source.c/.h` compile against actual Linux6.12 ARMv7 headers (commit `adc218676eef25575469234709c2d87185ca223a`) as a single object. Real APIs are `regmap_bulk_read/read/write`, `mutex`, `atomic64`/`atomic`. The object has no init function and is not a loadable standalone driver. `Makefile` exists only for object validation in the disposable/private Linux copy.

Initialization requires entirely fresh zero storage exactly once and returns an error for NULL/reused storage. Capture rejects uninitialized sources and NULL/ERR_PTR resources before callbacks. Failed restoration retains the exact lease and error, as well as a latched fault; neither reinitialization nor retry can erase that uncertain ownership. Generation wrap also latches a permanent fault.

The call requires explicitly supplied Tristar/PMU regmaps plus an exclusive owner for **both Tristar routing and PMU ADC6**. The owner supplies acquire, validate, bounded settle, checked fresh ADC6 read, and restore/release callbacks. The owner must independently hold its resources/context until this synchronous call returns and report removal, role, orientation, fault and lease expiry. `p105_wall_invalidate()` is an atomic-safe generation/fault invalidation entry point; no real event producer is connected to it yet. Caller stop/remove must prevent new calls and drain existing capture before releasing regmaps/context. This group is not a managed supplier implementation.

No matching Linux owner currently proves `checked_restore_available`. When it is false, the call returns `-EOPNOTSUPP` after releasing its acquired reservation, **before any regmap access**. A positive fixture explicitly fakes that proof. No production provider, DT, boolean or test result is substituted for analog CV/termination/protection retention.

Inside a proved synthetic reservation, the candidate:

- Checks generation/owner validity, PMU status0a..0f USB presence and known status0b mask38 before each routing operation and around samples. These are status bytes, not read-to-clear event01..09. Mask38 is a conservative HOLD for the already decoded firmware status conditions; it is not a complete hardware-fault register map.
- Reads actual Tristar0c/06 qualifiers; checks stable orientation across routes. Original mode2/mode1/mode0/mode4 writes to Tristar03/01 are reproduced for the two orientation values. Every write checks its return, reads back, compares and revalidates; failed reads/writes/readback/epoch changes cannot publish a positive observation.
- Requests bounded ADC channel6 samples through the exclusive owner's checked conversion interface. It rejects raw values >4095. The original scaling is exactly `raw*5000>>12`; scalar units remain a firmware conversion, not a calibrated voltage claim. The settle callback receives original software intervals2000/40000; its actual timing implementation/provenance is the owner's obligation (no physical timing measured here).
- Executes exact original thresholds/ratio/ID arithmetic. A low-sample path takes one bounded third measurement; no cached ID is reused. Unlike the original, source/ADC/routing errors reject the result instead of becoming an implicit lower source class.
- Always releases an acquired reservation, even after intermediate failure. Restore failure latches a fault and increments generation, denying future capture. Route0 is an interim known neutral route, **not** a restored prior owner. The explicit restore/release callback must restore/drain the real resources; this group does not invent saved-register writes or assert that arbitrary Tristar03/01 reads/restores are physically safe.
- Publishes only an observation tagged with capture generation after successful completion/release. The lease has ended, so it cannot authorize current. There is deliberately no current budget/permission field. Concurrent later invalidation makes the old generation stale; this is not an instant physical cutoff promise.

A future real provider still needs primary evidence for Tristar readback/restore semantics, exclusive reservation across accessory/PHY drivers, a checked PMU ADC converter/cancel/drain lifecycle, coherent physical attachment/removal reporting, and matching lifetime management. Until those exist, this remains an unregistered deny-by-default source candidate. The independently developed combined USB/gauge producer, frontend analog assertions and PMU-current implementation are unchanged.

## Validation

`tests/test-experimental-wall-source.py` compiles the **complete actual candidate C**, including its header, with explicit fake kernel primitives/regmap and owner callbacks under host GCC `-Wall -Wextra -Werror -fsanitize=address,undefined`. No duplicated Python implementation stands in for C.

Nine groups pass, zero skip in the existing resource-limited offline container:

1. Default-disabled/missing owner/acquire failure/unproved restore: no regmap access; acquired reservations released.
2. Explicit synthetic positive classification for both orientations and the third-sample usb-charger path; never a hardware or current grant.
3. Every one of the successful two-sample path's regmap transactions individually fails, plus ADC/settle/validate/readback failures and out-of-range samples: zero result and release.
4. Generation changes at every transaction, changed orientation, absent USB, known status holds, invalid Tristar qualifiers: no published observation.
5. Restore failure: latched fault/generation, zero result and subsequent calls deny without retry or I/O.
6. **72 recorded original-code comparisons** from `original-classifier-oracle.json`, pinned by SHA256 in the test. These cover raw0/819/820/1819/1820/2368/2369/4095 grids, third-sample bounds and divider IDs. The optional independent verifier executes the actual original instructions to regenerate/check their results. Software arithmetic only.

7. Checked one-shot initialization, invalid resources, retained failed lease and generation wrap: no reset/retry or unowned access.
8. Malformed positive provider results: negative errors, unknown acquisition latch and no automatic retry.

Run portable GCC/sanitizer tests from the repository:

```
python3 tests/test-experimental-wall-source.py
```

They do not need firmware or Unicorn and have no private research imports. Original observations retain image hash, raw and scaled input samples, exact entry/base, mocked status, and output ordinals; no Apple code bytes are distributed. Separately verify them and the original failure/cache/detach/profile controls using an independently obtained matching official plaintext, not a normal build dependency:

```
python3 experiments/p105-wall-source/check-original-oracle.py --image /path/to/iBoot.13G36.dec
```

This optional verifier requires Python Unicorn and checks exact input SHA256/size; wrong input fails. All I2C, ADC helper, waits/logging and final zero-charge-policy setter are fake callbacks. It sends nothing to a device and includes no private imports.

ARM object validation used the existing isolated Linux6.12 copy with `ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- W=1`: PASS, zero warnings. It did not change normal Linux source/config/build. This candidate is not added to normal kernel Makefiles or Kconfig.

## Concrete original classifier evidence

At original iBoot `0x9ff20f3c`, actual string table `0x9ff42e78` identifies source ordinals0..6 as error/batt/firewire/usb host/usb brick/usb charger/external. Source type/ID are stored at `0x9ff4818c/190`. `ce4c→ce7c` reads PMU status0a..0f and checks first-byte08; absence clears ID and selects batt unless second status pass masks first-byte10 (firewire). External predicate `cf1c` is a zero-return stub in this image.

For USB presence, `1789c→17678` requests Tristar route2 and route1, each followed by delay2000 and ADC channel6 `c854→c88c`. ADC scaling is `raw*5000>>12`, error becomes zero in the original. Route0 follows each sample. Original qualifiers are `Tristar0c&84==80` and unequal06 bits2/3; mode2/mode1/mode0/mode4 produce the candidate's route table in both orientations. Original route writes ignore their return codes; the C candidate checks them.

Sample classes use <=2220/<=2890/>2890. With nonzero second scalar the ratio is unsigned `first*1000/second`; ratios1291..1505 select ID2,665..775 ID4, else `class(first)+3*class(second)+1`. If first>=1000 and second>=1000, ratio500..2000 selects usb brick, otherwise usb host. Lower samples attempt route4 and third sample after40000; scalar251..799 selects usb charger, else usb host. These values are original software units, not a physical divider/current certification.

Original type changes call `d224`, which invalidates current cache byte `9ff46a9d`. Nonzero cached brick ID bypasses new sampling until detach clears it. Original selected type4 ID profiles1..5 request500/1000/1000/2100/2400; unknown IDs and type5 fall back1000. This metadata is **not** copied into a Linux power budget. The actual zero-charge-policy verifier isolates profile selection without operating a charger.

All eight single route-write failure injections still let the original report brick4/ID4 with independently fake positive samples. The optional verifier preserves that negative control, cached-ID reuse, detach/reclassification, raw nonUSB status, failed status reads and selected input-profile checks. Linux candidate rejects I/O/restore errors and does not reuse cached ID.

Heartbeat root review: provider int results are strictly0/negative errno. Positive values returnEPROTO. Wall acquisition with a positive result cannot establish a safely releasable lease: it retains supplied identity, latches fault and issues no I/O/release/retry. Validated acquisitions still perform checked release after malformed validate/settle/ADC results; malformed release retains uncertainty. ADC post-start validation errors still stop with held lifetime and retain UNKNOWN; malformed hold results are retained asEPROTO independently of the first transfer error. Eight actual-C groups now pass for each component;72 original wall comparisons remain unchanged. These are inactive source-only changes, with no normal build or RAM-bundle modification.

Validator callback boundary: fault/generation is rechecked after the external owner validator returns. Invalidation inside the final ADC or bank validator now returnsESTALE with empty output and retained/quarantined state. Invalidation before a wall routing write prevents that transaction and still performs the existing checked release. Complete-C historical/fixed counterexamples were independently repeated. This closes the callback-spanning software gap; no atomic physical cutoff or shared supplier binding is claimed.
