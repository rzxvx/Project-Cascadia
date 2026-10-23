# Normalized multi-list PPN observations

`tools/nand/ppn_observations.py` builds an independent immutable view over
the [synthetic error-list decoder](p105-ppn-synthetic-decoder.md). It has no
file/device I/O, firmware execution, pointer dereference, MMIO, kernel API,
opcode or driver integration. Request records are normalized caller inputs;
the module does not parse an Apple C/R descriptor ABI or enable NAND writes.

Import from the repository root with
`from tools.nand.ppn_observations import RequestSnapshot, Observation, reconcile_observations`
and use `RequestEntry` from `tools.nand.ppn_error_list`. Direct module use with
`tools/nand` on the Python path remains supported.

## Explicit offline binding

`RequestSnapshot(generation, entries)` validates and copies the normalized
request entries. Generation is an exact Python u64 integer, including zero;
Boolean, negative and overflowing values are rejected. This width is a local
analysis convention, **not a recovered firmware token or hardware field**.
The snapshot retains entries in their supplied order, with original page,
opaque CE index, and raw status. Duplicate page/CE keys are rejected even
when no observation is supplied.

`Observation(snapshot, ordinal, payload, ce_index, context_status)` copies
one exact 512-byte scratch and validates it with the existing decoder.
Ordinal is another explicit synthetic u64 identifier. CE and raw context
status are required u8 values; no bus/die map or bit interpretation is added.
Inputs may be mutable lists/bytearrays while being prepared, but must not be
concurrently mutated during copying. Earlier copied observations survive
subsequent scratch reuse and changes to caller-owned request containers.

`reconcile_observations(expected_snapshot, observations)` copies/revalidates
all observations and compares their generation and complete request tuple
against the expected snapshot. A changed raw status, request order, page/CE
or number of entries is a different snapshot, even with the same generation.
Different generations, foreign requests, duplicate ordinals, malformed data,
unknown result pages and ambiguous keys raise `DecodeError`, without returning
a partial view. Manually modified cached decoder results are not trusted.

Equal payloads with **distinct** ordinals remain separate observations. The
module cannot tell whether a caller mislabeled a replay as a new observation;
synthetic binding does not authenticate live AP/IOP events, establish buffer
ownership, or prevent real firmware stale-event races.

## Raw observations and unknown absence

The returned frozen `ObservationView` contains the copied snapshot, original
observation order, and `PageObservations` in request order. Each page retains
a tuple of `EntryObservation` records with ordinal, within-list index, opaque
CE, raw context status and the original request status. Overlaps between
different lists are preserved individually, including contradictory-looking
raw labels. Statuses are never ORed, reclassified, cleared or merged.

`PageObservations.presence` is exactly `listed` when a captured list contains
the key and otherwise `unknown`. An empty or partial list never classifies
its absent requested pages as successful, complete, durable or safe to retry.
Even presence in all observations says nothing about completed programming,
retirement, acknowledgement or power-cut recovery. No aggregate success or
durability field exists.

The analysis does not require three observations, particular raw categories,
disjoint page sets, or a specific order. Those would be protocol assumptions
not established by the
[completion/address evidence](p105-ppn-completion-boundaries.md). No firmware
query or recovery write is performed to fill missing observations.

## Validation

Run `python3 -B tests/test-ppn-observations.py -v`. The 22 unittest cases
cover overlapping lists, partial/empty absence, scratch reuse, raw status
0..255, opaque CE255, exact count/length boundaries, missing/foreign CE,
duplicate request/result/ordinal data, stale/foreign generation, changed
snapshot content/order, strict integer types and immutable copied results.
Five hundred deterministic random fixtures are compared against an independent
linear-search tuple oracle; another 240 adversarial binding fixtures exercise
both valid and rejected request/generation/ordinal combinations.

The host and existing Ubuntu22 build image passed these fixtures without
firmware input, network or device mounts. Driver behavior, DMA lifetime,
firmware generation semantics, NAND write order, ECC, power-cut integrity and
iOS preservation remain outside these tests' authority.
