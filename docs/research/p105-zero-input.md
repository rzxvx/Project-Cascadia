# p105 actual zero-input apply and error boundaries

This is a bounded offline follow-up to [source notifications](p105-source-events.md)
and [source backoff](p105-source-backoff.md). It executes the actual generic input
apply routine and D1946 setter when the effective input budget is already zero.
It does **not** establish physical cable removal → zero budget, battery charger
shutdown, or complete analog fault protection. No connected device is accessed.

## Pinned input and scope

The local Apple iPad2,5 iOS 8.4.1 / 12H321 kernelcache SHA256 is
`db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c`.
The official IPSW provenance is recorded in [charging research](p105-charging.md).
No Apple firmware bytes are included in the tool or tests. The command-line entry
point and test loader verify the hash before using fixed addresses.

Actual instructions executed:

- AppleARMPMUCharger input apply **0x804c479c** through its return;
- source actual vtable **0x80ed3c68**, slot **+0x60** → D1946 input setter
  **0x80ed086c**, its current-code arithmetic **0x80ed08c8**, and low-current
  control helper **0x80ed09d4**;
- surrounding generic bookkeeping and return/error branches.

Synthetic inputs are source-reference effective budget **+0x0c = 0**, previous
current **+0x10 = 0..3262**, zero cap **+0x18**, absent calibration, zero logging
flag, and zero-initialized remaining fake fields/stack. PMU RAM starts with
`1b=a1`, `1c=13`, `3d=54`. Those values deliberately expose preservation and
failure differences; they are not observations of the real iPad.

Mocks are PMU read/write I/O; absolute time at **0x804c9d34** and its elapsed
conversion **0x804c9bf4**; uptime **0x804c9c74**; interval conversion
**0x804c9c84**; owner/error names and logging; source virtual slot **+0x50**.
The latter is observed with argument zero and returns zero in the fixture; its
real hardware consequences are outside this experiment. The inherited generic
notification stub is not reached by these fixtures. No timer delivery or real
clock progression is modeled. The recorded 500 / 1,000,000 interval request is
an API request; returned 500 ticks and uptime 100 are synthetic values.

## Zero budget with previous nonzero current

At **0x804c47e2..4812**, the actual routine compares the effective budget with
100 and the previous current with zero. The zero-budget/nonzero-current case
calls the source slot +0x60 with request **0**. It ignores that first call's
return value at **0x804c4814** and continues with selected current zero.

The bounded path then reaches **0x804c4a78..4a8a**, calls the same actual setter
with request zero a second time, and checks its result at **0x804c4a8c..4a92**.
On success, **0x804c4ade..4b10** stores current zero, negative previous-current
as delta, and uptime/deadline. This fixture subsequently observes the source
slot +0x50 call with argument zero, then returns. On failure, the actual
error-reporting branch **0x804c4a94..4abe** runs and exits without updating that
current/time/deadline bookkeeping or calling source slot +0x50.

Each real D1946 zero setter produces these **fake** transfers:

1. Read register `0x3d` (one byte).
2. Read `0x1b..0x1c` (two bytes).
3. Write those two bytes with `0x1b` bit `0x04` and `0x1c` bit `0x80` set.
4. Write current code zero to `0x3d`.

D1946 **0x80ed088c** does not check the first read result.
**0x80ed08a4** does not check the low-current helper's result. Source cached code
**+0x2a** is updated at **0x80ed08a8**, before the `0x3d` write. Its returned
status is the final `0x3d` write's status. These observations describe control
flow and byte masks; the physical meaning of the mode bits remains unverified.

## Error and cache limits

The ten tests include independent failures at every one of the eight transfers,
both final-current writes failing, and boundary previous-current fixtures.

| Synthetic failure | Generic stored current | Fake register 0x3d | Consequence |
| --- | ---: | ---: | --- |
| None, previous 500 | 0 | 0 | Both requests succeed; delta −500; fake deadline 600 |
| First `0x3d` write | 0 | 0 | First error ignored; second write retries successfully |
| Second `0x3d` write | 500 | 0 | First write succeeded, but generic cache remains 500 |
| Both `0x3d` writes | 500 | 0x54 | Generic cache stays 500; source code cache is already zero |
| Previous already zero | 0 | 0x54 | No PMU transactions or setter calls occur |

The last row is particularly important: generic cached zero alone cannot prove
that register `0x3d` was recently verified, nor that the physical input is safe.
Similarly, cached source code zero can coexist with a failed write. Ignored read
errors can use stack contents; zero-initialized synthetic RAM is **not** a claim
that failed live reads reliably produce zero. Fake RAM does not implement
read-to-clear side effects, electrical current, temperature, or battery safety.

Nonzero effective budgets are rejected by this tool. Even below 100, they can
reach additional delayed-adjustment timer branches requiring a different
fixture; this group does not silently treat them as the tested zero case.

## Reproduce

```sh
python3 tools/power/emulate-zero-input.py build/firmware/kernelcache.12H321.macho
python3 tests/test-zero-input-emulation.py
```

Unicorn 2.1.4 is available in the reviewed local build image. All ten tests pass
with no skip; input guards reject booleans, out-of-range currents and malformed
failure ordinals. Each experiment is single-use. No runtime driver changes are
part of this group.

## Remaining source-removal chain

A physical detach → detection/notification → source classification → generic
zero input **and charge-current** request chain still needs complete owner and
workloop evidence. Actual source-classification **0x804c75b8** calls
**0x804c74a8** and **0x804c829c** after property/policy updates; this group does
not execute those preceding transitions. Generic charge distribution
**0x804c437c** invokes helper **0x804c46e4**, source slot +0x70 → **0x80ed0768**.
The helper's optional actual-current output is not written by that D1946 method;
nonzero-budget distribution accounting therefore requires further proof.
The zero-budget helper bypasses that accounting read, but its full preceding
source-removal policy has not yet been emulated here.

These gaps, target voltage, termination and hardware fault protection remain
conditions for implementing an active Linux charger. The offline proof does
not authorize PMU writes or suggest charger defaults.
