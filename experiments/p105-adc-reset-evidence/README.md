# Narrow search for a real D1946 ADC reset / cancellation owner

Source-only offline evidence,2026-10-12. This follows the separately frozen
ADC IRQ/provider group without changing or rerunning its successful tests.
No runtime code, device query, PMU access, system/network operation, build,
commit or shared-tree change was performed.16 new bounded original-instruction
fixtures pass (6 alternate-helper paths per image,4 additional12H config paths).
No physical quiescence witness is generated.

## Actual additional control setter (12H321)

Actual D1946 vptr `80ed3208`, slot3b0 is `80ecdcb9`. The generic base wrapper
`80ec8c1c` loads that slot at `80ec8c2a` and passes it to Gate.runAction slot70
with its caller's two parameters. The native setter `80ecdcb8` waits until
software ADC state `PMU+94` is0 or3. It updates cached byte `PMU+98` as
`(cache & ~r2) | r1`, sends it to full logical register43, then unconditionally
returns0. There is no readback, checked error, event ACK or physical busy test.

New real-instruction controls use explicitly synthetic initial cache/43=a5,
r1=0,r2=20 (a bit pattern fixture, not a programming recommendation):

|Fixture|Native result|Software cache|Fake bus register43|
|---|---|---|---|
|State0|0|85|85|
|State3|0|85|85|
|State0,failed write|0|85|a5|
|State1|Stops at real commandSleep call|a5|a5,no I/O|

The third fixture proves a successful setter return cannot certify delivered
control. The fourth stops before the indefinite fake command-gate wait rather
than fabricating a completion. In13G36 the corresponding base config wrapper
`81141144` dispatches actual slot3b0 to `81149b9c`, which returns unsupported
E00002C7. That path is not a new hardware reset primitive.

## Independent alternate conversion path, not normal43/44 cancellation

Normal acquisition qualifier bit10000 dispatches to an alternate helper:
12H `80ecd9ee ->80ecdc24` with channel low6 bits;13G branch at `811472c6`
reaches call `8114744c..` to helper `81147508`. These are concrete native
logical operands15d/15e; no physical page/bank/hardware-capability interpretation
is inferred from their high byte. The parent bank0 allowlist is not expanded.

Both actual helpers execute the same bounded sequence for channel6:

1. Write native operand15d = `(channel &3f)|80`, checked return.
2. Call delay helper with argument1 (delay and time are synthetic; no physical
   settle or hardware deadline proof is asserted).
3. Read15d, checked return. If the returned byte has bit80 clear, read15e,count2
   and combine little-endian; sample-read error is propagated.
4. If bit80 remains set, write15d=0, then returnE00002D6. That cleanup write's
   error is ignored. There is no later status/readback or completion drain.

Six new original-instruction fixtures per build:

- Synthetic status06/sampleabcd yields success/outputcdab. There is no final
  cleanup write on this success path.
- Start write error, status read error and sample read error propagate the
  syntheticE00002C0, with output sentinel untouched.
- Synthetic busy status86 writes15d=0 and returnsE00002D6.
- Synthetic busy status86 with failed zero-write still returnsE00002D6. The
  fake transport failure is visible in the log but not distinguished by return.

The alternative helper is genuine additional controller code with some checked
I/O, but it neither acknowledges ADC event02.5 nor accesses normal control43.
Its callback/IRQ relation and physical reset effects are not established. A
successful zero-write cannot be elevated to the normal43 engine's cancel
postcondition. A failed zero-write is an explicit source-backed counterexample
to treating the helper's return as a checked stop.

## Concrete reset-provider exclusion

Bounded string/PIC search found `function-panic_reset` in both base PMU start
and D1946 start, plus `function-external_shutdown` and `_shutdownGated`.
The13G native D1946 startup loads panic-reset string at `811454d2/811454d4`,
gets its provider via actual slot1cc, resolves its ARMFunction through helper
`8114bd6c`, and stores the result at PMU+a0. The external-shutdown lookup at
`811454bc..811454c4` stores PMU+9c. These are callbacks, not ADC reset-register
writes. The base startup at `811409a4..811409ba` queries the panic-reset property
and stores its presence flag at+8c.

The authenticated13G ADT maps the panic-reset callback to `/device-tree/arm-io/wdt`, dispatcher integer
57445472 (`WDTr`), argument0. That actual provider is compatible
wdt,s5l8940x /s5l8930x /s5l8920x, native register cells0f103020 /16,
AIC interrupt4. The PMU's name contains reset, but this native function binding
points to the SoC watchdog node, not an identified D1946 ADC reset operation.
The ADT argument is not itself a register value. `panic-reset-provider.json`
records the normalized binding/parser hash; no peripheral operation is run.

Actual native `_shutdownGated` source paths12H80ecec80 and13G8114859c perform
power/GPIO handling and read-modify-write native1b, then may enter an infinite
sleep loop.12H's RMW tail80ecedda..80ecee02 ORs1 into1b; another platform path
80ecce9e..80eccec8 ORs2. Neither bounded path accesses43/44 or confirms an ADC
abort/readback/drain. These values' analogue/reset meaning is deliberately not
borrowed from other Dialog chips. A global power transition is not an identified
non-destructive ADC-cancel protocol.

## Explicit positive and negative search coverage

Static candidate scan covered12H base80ec8000..80ecb000 and D1946 native text
80ecb570+673c;13G base81140000..81144000 and D1946 text81144e18+6d24.
Base regions also include literal/data areas: candidate instruction hints there
must be manually verified. The scan is a linear Thumb search, not a complete
interprocedural proof. It enumerated immediate R1 operands43/44/1b/1c/15d/15e,
selected direct helper calls and config virtual slot3b0. Newly traced paths are
above. Computed general register access/unknown runtime callbacks are not ruled
out merely because no constant match exists.

Within the verified D1946 startup13G81144f58..81145514, native writes include
12..1a masks,9f modification,1c bit01, optional3b and d3 controls; ADC completion
registration/unmask is software/event setup, not a control43 reset. GPIO default
and calibration properties are handled separately. No source-backed ADC abort
postcondition was found in the bounded normal init path. Earlier authenticated
boot initialization/table traces are consulted as existing evidence, not rerun;
those tables' unknown analogue semantics cannot be converted into a reset or
CV/protection assertion. There is no readback/late-IRQ proof established here.

The remaining physical requirement is specific: an authoritative statement or
permissioned measured sequence showing that a checked D1946 ADC cancellation
prevents every later completion02.5 and leaves normal43/44 belonging to the next
request. This must also be matched with the already proven exclusive software
IRQ/cache/work drain. Neither elapsed delay, zero event bytes, a control-byte
write, global-reset name, different logical operand, nor a mock status value
supplies that requirement. Therefore no additional runtime guard/reset/replay
implementation is added.

## Existing experiment comparison

This evidence does not require an analogue reset replay or a new deny gate.
The existing `experiments/p105-parent-adc-owner/p105-adc6-poll.c` cleanup path
already checks its write43=0, reads43 back, records cleanup errors and retains
unknown state on any uncertain result. Its explicit comment distinguishes
checked stop from physical cancellation/late-completion quiescence. The current
owner consumes a physical witness once; no runtime producer supplies one.
The newly examined native config/alternate paths therefore justify keeping that
existing distinction. They do not identify a new existing-code regression or
support allowing15d/15e through the normal parent bank0 allowlist.

## Optional reproduction and provenance

This package adds no normal test-suite entry point or active driver. The authored
standalone tool requires an already available Unicorn runtime. It reads only
user-supplied local images. Both exact SHA256 values are checked before native
emulation; offsets cannot be reused with another image. Image extraction,
download, installation and device accesses are outside this tool.

From the eventual repository root (or use this proposal's directory):

```sh
python3 experiments/p105-adc-reset-evidence/emulate-adc-reset-candidates.py 12 path/to/kernelcache.12H321.macho
python3 experiments/p105-adc-reset-evidence/emulate-adc-reset-candidates.py 13 path/to/kernelcache.13G36.macho
```

Existing build-image use can mount the evidence directory and supplied firmware
read-only with network disabled. No privileges are required. The tool executes
6 fresh alternate-helper fixtures per image and4 additional12H config fixtures,
16 total; its assertions verify native results. Existing successful IRQ/cache
and runtime suites are not repeated. There is no Linux-C addition/ARM compile.

`source-provenance.json` pins the original frozen research manifest, original
artifact hashes, exact input identities and selected authored tool/output files.
`original12-results.json` / `original13-results.json` contain only synthetic
PMU/gate/sample values and addresses; no Apple instruction bytes.
`panic-reset-provider.json` is normalized ADT evidence, not firmware bytes or
a programming recipe. Both binary images remain outside the package. The
primary provenance is the official IPSW extraction already verified by the
project; no unauthenticated Dialog datasheet assumptions are added.
